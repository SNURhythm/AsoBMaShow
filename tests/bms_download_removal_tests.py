"""Packaging contracts for the retired BMS download feature (no networking)."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ANDROID = "{http://schemas.android.com/apk/res/android}"


class DownloadRemovalTests(unittest.TestCase):
    def test_android_cannot_launch_or_schedule_retired_download_components(self):
        manifest = ET.parse(ROOT / "android/app/src/main/AndroidManifest.xml")
        names = {node.get(ANDROID + "name", "")
                 for node in manifest.findall("application/*")}
        retired = {".BmsBrowserActivity", ".BmsDownloadService",
                   ".BmsDownloadJobService", ".BmsDownloadCancelReceiver"}
        self.assertFalse(names & retired, names & retired)


    def test_retired_clients_are_not_packaged(self):
        for path in ("src/bms_search", "src/downloads", "src/BmsSearchService.cpp",
                     "src/scene/FindBmsModal.cpp"):
            self.assertFalse((ROOT / path).exists(), path)
        import plistlib
        with (ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist").open("rb") as stream:
            info = plistlib.load(stream)
        self.assertFalse(any(".downloads." in identifier for identifier in
                             info.get("BGTaskSchedulerPermittedIdentifiers", [])))

    def test_removed_webkit_does_not_leave_a_dangling_linker_flag(self):
        import re
        project = (ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj").read_text()
        self.assertNotRegex(project, re.compile(r'"-framework",\s*"\$\(DERIVED_FILE_DIR\)'))

    def test_apology_assets_are_complete_and_korean_is_verbatim(self):
        import hashlib
        directory = ROOT / "assets/notices"
        original = (directory / "apology-2026-11-08.ko.txt").read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), "1777d44bed5845fe61737024370bad59290198f7f1dc6495ac653982c11454fd")
        for language in ("en", "ko", "ja", "zh-Hans", "zh-Hant"):
            text = (directory / f"apology-2026-11-08.{language}.txt").read_text()
            self.assertEqual(sum(line.startswith("# ") for line in text.splitlines()), 3)
            self.assertEqual(len(text.strip().split("\n\n")), 16)
            for name in ("HorieYuuka", "GingerRush", "Konmai", "Wriggle", "BMS Search",
                         "Bokutachi IR", "User Agent", "REST API", "Codex"):
                self.assertIn(name, text, language)

if __name__ == "__main__":
    unittest.main()
