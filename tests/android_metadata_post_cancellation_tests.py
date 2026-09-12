from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from find_bms_transport_extract import function


ROOT = Path(__file__).resolve().parents[1]


class AndroidMetadataPostCancellationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if sys.platform not in ("darwin", "linux"):
            raise unittest.SkipTest("Unsupported host for JNI fixture: " + sys.platform)
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and sys.platform == "darwin" and Path("/usr/libexec/java_home").is_file():
            try:
                java_home = subprocess.check_output(
                    ["/usr/libexec/java_home", "-v", "17"], text=True,
                    stderr=subprocess.DEVNULL).strip()
            except (OSError, subprocess.CalledProcessError):
                pass
        if not java_home:
            javac = shutil.which("javac")
            if not javac:
                raise unittest.SkipTest("JDK unavailable: set JAVA_HOME or put javac on PATH")
            java_home = Path(javac).resolve().parent.parent
        java_home = Path(java_home).expanduser().resolve()
        if not all(shutil.which(str(java_home / "bin" / name)) for name in ("java", "javac")):
            raise unittest.SkipTest("JDK java/javac executables unavailable under " + str(java_home))
        if not all((java_home / "include" / name).is_file()
                   for name in ("jni.h", sys.platform + "/jni_md.h")):
            raise unittest.SkipTest("JDK JNI headers unavailable under " + str(java_home))
        compiler_names = [os.environ["CXX"]] if os.environ.get("CXX") else ["clang++", "g++", "c++"]
        compiler = next((path for name in compiler_names if (path := shutil.which(name))), None)
        if not compiler:
            raise unittest.SkipTest("C++ compiler unavailable: set CXX or install clang++/g++")
        cls.java = str(java_home / "bin/java")
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        output = Path(cls.output.name)
        activity = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java").read_text()
        signatures = ["public String postUrlText(",
                      "private HttpURLConnection openHttpConnection(String",
                      "private HttpURLConnection openHttpConnection(\n",
                      "private boolean isRedirectStatus(",
                      "private String readTextResponse("]
        methods = "\n\n".join(function(activity, signature) for signature in signatures)
        for name in ("ERROR_PREFIX", "MAX_TEXT_DOWNLOAD_BYTES"):
            start = activity.index("    private static final", activity.index(name) - 40)
            methods += "\n" + activity[start:activity.index(";", start) + 1]
        fixture = (ROOT / "tests/java/AndroidMetadataPostActivityFixture.java").read_text()
        generated = output / "AsoBMaShowActivity.java"
        generated.write_text(fixture.replace("ACTIVITY_METHODS", methods))
        subprocess.run([str(java_home / "bin/javac"), "-d", str(output),
                        str(generated)], check=True)

        native = (ROOT / "src/AndroidNatives.cpp").read_text()
        start = native.index("struct AndroidDownloadProgressBridge {")
        stop = native.index("std::mutex gAndroidFolderPickerMutex;", start)
        pieces = [native[start:stop]]
        for signature in ("void appendUtf8(", "std::string jstringToUtf8(",
                          "jstring utf8ToJString(", "bool clearPendingJavaException(",
                          "jlong registerAndroidDownloadProgressBridge(",
                          "void unregisterAndroidDownloadProgressBridge(",
                          "AndroidDownloadProgressBridge *androidDownloadProgressBridge(",
                          "std::string callActivityStringMethod(",
                          "std::string callActivityStringMethodLong(",
                          "bool parseBridgeResult(", "bool PostURLTextAndroid("):
            pieces.append(function(native, signature))
        callback = "Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDownloadUrlTextCheckpoint("
        pieces.append('extern "C" JNIEXPORT jboolean JNICALL\n' + function(native, callback))
        transport = (ROOT / "src/bms_search/DownloadSupport.cpp").read_text()
        signature = "std::optional<std::string> postUrlText("
        pieces.append(function(transport[transport.rindex(signature):], signature))
        (output / "android_metadata_post_methods.inc").write_text("\n\n".join(pieces))
        cls.library = output / ("libpost.dylib" if sys.platform == "darwin" else "libpost.so")
        subprocess.run([compiler, "-std=c++20", "-shared", "-fPIC",
                        "-pthread", "-DTARGET_OS_ANDROID=1", "-I", str(ROOT / "src"),
                        "-I", str(output), "-I", str(java_home / "include"), "-I",
                        str(java_home / "include" / ("darwin" if sys.platform == "darwin" else "linux")),
                        str(ROOT / "tests/android_metadata_post_bridge_fixture.cpp"),
                        "-o", str(cls.library)], check=True)

    def run_scenario(self, scenario):
        result = subprocess.run([self.java, "-Xcheck:jni", "-cp", self.output.name,
                                 "com.snurhythm.asobmashow.AsoBMaShowActivity",
                                 str(self.library), scenario], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="", flush=True)

    def test_cancel_stalled_post(self):
        for phase in ("output", "headers", "body", "redirect"):
            with self.subTest(phase=phase):
                self.run_scenario(phase)

    def test_cancel_before_network_engine_exists(self):
        for scenario in ("preconnect", "bridge-preconnect"):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_response_security_and_cleanup(self):
        for scenario in ("success", "uncancellable", "pre-cancelled", "http-error",
                         "io-error", "too-large", "redirect-302", "redirect-307",
                         "downgrade", "bad-scheme", "redirect-scheme", "redirect-limit"):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_native_bridge_cancellation_errors_and_optional_checkpoint(self):
        for scenario in ("output", "headers", "body", "redirect", "pre-cancelled",
                         "uncancellable", "http-error", "io-error"):
            with self.subTest(scenario=scenario):
                self.run_scenario("bridge-" + scenario)


