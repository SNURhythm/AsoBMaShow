from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BackgroundDownloadTests(unittest.TestCase):
    def test_transport(self):
        java_home = os.environ.get('JAVA_HOME')
        if not java_home and sys.platform == 'darwin':
            java_home = subprocess.check_output(['/usr/libexec/java_home', '-v', '17'], text=True).strip()
        javac = str(Path(java_home) / 'bin/javac') if java_home else shutil.which('javac')
        java = str(Path(java_home) / 'bin/java') if java_home else shutil.which('java')
        if not javac or not java:
            self.skipTest('JDK unavailable')
        with tempfile.TemporaryDirectory() as output:
            subprocess.run([javac, '-d', output,
                str(ROOT / 'android/app/src/main/java/com/snurhythm/asobmashow/BmsDownloadTransport.java'),
                str(ROOT / 'tests/java/BmsDownloadTransportTest.java')], check=True)
            subprocess.run([java, '-cp', output, 'com.snurhythm.asobmashow.BmsDownloadTransportTest', output], check=True, timeout=20)

if __name__ == '__main__': unittest.main()
