from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
APPLICATION = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowApplication.java"


class AndroidTemporaryStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.output.cleanup)
        stubs = {
            "android/app/Application.java": """
package android.app;
import java.io.File;
public class Application {
    public static final File CACHE = new File("/data/user/0/com.snurhythm.asobmashow/cache");
    public void onCreate() {}
    public File getCacheDir() { return CACHE; }
}
""",
            "android/util/Log.java": """
package android.util;
public class Log {
    public static Throwable lastError;
    public static int e(String tag, String message, Throwable error) {
        lastError = error;
        return 0;
    }
}
""",
            "com/snurhythm/asobmashow/AsoBMaShowDocumentsProvider.java": """
package com.snurhythm.asobmashow;
import android.app.Application;
import android.system.Os;
import java.io.IOException;
public class AsoBMaShowDocumentsProvider {
    public static int calls;
    public static boolean privateEnvironmentReady;
    public static IOException failure;
    public static void initializeDocuments(Application application) throws IOException {
        calls++;
        String cache = application.getCacheDir().getAbsolutePath();
        privateEnvironmentReady = cache.equals(Os.getenv("TMPDIR"))
                && cache.equals(Os.getenv("SQLITE_TMPDIR"));
        if (failure != null) throw failure;
    }
}
""",
            "android/system/ErrnoException.java": """
package android.system;
public class ErrnoException extends Exception {
    public ErrnoException(String message) { super(message); }
}
""",
            "android/system/Os.java": """
package android.system;
import java.util.HashMap;
import java.util.Map;
public class Os {
    public static final Map<String, String> environment = new HashMap<>();
    public static String failVariable;
    public static final ErrnoException failure = new ErrnoException("setenv failed");
    public static String getenv(String name) { return environment.get(name); }
    public static void setenv(String name, String value, boolean overwrite) throws ErrnoException {
        if (name.equals(failVariable)) throw failure;
        if (overwrite || !environment.containsKey(name)) environment.put(name, value);
    }
}
""",
        }
        sources = []
        for relative, content in stubs.items():
            path = Path(cls.output.name) / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
            sources.append(str(path))
        subprocess.run([javac, "-d", cls.output.name, *sources, str(APPLICATION),
                        str(ROOT / "tests/java/AndroidTemporaryStorageFixture.java")],
                       check=True)

    def run_scenario(self, scenario):
        subprocess.run([self.java, "-cp", self.output.name,
                        "com.snurhythm.asobmashow.AndroidTemporaryStorageFixture",
                        scenario], check=True, timeout=15)

    def test_application_initializes_native_and_sqlite_temporary_storage(self):
        self.run_scenario("fresh")

    def test_application_replaces_inherited_temporary_storage(self):
        self.run_scenario("inherited")

    def test_documents_failure_keeps_private_temporary_storage_ready(self):
        self.run_scenario("documents-failure")

    def test_environment_failure_prevents_native_startup(self):
        for variable in ("TMPDIR", "SQLITE_TMPDIR"):
            with self.subTest(variable=variable):
                self.run_scenario(variable)


if __name__ == "__main__":
    unittest.main()