@unittest.skipUnless(sys.platform in ("darwin", "linux"),
                     "Unsupported host for JNI fixture toolchain tests: " + sys.platform)
class AndroidMetadataPostToolchainTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.jdk = self.root / "jdk"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for name in ("java", "javac"):
            executable = self.jdk / "bin" / name
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.touch()
            executable.chmod(0o755)
        for name in ("jni.h", "darwin/jni_md.h", "linux/jni_md.h"):
            header = self.jdk / "include" / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.touch()
        self.compiler = self.bin / "g++"
        self.compiler.touch()
        self.compiler.chmod(0o755)
        (self.bin / "javac").symlink_to(self.jdk / "bin/javac")

    def setup_fixture(self, platform, environment, resolver=None):
        class Fixture(AndroidMetadataPostCancellationTests):
            pass

        self.addCleanup(Fixture.doClassCleanups)
        with mock.patch.object(sys, "platform", platform), \
                mock.patch.dict(os.environ, environment, clear=True), \
                mock.patch.object(subprocess, "check_output", side_effect=resolver) as lookup, \
                mock.patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0)) as run:
            try:
                Fixture.setUpClass()
            finally:
                if hasattr(Fixture, "output"):
                    Fixture.output.cleanup()
        return lookup, [call.args[0] for call in run.call_args_list]

    def test_linux_resolves_jdk_from_javac_symlink(self):
        lookup, commands = self.setup_fixture(
            "linux", {"PATH": str(self.bin)},
            resolver=FileNotFoundError("macOS java_home is unavailable"))
        lookup.assert_not_called()
        self.assertEqual(commands[0][0], str(self.jdk / "bin/javac"))
        self.assertEqual(commands[1][0], str(self.compiler))
        self.assertIn(str(self.jdk / "include/linux"), commands[1])

    def test_java_home_takes_precedence_and_honors_cxx(self):
        lookup, commands = self.setup_fixture(
            "darwin", {"JAVA_HOME": str(self.jdk), "CXX": str(self.compiler), "PATH": ""})
        lookup.assert_not_called()
        self.assertEqual(commands[0][0], str(self.jdk / "bin/javac"))
        self.assertEqual(commands[1][0], str(self.compiler))

    def test_macos_prefers_java_17_when_resolver_available(self):
        def resolver(*args, **kwargs):
            return str(self.jdk) + "\n"

        with mock.patch.object(Path, "is_file", return_value=True):
            lookup, commands = self.setup_fixture(
                "darwin", {"PATH": str(self.bin)}, resolver=resolver)
        self.assertEqual(lookup.call_args.args[0], ["/usr/libexec/java_home", "-v", "17"])
        self.assertEqual(commands[0][0], str(self.jdk / "bin/javac"))

    def test_macos_falls_back_when_java_17_unavailable(self):
        _, commands = self.setup_fixture(
            "darwin", {"PATH": str(self.bin)},
            resolver=subprocess.CalledProcessError(1, "java_home"))
        self.assertEqual(commands[0][0], str(self.jdk / "bin/javac"))

    def test_missing_jdk_skips_explicitly(self):
        (self.bin / "javac").unlink()
        with self.assertRaisesRegex(unittest.SkipTest, "JDK"):
            self.setup_fixture("linux", {"PATH": str(self.bin)})

    def test_missing_jni_headers_skips_explicitly(self):
        (self.jdk / "include/linux/jni_md.h").unlink()
        with self.assertRaisesRegex(unittest.SkipTest, "JNI"):
            self.setup_fixture("linux", {"JAVA_HOME": str(self.jdk), "PATH": str(self.bin)})

    def test_missing_compiler_skips_explicitly(self):
        with self.assertRaisesRegex(unittest.SkipTest, "C\\+\\+ compiler"):
            self.setup_fixture("linux", {"JAVA_HOME": str(self.jdk), "PATH": ""})

    def test_unsupported_host_skips_explicitly(self):
        with self.assertRaisesRegex(unittest.SkipTest, "Unsupported host.*win32"):
            self.setup_fixture("win32", {"JAVA_HOME": str(self.jdk), "PATH": str(self.bin)})


if __name__ == "__main__":
    unittest.main()
