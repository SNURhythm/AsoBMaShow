#!/usr/bin/env python3
import plistlib
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts/macos_artifact_audit.sh"


class MacOSArtifactAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        required = ("clang", "codesign", "install_name_tool", "otool", "vtool")
        if platform.system() != "Darwin" or any(shutil.which(tool) is None for tool in required):
            raise unittest.SkipTest("macOS artifact tools are required")
        if platform.machine() != "arm64":
            raise unittest.SkipTest("fixtures currently exercise the arm64 release contract")

    def make_app(self, root: Path, *, binary_secret_with_large_trailer=False) -> Path:
        app = root / "AsoBMaShow.app"
        executable_dir = app / "Contents/MacOS"
        resource_dir = app / "Contents/Resources"
        executable_dir.mkdir(parents=True)
        (resource_dir / "assets").mkdir(parents=True)
        (resource_dir / "shaders/metal").mkdir(parents=True)

        source = root / "main.c"
        if binary_secret_with_large_trailer:
            trailer = "post-secret-padding" * (1024 * 128)
            source_text = (
                '__attribute__((used, section("__TEXT,__secret"))) '
                'static const char secret[] = "Authorization: Bearer '
                'abcdefghijklmnopqrstuvwxyz0123456789";\n'
                '__attribute__((used, section("__TEXT,__trailer"))) '
                f'static const char trailer[] = "{trailer}";\n'
                "int main(void) { return secret[0] == 0 || trailer[0] == 0; }\n"
            )
        else:
            source_text = (
                'int main(void) { const char *volatile header = '
                '"Authorization: Bearer %s"; return header[0] == 0; }\n'
            )
        source.write_text(source_text, encoding="utf-8")
        subprocess.run(
            [
                "clang",
                "-arch",
                "arm64",
                "-mmacosx-version-min=13.0",
                str(source),
                "-o",
                str(executable_dir / "AsoBMaShow"),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        info = {
            "CFBundleExecutable": "AsoBMaShow",
            "CFBundleIdentifier": "com.SNURhythm.AsoBMaShow",
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": "AsoBMaShow",
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": "0.0.1",
            "CFBundleVersion": "0.0.1",
            "CFBundleIconFile": "AsoBMaShow.icns",
            "LSMinimumSystemVersion": "13.0",
        }
        with (app / "Contents/Info.plist").open("wb") as handle:
            plistlib.dump(info, handle)
        (resource_dir / "AsoBMaShow.icns").write_bytes(b"fixture-icon")
        (resource_dir / "assets/fixture.txt").write_text("asset\n", encoding="utf-8")
        (resource_dir / "shaders/metal/fixture.bin").write_bytes(b"shader")
        subprocess.run(
            ["codesign", "--force", "--deep", "--options", "runtime", "--sign", "-", str(app)],
            check=True,
            text=True,
            capture_output=True,
        )
        return app

    def run_audit(self, app: Path, *arguments: str):
        return subprocess.run(
            [str(AUDIT), *arguments, str(app)],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def mutate_plist(self, app: Path, key: str, value) -> None:
        plist_path = app / "Contents/Info.plist"
        with plist_path.open("rb") as handle:
            info = plistlib.load(handle)
        info[key] = value
        with plist_path.open("wb") as handle:
            plistlib.dump(info, handle)

    def test_valid_ad_hoc_verification_bundle_passes(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            result = self.run_audit(app)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("macOS artifact audit passed", result.stdout)
            self.assertIn("signature check passed", result.stdout)

    def test_release_metadata_and_resources_are_enforced(self):
        cases = (
            ("CFBundleShortVersionString", "0.0.2", "version"),
            ("LSMinimumSystemVersion", "26.0", "minimum OS"),
            ("CFBundleIdentifier", "invalid.bundle", "bundle identifier"),
            ("CFBundleIconFile", "Missing.icns", "bundle icon"),
        )
        for key, value, diagnostic in cases:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temp:
                app = self.make_app(Path(temp))
                self.mutate_plist(app, key, value)
                result = self.run_audit(app)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(diagnostic, result.stderr)

    def test_local_build_rpath_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            executable = app / "Contents/MacOS/AsoBMaShow"
            subprocess.run(
                ["install_name_tool", "-add_rpath", "/opt/homebrew/lib", str(executable)],
                check=True,
                text=True,
                capture_output=True,
            )
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("local build rpath", result.stderr)

    def test_tampered_signature_and_embedded_secret_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "Contents/Resources/assets/fixture.txt").write_text(
                "tampered\n", encoding="utf-8"
            )
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("signature verification", result.stderr)

        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "Contents/Resources/assets/credentials.txt").write_text(
                "-----BEGIN PRIVATE KEY-----\nfixture\n", encoding="utf-8"
            )
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("credential material", result.stderr)

    def test_binary_secret_before_large_trailer_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(
                Path(temp), binary_secret_with_large_trailer=True
            )

            result = self.run_audit(app)

            self.assertNotEqual(0, result.returncode)
            self.assertIn(
                "credential material is embedded in binary", result.stderr
            )

    def test_resource_secret_is_rejected_without_disclosing_value(self):
        token = "synthetic-review-token-0123456789"
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "Contents/Resources/assets/credentials.txt").write_text(
                f"Authorization: Bearer {token}\n", encoding="utf-8"
            )

            result = self.run_audit(app)
            output = result.stdout + result.stderr

            self.assertNotEqual(0, result.returncode)
            self.assertIn("credential material", output)
            self.assertNotIn(token, output)

    def test_release_mode_rejects_ad_hoc_signature(self):
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            result = self.run_audit(app, "--require-signature")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Developer ID Application", result.stderr)


if __name__ == "__main__":
    unittest.main()
