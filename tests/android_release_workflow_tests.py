#!/usr/bin/env python3
import re
import unittest
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
