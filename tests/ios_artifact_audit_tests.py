#!/usr/bin/env python3
import plistlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts/ios_artifact_audit.sh"


class IOSArtifactAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if shutil.which("xcrun") is None or shutil.which("otool") is None:
            raise unittest.SkipTest("iOS artifact tools require Xcode on macOS")

    def make_app(self, root: Path) -> Path:
        app = root / "Fixture.app"
        app.mkdir()
        executable = "Fixture"
        source = root / "main.c"
        source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        subprocess.run(
            [
                "xcrun",
                "--sdk",
                "iphoneos",
                "clang",
                "-arch",
                "arm64",
                "-miphoneos-version-min=14.0",
                str(source),
                "-o",
                str(app / executable),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        info = {
            "CFBundleExecutable": executable,
            "CFBundleIdentifier": "com.snurhythm.AsoBMaShow",
            "CFBundleShortVersionString": "0.0.1",
            "CFBundleVersion": "1",
            "CFBundleSupportedPlatforms": ["iPhoneOS"],
            "DTPlatformName": "iphoneos",
            "DTSDKName": "iphoneos26.0",
            "MinimumOSVersion": "14.0",
            "UIDeviceFamily": [1, 2],
            "CFBundleIcons": {
                "CFBundlePrimaryIcon": {
                    "CFBundleIconName": "FixtureIcon",
                    "CFBundleIconFiles": ["FixtureIcon60x60"],
                }
            },
            "NSMotionUsageDescription": "Motion controls the turntable.",
            "NSPhotoLibraryUsageDescription": "Replay export access.",
            "NSPhotoLibraryAddUsageDescription": "Save replay exports.",
            "NSAppTransportSecurity": {"NSAllowsArbitraryLoads": True},
        }
        with (app / "Info.plist").open("wb") as handle:
            plistlib.dump(info, handle)
        (app / "FixtureIcon60x60@2x.png").write_bytes(b"fixture-icon")
        return app

    def run_audit(self, artifact: Path, *arguments: str):
        return subprocess.run(
            [str(AUDIT), *arguments, str(artifact)],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def mutate_plist(self, app: Path, key: str, value) -> None:
        plist_path = app / "Info.plist"
        with plist_path.open("rb") as handle:
            info = plistlib.load(handle)
        info[key] = value
        with plist_path.open("wb") as handle:
            plistlib.dump(info, handle)

    def test_valid_unsigned_app_passes_without_privacy_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            self.assertFalse((app / "PrivacyInfo.xcprivacy").exists())
            result = self.run_audit(app)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("audit passed", result.stdout)
            self.assertIn("signature check skipped", result.stdout)

    def test_valid_ipa_is_extracted_outside_the_source_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = self.make_app(root)
            ipa = root / "Fixture.ipa"
            with zipfile.ZipFile(ipa, "w") as archive:
                for path in app.rglob("*"):
                    if path.is_file():
                        archive.write(
                            path,
                            Path("Payload") / app.name / path.relative_to(app),
                        )
            result = self.run_audit(ipa)
            self.assertEqual(0, result.returncode, result.stderr)

    def test_release_metadata_failures_are_specific(self):
        cases = (
            ("CFBundleShortVersionString", "0.0.2", "version"),
            ("MinimumOSVersion", "15.0", "minimum OS"),
            ("DTSDKName", "macosx26.0", "SDK"),
            ("CFBundleIdentifier", "invalid.bundle", "bundle identifier"),
            ("UIDeviceFamily", [1], "device family"),
            ("CFBundleIcons", {}, "icon"),
            ("NSMotionUsageDescription", "", "NSMotionUsageDescription"),
            ("NSAppTransportSecurity", {}, "NSAllowsArbitraryLoads"),
        )
        for key, value, diagnostic in cases:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temp:
                app = self.make_app(Path(temp))
                self.mutate_plist(app, key, value)
                result = self.run_audit(app)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(diagnostic, result.stderr)

    def test_unwanted_files_and_embedded_secrets_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "._Symbols").write_text("unwanted", encoding="utf-8")
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("unwanted file", result.stderr)

        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "credentials.txt").write_text(
                "-----BEGIN PRIVATE KEY-----\nfixture\n", encoding="utf-8"
            )
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("credential material", result.stderr)

    def test_embedded_binary_secret_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            with (app / "Fixture").open("ab") as executable:
                executable.write(b"\0APP_STORE_KEY=embedded-binary-fixture\0")

            result = self.run_audit(app)

            self.assertNotEqual(0, result.returncode)
            self.assertIn("credential material", result.stderr)

    def test_architecture_and_dependency_resolution_failures_are_detected(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "Fixture").write_text("not a Mach-O", encoding="utf-8")
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Mach-O", result.stderr)

        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            subprocess.run(
                [
                    "install_name_tool",
                    "-add_rpath",
                    "/opt/homebrew/lib",
                    str(app / "Fixture"),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("local build dependency", result.stderr)

    def test_simulator_slice_in_distribution_binary_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            app = self.make_app(root)
            simulator = root / "Fixture-simulator"
            subprocess.run(
                [
                    "xcrun",
                    "--sdk",
                    "iphonesimulator",
                    "clang",
                    "-arch",
                    "x86_64",
                    "-mios-simulator-version-min=14.0",
                    str(root / "main.c"),
                    "-o",
                    str(simulator),
                ],
                check=True,
                text=True,
                capture_output=True,
            )
            device = root / "Fixture-device"
            (app / "Fixture").replace(device)
            subprocess.run(
                [
                    "xcrun",
                    "lipo",
                    "-create",
                    str(device),
                    str(simulator),
                    "-output",
                    str(app / "Fixture"),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            result = self.run_audit(app)

            self.assertNotEqual(0, result.returncode)
            self.assertIn("device arm64 only", result.stderr)

    def test_signature_is_only_mandatory_when_requested(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            result = self.run_audit(app, "--require-signature")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("signature", result.stderr)


if __name__ == "__main__":
    unittest.main()
