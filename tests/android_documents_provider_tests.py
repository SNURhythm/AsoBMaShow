from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / 'android/app/src/main/java/com/snurhythm/asobmashow'


class DocumentsProviderTests(unittest.TestCase):
    def test_paths_and_change_batches(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home and Path('/usr/libexec/java_home').is_file():
            java_home = subprocess.check_output(['/usr/libexec/java_home', '-v', '17'], text=True).strip()
        javac = str(Path(java_home) / 'bin/javac') if java_home else 'javac'
        java = str(Path(java_home) / 'bin/java') if java_home else 'java'
        with tempfile.TemporaryDirectory() as output:
            subprocess.run([javac, '-d', output,
                            str(JAVA / 'DocumentsPathPolicy.java'),
                            str(JAVA / 'DocumentsLibraryChanges.java'),
                            str(ROOT / 'tests/java/DocumentsProviderTests.java')], check=True)
            subprocess.run([java, '-cp', output,
                            'com.snurhythm.asobmashow.DocumentsProviderTests'], check=True)


if __name__ == '__main__':
    unittest.main()
