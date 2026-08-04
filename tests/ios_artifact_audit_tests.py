#!/usr/bin/env python3
import plistlib
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts/ios_artifact_audit.sh"
SKIN_SHADER_AUDIT = ROOT / "scripts/verify_skin_shader_outputs.py"
BUILD_COMMIT = "1234567890abcdef1234567890abcdef12345678"
BUILD_CONFIGURATION = "Release"
BUILD_SOURCE_CLEAN = "1"


class SkinShaderOutputVerifierTests(unittest.TestCase):
    def make_shader_tree(self, root: Path) -> Path:
        (root / "shader_src").mkdir(parents=True)
        (root / "shader_src/vs_skin_quad.sc").write_text(
            "void main() { /* synthetic vertex */ }\n", encoding="utf-8"
        )
        (root / "shader_src/fs_skin_quad.sc").write_text(
            "void main() { /* synthetic fragment */ }\n", encoding="utf-8"
        )
        for backend in ("metal", "spirv", "essl", "dx11"):
            output = root / "shaders" / backend
            output.mkdir(parents=True)
            (output / "vs_skin_quad.bin").write_bytes(
                f"{backend}-vertex".encode("ascii")
            )
            (output / "fs_skin_quad.bin").write_bytes(
                f"{backend}-fragment".encode("ascii")
            )
        return root / "tests/fixtures/beatoraja_skin/shaders/skin_shader_manifest.json"

    def run_shader_audit(self, root: Path, *arguments: str):
        return subprocess.run(
            [
                "python3",
                str(SKIN_SHADER_AUDIT),
                "--root",
                str(root),
                "--shader",
                "skin_quad",
                *arguments,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def test_manifest_write_and_read_verification_are_deterministic(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = self.make_shader_tree(root)
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl,dx11",
                "--write-manifest",
                str(manifest),
            )
            self.assertEqual(0, result.returncode, result.stderr)
            first = manifest.read_bytes()
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl,dx11",
                "--write-manifest",
                str(manifest),
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(first, manifest.read_bytes())
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl,dx11",
                "--manifest",
                str(manifest),
            )
            self.assertEqual(0, result.returncode, result.stderr)

    def test_missing_empty_and_hash_mismatched_outputs_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = self.make_shader_tree(root)
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl,dx11",
                "--write-manifest",
                str(manifest),
            )
            self.assertEqual(0, result.returncode, result.stderr)

            output = root / "shaders/dx11/fs_skin_quad.bin"
            output.unlink()
            result = self.run_shader_audit(
                root, "--require-backends", "metal,spirv,essl,dx11"
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("missing", result.stderr.lower())

            output.write_bytes(b"")
            result = self.run_shader_audit(
                root, "--require-backends", "metal,spirv,essl,dx11"
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("empty", result.stderr.lower())

            output.write_bytes(b"tampered")
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl,dx11",
                "--manifest",
                str(manifest),
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("manifest", result.stderr.lower())

    def test_unexpected_shader_tree_change_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_shader_tree(root)
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl",
                "--changed-path",
                "shader_src/unrelated.sc",
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("unexpected shader-tree change", result.stderr.lower())

    def test_explicit_changed_path_cannot_hide_dirty_git_shader_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_shader_tree(root)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            subprocess.run(
                ["git", "config", "user.email", "fixture@example.invalid"],
                cwd=root,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Shader Fixture"],
                cwd=root,
                check=True,
            )
            subprocess.run(["git", "add", "."], cwd=root, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "fixture baseline"],
                cwd=root,
                check=True,
            )
            (root / "shader_src/unrelated.sc").write_text(
                "unexpected\n", encoding="utf-8"
            )
            result = self.run_shader_audit(
                root,
                "--require-backends",
                "metal,spirv,essl",
                "--changed-path",
                "shader_src/vs_skin_quad.sc",
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("shader_src/unrelated.sc", result.stderr)


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
        source.write_text(
            'const char buildIdentity[] = '
            '"AsoBMaShowBuildIdentityV1|'
            f'{BUILD_COMMIT}|{BUILD_CONFIGURATION}|{BUILD_SOURCE_CLEAN}";\n'
            "int main(void) { return buildIdentity[0] == 0; }\n",
            encoding="utf-8",
        )
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
            "UIFileSharingEnabled": True,
            "LSSupportsOpeningDocumentsInPlace": True,
            "UISupportsDocumentBrowser": True,
            "AsoBMaShowBuildCommit": BUILD_COMMIT,
            "AsoBMaShowBuildConfiguration": BUILD_CONFIGURATION,
            "AsoBMaShowSourceClean": BUILD_SOURCE_CLEAN,
        }
        with (app / "Info.plist").open("wb") as handle:
            plistlib.dump(info, handle)
        (app / "FixtureIcon60x60@2x.png").write_bytes(b"fixture-icon")
        shader_directory = app / "shaders/metal"
        shader_directory.mkdir(parents=True)
        (shader_directory / "vs_skin_quad.bin").write_bytes(b"metal-vertex")
        (shader_directory / "fs_skin_quad.bin").write_bytes(b"metal-fragment")
        (shader_directory / "vs_skin_yuvrgb.bin").write_bytes(b"metal-yuv-vertex")
        (shader_directory / "fs_skin_yuvrgb.bin").write_bytes(b"metal-yuv-fragment")
        return app

    def run_audit(self, artifact: Path, *arguments: str):
        environment = {
            **os.environ,
            "ASOBMASHOW_EXPECTED_BUILD_COMMIT": BUILD_COMMIT,
            "ASOBMASHOW_EXPECTED_BUILD_CONFIGURATION": BUILD_CONFIGURATION,
            "ASOBMASHOW_EXPECTED_SOURCE_CLEAN": BUILD_SOURCE_CLEAN,
        }
        return subprocess.run(
            [str(AUDIT), *arguments, str(artifact)],
            cwd=ROOT,
            text=True,
            capture_output=True,
            env=environment,
        )

    def test_build_identity_must_match_plist_compiled_marker_and_expected_environment(self):
        cases = (
            ("AsoBMaShowBuildCommit", "f" * 40, "build commit"),
            ("AsoBMaShowBuildConfiguration", "Debug", "build configuration"),
            ("AsoBMaShowSourceClean", "0", "source-clean"),
        )
        for key, value, diagnostic in cases:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as temp:
                app = self.make_app(Path(temp))
                self.mutate_plist(app, key, value)
                result = self.run_audit(app)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(diagnostic, result.stderr.lower())

        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            executable = app / "Fixture"
            binary = executable.read_bytes().replace(
                b"AsoBMaShowBuildIdentityV1|", b"AsoBMaShowInvalidIdentityV1|",
                1,
            )
            executable.write_bytes(binary)
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("compiled build identity", result.stderr.lower())

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

    def test_missing_or_empty_metal_skin_shader_fails(self):
        for name in (
            "vs_skin_quad.bin",
            "fs_skin_quad.bin",
            "vs_skin_yuvrgb.bin",
            "fs_skin_yuvrgb.bin",
        ):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                app = self.make_app(Path(temp))
                (app / "shaders/metal" / name).unlink()
                result = self.run_audit(app)
                self.assertNotEqual(0, result.returncode)
                self.assertIn("Metal skin shader is missing", result.stderr)
                self.assertIn(name, result.stderr)

        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "shaders/metal/fs_skin_quad.bin").write_bytes(b"")
            result = self.run_audit(app)
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Metal skin shader is empty", result.stderr)
            self.assertIn("fs_skin_quad.bin", result.stderr)

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
            ("UIFileSharingEnabled", False, "UIFileSharingEnabled"),
            (
                "LSSupportsOpeningDocumentsInPlace",
                False,
                "LSSupportsOpeningDocumentsInPlace",
            ),
            ("UISupportsDocumentBrowser", False, "UISupportsDocumentBrowser"),
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

    def test_resource_secret_is_rejected_without_disclosing_value(self):
        token = "synthetic-review-token-0123456789"
        with tempfile.TemporaryDirectory() as temp:
            app = self.make_app(Path(temp))
            (app / "credentials.txt").write_text(
                f"Authorization: Bearer {token}\n", encoding="utf-8"
            )

            result = self.run_audit(app)
            output = result.stdout + result.stderr

            self.assertNotEqual(0, result.returncode)
            self.assertIn("credential material", output)
            self.assertNotIn(token, output)

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
