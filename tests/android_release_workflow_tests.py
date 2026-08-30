#!/usr/bin/env python3
import os
import re
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class AndroidReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gradle = read("android/app/build.gradle")
        cls.root_gradle = read("android/build.gradle")
        cls.lint_config = read("android/app/lint.xml")
        cls.workflow = read(".github/workflows/mobile-beta-deploy.yml")
        cls.deploy_script = read("scripts/android_firebase_deploy.sh")
        cls.android_readme = read("android/README.md")
        cls.manifest = read("android/app/src/main/AndroidManifest.xml")
        cls.activity = read(
            "android/app/src/main/java/com/snurhythm/asobmashow/"
            "AsoBMaShowActivity.java"
        )
        cls.music_service = read(
            "android/app/src/main/java/com/snurhythm/asobmashow/"
            "AsoBMaShowMusicService.java"
        )
        cls.sqlite_raii = read("src/repositories/SqliteRAII.h")
        cls.android_natives = read("src/AndroidNatives.cpp")
        cls.credential_backend = read("src/ir/IrCredentialBackend.cpp")
        cls.sdl_hid_manager = read(
            "SDL/android-project/app/src/main/java/org/libsdl/app/"
            "HIDDeviceManager.java"
        )

    def test_android_release_toolchain_meets_api_36_policy(self):
        self.assertRegex(self.gradle, r"(?m)^\s*compileSdk 36$")
        self.assertRegex(self.gradle, r"(?m)^\s*targetSdk 36$")
        self.assertIn(
            "id 'com.android.application' version '8.10.1' apply false",
            self.root_gradle,
        )
        self.assertIn("28.2.13676358", self.gradle)
        self.assertIn("source.properties", self.gradle)
        self.assertIn("Pkg.Revision", self.gradle)
        self.assertNotIn("file(androidNdkRoot).name", self.gradle)
        self.assertRegex(self.gradle, r"(?m)^\s*ndkPath androidNdkRoot$")

        wrapper = ROOT / "android/gradle/wrapper/gradle-wrapper.properties"
        self.assertTrue(wrapper.is_file(), "repository-owned Gradle wrapper is missing")
        wrapper_properties = wrapper.read_text(encoding="utf-8")
        self.assertIn("gradle-8.11.1-bin.zip", wrapper_properties)
        self.assertIn(
            "distributionSha256Sum="
            "f397b287023acdba1e9f6fc5ea72d22dd63669d59ed4a289a29b1a76eee151c6",
            wrapper_properties,
        )

    def test_android_release_entrypoints_use_repository_wrapper(self):
        expected_wrapper = "android/gradlew"
        self.assertIn(expected_wrapper, self.deploy_script)
        self.assertNotIn("SDL/android-project/gradlew", self.deploy_script)
        self.assertIn(expected_wrapper, self.workflow)
        self.assertNotIn("SDL/android-project/gradlew", self.workflow)
        self.assertIn(expected_wrapper, self.android_readme)
        self.assertTrue((ROOT / expected_wrapper).is_file())

    def test_android_sqlite_snapshots_use_private_cache(self):
        self.assertIn("GetAndroidCacheDir()", self.sqlite_raii)
        self.assertRegex(
            self.sqlite_raii,
            r"(?s)#if TARGET_OS_ANDROID.*GetAndroidCacheDir\(\).*#else.*"
            r"temp_directory_path",
        )

    def test_android_ir_credentials_use_private_files_and_migrate(self):
        android_factory = self.credential_backend.split(
            "std::unique_ptr<IrCredentialBackend> CreatePlatformIrCredentialBackend",
            1,
        )[1]
        self.assertIn("GetAndroidInternalFilesDir()", android_factory)
        self.assertIn("requiresLegacyFileMigration", self.credential_backend)
        internal_files = self.android_natives.split(
            "std::string GetAndroidInternalFilesDir()", 1
        )[1].split("std::string GetAndroidCacheDir()", 1)[0]
        self.assertNotIn("GetAndroidExternalFilesDir()", internal_files)

    def test_android_credentials_are_excluded_from_backup_and_transfer(self):
        android_namespace = "http://schemas.android.com/apk/res/android"
        application = ET.fromstring(self.manifest).find("application")
        self.assertIsNotNone(application)
        self.assertEqual(
            application.get(f"{{{android_namespace}}}allowBackup"), "true"
        )
        self.assertEqual(
            application.get(f"{{{android_namespace}}}fullBackupContent"),
            "@xml/backup_rules",
        )
        self.assertEqual(
            application.get(f"{{{android_namespace}}}dataExtractionRules"),
            "@xml/data_extraction_rules",
        )

        legacy_rules = ET.parse(
            ROOT / "android/app/src/main/res/xml/backup_rules.xml"
        ).getroot()
        self.assertEqual(legacy_rules.tag, "full-backup-content")
        self.assertEqual(
            {(node.get("domain"), node.get("path")) for node in legacy_rules},
            {("file", "profiles")},
        )

        extraction_rules = ET.parse(
            ROOT / "android/app/src/main/res/xml/data_extraction_rules.xml"
        ).getroot()
        self.assertEqual(extraction_rules.tag, "data-extraction-rules")
        for section_name in ("cloud-backup", "device-transfer"):
            section = extraction_rules.find(section_name)
            self.assertIsNotNone(section)
            self.assertEqual(
                {
                    (node.get("domain"), node.get("path"))
                    for node in section.findall("exclude")
                },
                {("file", "profiles")},
            )

    def test_android_download_redirects_cannot_downgrade_https_to_http(self):
        self.assertIn(
            'requireHttps && !"https".equalsIgnoreCase(redirectUrl.getProtocol())',
            self.activity,
        )
        self.assertIn(
            'throw new IOException("HTTPS download redirected to insecure HTTP.")',
            self.activity,
        )
        self.assertNotIn(
            'requireHttps = requireHttps || "https".equalsIgnoreCase(redirectProtocol)',
            self.activity,
        )

    def test_android_deploy_accepts_standalone_ndk_directory_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            android_home = root / "sdk"
            android_home.mkdir()
            vcpkg_root = root / "vcpkg"
            vcpkg_root.mkdir()
            ndk_root = root / "android-ndk-r28c"
            (ndk_root / "build/cmake").mkdir(parents=True)
            (ndk_root / "build/cmake/android.toolchain.cmake").touch()
            (ndk_root / "source.properties").write_text(
                "Pkg.Desc = Android NDK\nPkg.Revision = 28.2.13676358\n",
                encoding="utf-8",
            )

            environment = os.environ.copy()
            environment.update(
                {
                    "ANDROID_HOME": str(android_home),
                    "ANDROID_SDK_ROOT": str(android_home),
                    "ANDROID_NDK_HOME": str(ndk_root),
                    "VCPKG_ROOT": str(vcpkg_root),
                }
            )
            result = subprocess.run(
                [
                    str(ROOT / "scripts/android_firebase_deploy.sh"),
                    "--build-only",
                    "--skip-build",
                    "--variant",
                    "firebaseDebug",
                ],
                cwd=ROOT,
                env=environment,
                text=True,
                capture_output=True,
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_android_artifact_upload_does_not_require_build_toolchains(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apk = root / "app-firebase-debug.apk"
            apk.write_bytes(b"fixture")
            firebase = root / "firebase"
            firebase.write_text(
                "#!/bin/sh\nexit 0\n",
                encoding="utf-8",
            )
            firebase.chmod(0o755)

            environment = os.environ.copy()
            for name in (
                "ANDROID_HOME",
                "ANDROID_SDK_ROOT",
                "ANDROID_NDK_HOME",
                "ANDROID_NDK_ROOT",
                "VCPKG_ROOT",
                "JAVA_HOME",
            ):
                environment.pop(name, None)
            environment.update(
                {
                    "ANDROID_HOME": str(root / "missing-sdk"),
                    "ANDROID_SDK_ROOT": str(root / "missing-sdk"),
                    "ANDROID_NDK_HOME": str(root / "missing-ndk"),
                    "ANDROID_NDK_ROOT": str(root / "missing-ndk"),
                    "VCPKG_ROOT": str(root / "missing-vcpkg"),
                    "JAVA_HOME": str(root / "missing-java"),
                    "FIREBASE_ANDROID_APP_ID": "1:1234567890:android:fixture",
                    "FIREBASE_TOKEN": "fixture-token",
                }
            )
            result = subprocess.run(
                [
                    str(ROOT / "scripts/android_firebase_deploy.sh"),
                    "--skip-build",
                    "--apk",
                    str(apk),
                    "--variant",
                    "firebaseDebug",
                    "--firebase-cli",
                    str(firebase),
                ],
                cwd=ROOT,
                env=environment,
                text=True,
                capture_output=True,
            )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_android_lint_is_fatal_and_runs_before_firebase_deploy(self):
        self.assertIn("abortOnError true", self.gradle)
        self.assertNotIn("abortOnError false", self.gradle)
        self.assertRegex(self.gradle, r"lintConfig\s+file\(['\"]lint\.xml['\"]\)")
        self.assertTrue((ROOT / "android/app/lint.xml").is_file())
        ignored_paths = re.findall(
            r'<ignore path="([^"]+)"', self.lint_config
        )
        self.assertEqual(
            ignored_paths,
            [
                "../../SDL/android-project/app/src/main/java/org/libsdl/app/"
                "HIDDeviceBLESteamController.java",
                "../../SDL/android-project/app/src/main/java/org/libsdl/app/"
                "HIDDeviceManager.java",
                "../../SDL/android-project/app/src/main/java/org/libsdl/app/"
                "SDLAudioManager.java",
            ],
        )

        android_job = self.workflow.split("  android-firebase:", 1)[1]
        self.assertIn(
            "android/gradlew -p android lintFirebaseDebug",
            android_job,
        )
        lint_index = android_job.index("lintFirebaseDebug")
        deploy_index = android_job.index("android_firebase_deploy.sh")
        self.assertLess(lint_index, deploy_index)

    def test_notification_permission_is_declared_requested_and_checked(self):
        self.assertIn("android.permission.POST_NOTIFICATIONS", self.manifest)
        self.assertIn("requestPermissions", self.activity)
        self.assertIn("Manifest.permission.POST_NOTIFICATIONS", self.activity)
        self.assertIn("checkSelfPermission", self.music_service)
        self.assertIn("Manifest.permission.POST_NOTIFICATIONS", self.music_service)

    def test_persisted_uri_permissions_use_explicit_read_grants(self):
        permission_calls = re.findall(
            r"takePersistableUriPermission\(\s*\w+,\s*([^\)]+)\)",
            self.activity,
        )
        self.assertEqual(
            permission_calls,
            [
                "Intent.FLAG_GRANT_READ_URI_PERMISSION",
                "Intent.FLAG_GRANT_READ_URI_PERMISSION",
            ],
        )

    def test_sdl_dynamic_receivers_are_android_13_compatible(self):
        self.assertIn("registerReceiverCompat(mUsbBroadcast, filter)",
                      self.sdl_hid_manager)
        self.assertIn("registerReceiverCompat(mBluetoothBroadcast, filter)",
                      self.sdl_hid_manager)
        self.assertIn("Context.RECEIVER_NOT_EXPORTED", self.sdl_hid_manager)


if __name__ == "__main__":
    unittest.main()
