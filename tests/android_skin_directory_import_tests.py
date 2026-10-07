import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AndroidSkinDirectoryImportTests(unittest.TestCase):
    def test_folder_import_engine(self):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and sys.platform == "darwin":
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        java = str(Path(java_home) / "bin/java") if java_home else "java"
        source = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"
        with tempfile.TemporaryDirectory(prefix="folder-import-tests-") as output:
            subprocess.run([javac, "-d", output,
                            str(source / "ImportCopyWorkers.java"),
                            str(source / "SkinDirectoryImport.java"),
                            str(ROOT / "tests/java/SkinDirectoryImportTests.java")], check=True)
            subprocess.run([java, "-cp", output,
                            "com.snurhythm.asobmashow.SkinDirectoryImportTests"], check=True)



if __name__ == "__main__":
    unittest.main()
