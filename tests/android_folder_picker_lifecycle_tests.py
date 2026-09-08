from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JAVA_ROOT = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class AndroidFolderPickerLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        activity = (JAVA_ROOT / "AsoBMaShowActivity.java").read_text()
        signatures = ["protected void onDestroy()", "public String pickChartFolder(",
                      "public String ensureManageExternalStorageAccess("]
        for optional in ["private void finishPicker()",
                         "private void finishManageStorageRequest()"]:
            if optional in activity:
                signatures.append(optional)
        methods = "\n".join(method(activity, signature) for signature in signatures)
        fixture = (ROOT / "tests/java/AndroidFolderPickerActivityFixture.java").read_text()
        helper = JAVA_ROOT / "NativeFolderPickerRequests.java"
        fields = ""
        sources = []
        if helper.exists():
            fields = "final NativeFolderPickerRequests folderPickerRequests = new NativeFolderPickerRequests();"
            sources = [str(helper), str(JAVA_ROOT / "DocumentHandoffRequestCodeAllocator.java"),
                       str(ROOT / "tests/java/NativeFolderPickerRequestsTests.java")]
        generated = Path(cls.output.name) / "AndroidFolderPickerActivityFixture.java"
        generated.write_text(fixture.replace("ACTIVITY_METHODS", methods).replace("ACTIVITY_FIELDS", fields))
        subprocess.run([javac, "-d", cls.output.name, str(generated), *sources], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.output.cleanup()

    def run_scenario(self, scenario):
        subprocess.run([self.java, "-cp", self.output.name,
                        "com.snurhythm.asobmashow.AndroidFolderPickerActivityFixture",
                        scenario], check=True, timeout=15)

    def test_destroy_releases_folder_before_sdl_join(self):
        self.run_scenario("folder-wait")

    def test_destroy_releases_permission_before_sdl_join(self):
        self.run_scenario("permission-wait")

    def test_destroy_racing_folder_dispatch(self):
        self.run_scenario("folder-dispatch")

    def test_destroy_racing_permission_dispatch(self):
        self.run_scenario("permission-dispatch")

    def test_late_folder_request_is_rejected(self):
        self.run_scenario("folder-late")

    def test_late_permission_request_is_rejected(self):
        self.run_scenario("permission-late")

    def test_owner_cancellation_and_result_delivery(self):
        subprocess.run([self.java, "-cp", self.output.name,
                        "com.snurhythm.asobmashow.NativeFolderPickerRequestsTests"],
                       check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
