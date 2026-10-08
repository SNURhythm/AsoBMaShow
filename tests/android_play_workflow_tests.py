#!/usr/bin/env python3
"""Exercise release orchestration without contacting Google Play or compiling."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
AAB = "android/app/build/outputs/bundle/restricted_file_accessRelease/app-restricted_file_access-release.aab"

# Substitute only the external Fastlane actions; evaluate the actual lane bodies.
HARNESS = r'''
require "json"
module UI
  def self.user_error!(message); raise message; end
  def self.success(message); end
end
def default_platform(*); end
def platform(*); yield; end
def desc(*); end
def lane(name, &block); define_singleton_method(name, &block); end
def sh(*args); raise "Build failed" unless system(*args); end
def upload_to_play_store(**args)
  File.write("upload.json", JSON.generate(args))
end
load ARGV.fetch(0)
public_send(ARGV.fetch(1))
'''


class AndroidPlayWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        (self.root / "android/fastlane").mkdir(parents=True)
        (self.root / "scripts").mkdir()
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("GOOGLE_PLAY_", "SUPPLY_", "ANDROID_", "FIREBASE_"))}
        self.env.pop("GITHUB_RUN_NUMBER", None)

    def run_lane(self, lane, *, fail_build=False, omit_bundle=False, stale_bundle=False):
        fastfile = ROOT / "android/fastlane/Fastfile"
        self.assertTrue(fastfile.is_file(), "Android Fastlane lanes are missing")
        shutil.copyfile(fastfile, self.root / "android/fastlane/Fastfile")
        artifact = self.root / AAB
        if stale_bundle:
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"old bundle")
        helper = self.root / "scripts/android_firebase_deploy.sh"
        helper.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$@" > build-args.txt\n' +
            ("exit 1\n" if fail_build else "" if omit_bundle else
             f'mkdir -p "{artifact.parent}"\nprintf bundle > "{artifact}"\n'))
        helper.chmod(0o755)
        return subprocess.run(
            ["ruby", "-e", HARNESS, str(self.root / "android/fastlane/Fastfile"), lane],
            cwd=self.root, env=self.env, text=True, capture_output=True)

    def test_build_only_needs_no_play_credentials_and_cannot_upload(self):
        result = self.run_lane("build_bundle")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "upload.json").exists())
        self.assertEqual((self.root / "build-args.txt").read_text().splitlines(),
                         ["--build-only", "--bundle", "--variant", "restricted_file_accessRelease"])

    def test_uploads_only_aab_to_public_beta_as_draft(self):
        self.env["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON"] = '{"type":"service_account","private_key":"fixture"}'
        result = self.run_lane("play_beta")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        upload = json.loads((self.root / "upload.json").read_text())
        self.assertEqual(upload["track"], "beta")
        self.assertEqual(upload["release_status"], "draft")
        self.assertEqual(upload["package_name"], "com.snurhythm.asobmashow")
        self.assertEqual(upload["aab"], str(self.root / AAB))
        self.assertEqual(upload["json_key_data"], self.env["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON"])
        for key in ("skip_upload_apk", "skip_upload_metadata", "skip_upload_images",
                    "skip_upload_screenshots", "skip_upload_changelogs"):
            self.assertTrue(upload[key], key)
        self.assertNotIn("private_key", result.stdout + result.stderr)

    def test_missing_credentials_fails_before_build(self):
        result = self.run_lane("play_beta")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GOOGLE_PLAY_SERVICE_ACCOUNT_JSON", result.stderr)
        self.assertFalse((self.root / "build-args.txt").exists())
        self.assertFalse((self.root / "upload.json").exists())

    def test_failed_build_never_uploads_stale_artifact(self):
        self.env["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON"] = "{}"
        result = self.run_lane("play_beta", fail_build=True, stale_bundle=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "upload.json").exists())

    def test_missing_bundle_never_uploads_stale_artifact(self):
        self.env["GOOGLE_PLAY_SERVICE_ACCOUNT_JSON"] = "{}"
        result = self.run_lane("play_beta", omit_bundle=True, stale_bundle=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("AAB", result.stderr)
        self.assertFalse((self.root / "upload.json").exists())

    def test_bundle_flag_requires_build_only(self):
        result = subprocess.run([str(ROOT / "scripts/android_firebase_deploy.sh"), "--bundle"],
                                env=self.env, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--bundle requires --build-only", result.stderr)

    def test_bundle_build_uses_signed_release_task_and_propagates_failure(self):
        shutil.copyfile(ROOT / "scripts/android_firebase_deploy.sh",
                        self.root / "scripts/android_firebase_deploy.sh")
        (self.root / "scripts/android_firebase_deploy.sh").chmod(0o755)
        ndk = self.root / "sdk/ndk/28.2.13676358"
        (ndk / "build/cmake").mkdir(parents=True)
        (ndk / "build/cmake/android.toolchain.cmake").touch()
        (ndk / "source.properties").write_text("Pkg.Revision = 28.2.13676358\n")
        java = self.root / "java/bin/java"
        java.parent.mkdir(parents=True)
        java.write_text('#!/bin/sh\necho \'openjdk version "17.0.1"\' >&2\n')
        java.chmod(0o755)
        (self.root / "release.jks").touch()
        self.env.update({
            "ANDROID_HOME": str(self.root / "sdk"),
            "ANDROID_SDK_ROOT": str(self.root / "sdk"),
            "ANDROID_NDK_HOME": str(ndk), "VCPKG_ROOT": str(self.root),
            "JAVA_HOME": str(java.parent.parent),
            "ANDROID_KEYSTORE_PATH": "release.jks",
            "ANDROID_KEYSTORE_PASSWORD": "fixture-password",
            "ANDROID_KEY_ALIAS": "release", "ANDROID_KEY_PASSWORD": "fixture-password",
            "GITHUB_SHA": "fixture", "GITHUB_HEAD_REF": "fixture",
        })
        gradle = self.root / "android/gradlew"
        gradle.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > gradle-args.txt\n'
                          'printf "%s" "$ANDROID_KEYSTORE_PATH" > signing-path.txt\n'
                          'printf "%s" "$ANDROID_VERSION_CODE" > version-code.txt\n'
                          'exit "${FIXTURE_GRADLE_EXIT:-0}"\n')
        gradle.chmod(0o755)
        command = [str(self.root / "scripts/android_firebase_deploy.sh"), "--build-only", "--bundle"]
        result = subprocess.run(command, cwd=self.root, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "gradle-args.txt").read_text().splitlines(),
                         ["-p", str(self.root / "android"), ":app:bundleRestricted_file_accessRelease", "--no-daemon"])
        self.assertEqual((self.root / "signing-path.txt").read_text(), str(self.root / "release.jks"))
        self.assertEqual((self.root / "version-code.txt").read_text(), "1")
        self.env["GITHUB_RUN_NUMBER"] = "12"
        (self.root / ".env.local").write_text("GITHUB_RUN_NUMBER=''\n")
        for arguments, expected in (([], "12"), (["--version-code", "77"], "77")):
            result = subprocess.run(command + arguments, cwd=self.root, env=self.env,
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((self.root / "version-code.txt").read_text(), expected)
        self.env["ANDROID_VERSION_CODE"] = "88"
        result = subprocess.run(command, cwd=self.root, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "version-code.txt").read_text(), "88")
        self.env["FIXTURE_GRADLE_EXIT"] = "19"
        result = subprocess.run(command, cwd=self.root, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 19)

    def test_play_helper_preserves_ci_run_number_across_private_env_files(self):
        helper = self.root / "scripts/android_play_deploy.sh"
        shutil.copyfile(ROOT / "scripts/android_play_deploy.sh", helper)
        helper.chmod(0o755)
        (self.root / "android/.ruby-version").write_text("fixture\n")
        binaries = self.root / "bin"
        binaries.mkdir()
        for name, body in {
            "ruby": "#!/bin/sh\nexit 0\n",
            "bundle": '#!/bin/sh\n[ "$1" = check ] && exit 0\n'
                      'printf "%s" "$GITHUB_RUN_NUMBER" > "$FIXTURE_RUN_NUMBER_FILE"\n',
        }.items():
            path = binaries / name
            path.write_text(body)
            path.chmod(0o755)
        run_number_file = self.root / "run-number.txt"
        self.env.update({"PATH": str(binaries) + os.pathsep + self.env["PATH"],
                         "GITHUB_RUN_NUMBER": "12",
                         "FIXTURE_RUN_NUMBER_FILE": str(run_number_file)})
        for private_value in ("", "99"):
            with self.subTest(private_value=private_value):
                (self.root / "android/.env.local").write_text(
                    f"GITHUB_RUN_NUMBER='{private_value}'\n")
                result = subprocess.run([str(helper), "--build-only"], cwd=self.root,
                                        env=self.env, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(run_number_file.read_text(), "12")


if __name__ == "__main__":
    unittest.main()
