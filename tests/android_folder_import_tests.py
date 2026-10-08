import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class AndroidFolderImportTests(unittest.TestCase):
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
                            str(source / "ChartImportCopyControl.java"),
                            str(source / "ChartFolderImport.java"),
                            str(source / "ChartArchiveImport.java"),
                            str(source / "ArchivePermissionOwnership.java"),
                            str(source / "ArchiveDirectPath.java"),
                            str(ROOT / "tests/java/ArchiveDirectPathTests.java"),
                            str(ROOT / "tests/java/ChartArchiveImportTests.java"),
                            str(ROOT / "tests/java/ChartFolderImportTests.java"),
                            str(ROOT / "tests/java/ImportCopyWorkersTests.java"),
                            str(ROOT / "tests/java/ChartImportCopyControlTests.java")], check=True)
            for test in ("ImportCopyWorkersTests", "ChartFolderImportTests", "ChartImportCopyControlTests", "ChartArchiveImportTests", "ArchiveDirectPathTests"):
                subprocess.run([java, "-cp", output,
                                "com.snurhythm.asobmashow." + test], check=True)


if __name__ == "__main__":
    unittest.main()
