"""Packaging contracts for GingerRush-only downloads and the retired browser."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ANDROID = "{http://schemas.android.com/apk/res/android}"


class DownloadScopeTests(unittest.TestCase):
    def test_android_restores_transfers_but_not_browser(self):
        manifest = ET.parse(ROOT / "android/app/src/main/AndroidManifest.xml")
        names = {node.get(ANDROID + "name", "")
                 for node in manifest.findall("application/*")}
        self.assertNotIn(".BmsBrowserActivity", names)
        self.assertIn(".BmsDownloadService", names)
        self.assertIn(".BmsDownloadJobService", names)

    def test_only_authorized_driver_is_packaged(self):
        self.assertTrue((ROOT / "src/bms_search/GingerRushDriver.cpp").is_file())
        for name in ("BmsSearchDriver", "PackageSourceDrivers", "GoogleDriveDriver",
                     "DropboxDriver", "IpfsDriver", "DirectArchiveDriver"):
            self.assertFalse((ROOT / f"src/bms_search/{name}.cpp").exists(), name)
        for name in ("BmsBrowser.cpp", "BmsBrowserIOS.mm", "BmsBrowserAndroid.cpp",
                     "DownloadWebKitIOS.mm", "BrowserRequest.cpp"):
            self.assertFalse((ROOT / "src/downloads" / name).exists(), name)

    def test_legacy_and_authorized_jobs_use_separate_storage(self):
        android = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"
        ios = ROOT / "src/downloads/DownloadPlatformIOS.mm"
        self.assertTrue((android / "BmsDownloads.java").is_file())
        self.assertTrue(ios.is_file())
        self.assertIn('"GingerRushDownloadsV1"', (android / "BmsDownloads.java").read_text())
        self.assertIn('@"GingerRushDownloadsV1"', ios.read_text())
        self.assertIn('@"com.snurhythm.AsoBMaShow.gingerrush.v1"', ios.read_text())
        self.assertIn('downloads.v1', (ROOT / "src/platform/RetiredBmsDownloadsIOS.mm").read_text())

    def test_removed_webkit_does_not_leave_a_dangling_linker_flag(self):
        import re
        project = (ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj").read_text()
        self.assertNotRegex(project, re.compile(r'"-framework",\s*"\$\(DERIVED_FILE_DIR\)'))

    def test_apology_assets_are_complete_and_korean_is_verbatim(self):
        import hashlib
        directory = ROOT / "assets/notices"
        original = (directory / "apology-2026-11-08.ko.txt").read_bytes()
        self.assertEqual(hashlib.sha256(original.rsplit(b"\n\n", 1)[0] + b"\n").hexdigest(), "1777d44bed5845fe61737024370bad59290198f7f1dc6495ac653982c11454fd")
        self.assertEqual(original.decode().rsplit("\n\n", 1)[1].strip(),
                         '11월 10일 추가: GingerRush의 운영자, Catizard님께 문의드린 결과 다운로드 API 사용 허가를 해주셨기에 GingerRush를 통한 다운로드 기능을 복구하였습니다.')
        for language in ("en", "ko", "ja", "zh-Hans", "zh-Hant"):
            text = (directory / f"apology-2026-11-08.{language}.txt").read_text()
            self.assertEqual(sum(line.startswith("# ") for line in text.splitlines()), 3)
            self.assertEqual(len(text.strip().split("\n\n")), 17)
            for name in ("HorieYuuka", "GingerRush", "Konmai", "Wriggle", "BMS Search",
                         "Bokutachi IR", "User Agent", "REST API", "Codex", "Catizard"):
                self.assertIn(name, text, language)

if __name__ == "__main__":
    unittest.main()
