#!/usr/bin/env python3
"""Exercise cache identity and process locking without installing dependencies."""

import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[1] / "cmake/AndroidDependencies.cmake"


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "CMake/Git required")
class AndroidDependencyCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.vcpkg = self.root / "vcpkg"
        self.ndk = self.root / "ndk"
        for name, contents in {
            "vcpkg/scripts/buildsystems/vcpkg.cmake": "# fixture toolchain\n",
            "vcpkg/triplets/arm64-osx.cmake": "# host triplet\n",
            "vcpkg/vcpkg": "fixture executable\n",
            "ndk/source.properties": "Pkg.Revision = 28.2.13676358\n",
            "ndk/build/cmake/android.toolchain.cmake": "# fixture chainload\n",
            "vcpkg.json": '{"name":"fixture","version":"1"}\n',
            "vcpkg-triplets/arm64-android.cmake": "set(VCPKG_TARGET_ARCHITECTURE arm64)\n",
            "vcpkg-overlays/example/portfile.cmake": "# example port\n",
        }.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        subprocess.run(["git", "init", "-q", str(self.vcpkg)], check=True)
        subprocess.run(["git", "-C", str(self.vcpkg), "add", "."], check=True)
        subprocess.run(["git", "-C", str(self.vcpkg), "-c", "user.name=Fixture",
                        "-c", "user.email=fixture@example.invalid", "commit", "-qm", "initial"],
                       check=True)
        (self.root / "CMakeLists.txt").write_text(f"""
cmake_minimum_required(VERSION 3.22)
include("{MODULE.as_posix()}")
asobmashow_prepare_android_dependencies()
file(WRITE "${{CMAKE_BINARY_DIR}}/selected.txt" "${{VCPKG_INSTALLED_DIR}}")
if(FAIL_AFTER_LOCK)
    message(FATAL_ERROR "fixture configure failure")
endif()
if(HOLD_LOCK)
    file(WRITE "${{CMAKE_BINARY_DIR}}/locked.txt" "locked")
    while(NOT EXISTS "${{CMAKE_BINARY_DIR}}/release.txt")
        execute_process(COMMAND "${{CMAKE_COMMAND}}" -E sleep 0.05)
    endwhile()
endif()
project(cache_fixture LANGUAGES NONE)
asobmashow_release_android_dependencies()
file(WRITE "${{CMAKE_BINARY_DIR}}/found.txt" "${{Fixture_PACKAGE_DIR}}")
""")

    def command(self, build="build", *extra):
        return ["cmake", "-G", "Ninja", "-S", str(self.root), "-B", str(self.root / build),
                f"-DCMAKE_TOOLCHAIN_FILE={self.vcpkg}/scripts/buildsystems/vcpkg.cmake",
                f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={self.ndk}/build/cmake/android.toolchain.cmake",
                "-DVCPKG_TARGET_TRIPLET=arm64-android", "-DVCPKG_HOST_TRIPLET=arm64-osx",
                f"-DVCPKG_OVERLAY_TRIPLETS={self.root}/vcpkg-triplets",
                f"-DVCPKG_OVERLAY_PORTS={self.root}/vcpkg-overlays",
                "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28",
                "-DANDROID_STL=c++_shared", *extra]

    def configure(self, build="build", *extra, environment=None):
        result = subprocess.run(self.command(build, *extra), text=True, capture_output=True,
                                env=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (self.root / build / "selected.txt").read_text()

    def test_equivalent_paths_and_build_types_share_installation(self):
        debug = self.configure("debug", "-DCMAKE_BUILD_TYPE=Debug")
        alias = self.root / "ndk-alias"
        alias.symlink_to(self.ndk, target_is_directory=True)
        release = self.configure("release", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                                 "-DASOBMASHOW_APPLICATION_VERSION=2.0",
                                 f"-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={alias}//build/cmake/android.toolchain.cmake")
        self.assertEqual(debug, release)
        self.assertTrue(Path(debug).is_relative_to(self.root / ".cache/android-vcpkg"))

    def test_unchanged_build_does_not_reconfigure(self):
        self.configure()
        for _ in range(2):
            result = subprocess.run(["cmake", "--build", str(self.root / "build")],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("Re-running CMake", result.stdout)
            self.assertIn("no work to do", result.stdout)

    def test_adding_registry_configuration_invalidates_on_build(self):
        previous = self.configure()
        (self.root / "vcpkg-configuration.json").write_text('{"registries": []}\n')
        result = subprocess.run(["cmake", "--build", str(self.root / "build")],
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotEqual(previous, (self.root / "build/selected.txt").read_text())

    def test_dependency_changes_select_new_installation_and_clear_stale_find_results(self):
        previous = self.configure()
        for relative in ("vcpkg.json", "vcpkg-overlays/example/portfile.cmake",
                         "vcpkg-triplets/arm64-android.cmake", "ndk/source.properties",
                         "ndk/build/cmake/android.toolchain.cmake",
                         "vcpkg/scripts/buildsystems/vcpkg.cmake"):
            with self.subTest(input=relative):
                path = self.root / relative
                path.write_text(path.read_text() + "\n")
                current = self.configure("build", f"-DFixture_PACKAGE_DIR={previous}/share/example")
                self.assertNotEqual(previous, current)
                self.assertEqual((self.root / "build/found.txt").read_text(), "")
                previous = current

    def test_missing_dependency_input_fails_before_selecting_cache(self):
        (self.ndk / "source.properties").unlink()
        result = subprocess.run(self.command(), text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source.properties", result.stderr)
        self.assertFalse((self.root / "build/selected.txt").exists())

    def test_environment_and_registry_overlay_contents_invalidate_identity(self):
        overlay = self.root / "extra-ports"
        overlay.mkdir()
        port = overlay / "portfile.cmake"
        port.write_text("# initial\n")
        for source in ("environment", "registry", "embedded"):
            with self.subTest(source=source):
                environment = dict(os.environ)
                configuration = {"overlay-ports": ["extra-ports"]}
                (self.root / "vcpkg-configuration.json").unlink(missing_ok=True)
                (self.root / "vcpkg.json").write_text('{"name":"fixture","version":"1"}\n')
                if source == "environment":
                    environment["VCPKG_OVERLAY_PORTS"] = str(overlay)
                elif source == "registry":
                    (self.root / "vcpkg-configuration.json").write_text(json.dumps(configuration))
                else:
                    (self.root / "vcpkg.json").write_text(json.dumps({
                        "name": "fixture", "version": "1", "vcpkg-configuration": configuration}))
                previous = self.configure(source, environment=environment)
                port.write_text(port.read_text() + "# changed\n")
                current = self.configure(source, environment=environment)
                self.assertNotEqual(previous, current)

    def test_installation_lock_times_out_and_releases_after_failure(self):
        with tempfile.TemporaryFile(mode="w+") as output:
            process = subprocess.Popen(self.command("holder", "-DHOLD_LOCK=ON"),
                                       stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 20
                while not (self.root / "holder/locked.txt").exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        output.seek(0)
                        self.fail("lock holder failed: " + output.read())
                    time.sleep(0.02)
                blocked = subprocess.run(self.command("blocked", "-DASOBMASHOW_DEPENDENCY_LOCK_TIMEOUT=0"),
                                         text=True, capture_output=True)
                self.assertNotEqual(blocked.returncode, 0)
                self.assertIn("dependency lock", blocked.stderr)
                self.assertIn(".cache/android-vcpkg", blocked.stderr)
            finally:
                (self.root / "holder/release.txt").write_text("release")
                process.wait(timeout=20)
        failed = subprocess.run(self.command("failed", "-DFAIL_AFTER_LOCK=ON"),
                                text=True, capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn("fixture configure failure", failed.stderr)
        self.configure("retry", "-DASOBMASHOW_DEPENDENCY_LOCK_TIMEOUT=0")


if __name__ == "__main__":
    unittest.main()
