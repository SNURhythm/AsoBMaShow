from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from android_folder_picker_lifecycle_tests import method

ROOT = Path(__file__).resolve().parents[1]


class AndroidWindowPresentationTests(unittest.TestCase):
    def test_startup_and_focus_restore_immersive_window(self):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        source = (ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AsoBMaShowActivity.java").read_text()
        signatures = ["protected void onCreate(Bundle savedInstanceState)"]
        for signature in ["public void onWindowFocusChanged(boolean hasFocus)",
                          "private void restoreImmersiveMode()"]:
            if signature in source:
                signatures.append(signature)
        methods = "\n".join(method(source, signature) for signature in signatures)
        fixture = (ROOT / "tests/java/AndroidWindowPresentationFixture.java").read_text()
        with tempfile.TemporaryDirectory() as output:
            generated = Path(output) / "AndroidWindowPresentationFixture.java"
            generated.write_text(fixture.replace("ACTIVITY_METHODS", methods))
            subprocess.run([javac, "-d", output, str(generated)], check=True)
            subprocess.run([java, "-cp", output, "AndroidWindowPresentationFixture"],
                           check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
