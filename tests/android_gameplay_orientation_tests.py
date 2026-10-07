"""Exercise the activity's production orientation lock across launcher rotation."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from android_folder_picker_lifecycle_tests import method

ROOT = Path(__file__).resolve().parents[1]


class AndroidGameplayOrientationTests(unittest.TestCase):
    def test_gameplay_captures_a_fixed_orientation_until_unlock(self):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        source = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java").read_text()
        methods = method(source, "public void setScreenOrientation(int mode, boolean lockCurrent)")
        helper = "private int captureScreenOrientation()"
        if helper in source:
            methods += "\n" + method(source, helper)
        fixture = (ROOT / "tests/java/AndroidGameplayOrientationFixture.java").read_text()
        with tempfile.TemporaryDirectory() as output:
            generated = Path(output) / "AndroidGameplayOrientationFixture.java"
            generated.write_text(fixture.replace("ACTIVITY_METHODS", methods))
            subprocess.run([javac, "-d", output, str(generated)], check=True)
            subprocess.run([java, "-cp", output, "AndroidGameplayOrientationFixture"],
                           check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
