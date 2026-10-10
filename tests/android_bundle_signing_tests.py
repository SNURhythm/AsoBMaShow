#!/usr/bin/env python3
"""Verify upload signing with real JDK tools and disposable test keys."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts/android_sign_bundle.py"


class AndroidBundleSigningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.env = dict(os.environ, TEST_SIGNING_PASSWORD="fixture-only-password")
        java_home = cls.env.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        if java_home:
            cls.env["JAVA_HOME"] = java_home
        cls.keytool = str(Path(java_home) / "bin/keytool") if java_home else "keytool"
        cls.jarsigner = str(Path(java_home) / "bin/jarsigner") if java_home else "jarsigner"
        for alias in ("app", "upload"):
            subprocess.run([cls.keytool, "-genkeypair", "-noprompt", "-storetype", "JKS",
                            "-keystore", str(cls.root / f"{alias}.jks"), "-alias", alias,
                            "-storepass:env", "TEST_SIGNING_PASSWORD",
                            "-keypass:env", "TEST_SIGNING_PASSWORD", "-keyalg", "RSA",
                            "-keysize", "2048", "-validity", "10000", "-dname", f"CN={alias}"],
                           env=cls.env, check=True, capture_output=True)

    def setUp(self):
        self.work = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.work.cleanup)
        self.bundle = Path(self.work.name) / "test bundle.aab"
        self.payload = {"base/manifest/AndroidManifest.xml": b"fixture manifest",
                        "base/dex/classes.dex": b"fixture dex",
                        "BUNDLE-METADATA/example/info": b"keep metadata",
                        "META-INF/services/example": b"keep services",
                        "META-INF/nested/resource.SF": b"keep nested resource"}
        with zipfile.ZipFile(self.bundle, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in self.payload.items():
                archive.writestr(name, data)
        self.env = dict(self.__class__.env,
                        ANDROID_UPLOAD_KEYSTORE_PATH=str(self.root / "upload.jks"),
                        ANDROID_UPLOAD_KEYSTORE_PASSWORD="fixture-only-password",
                        ANDROID_UPLOAD_KEY_ALIAS="upload",
                        ANDROID_UPLOAD_KEY_PASSWORD="fixture-only-password")

    def sign_as_app(self):
        subprocess.run([self.jarsigner, "-keystore", str(self.root / "app.jks"),
                        "-storepass:env", "TEST_SIGNING_PASSWORD",
                        "-keypass:env", "TEST_SIGNING_PASSWORD", str(self.bundle), "app"],
                       env=self.env, check=True, capture_output=True)

    def run_helper(self, *arguments):
        return subprocess.run(["python3", str(HELPER), *arguments, str(self.bundle)],
                              env=self.env, text=True, capture_output=True)

    def test_replaces_app_signature_with_upload_signature_and_preserves_payload(self):
        self.sign_as_app()
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.bundle) as archive:
            for name, data in self.payload.items():
                self.assertEqual(archive.read(name), data)
            self.assertNotIn("META-INF/APP.RSA", archive.namelist())
            self.assertEqual(len([name for name in archive.namelist() if name.endswith(".RSA")]), 1)
        result = self.run_helper("--verify-only")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.env["ANDROID_UPLOAD_KEYSTORE_PATH"] = str(self.root / "app.jks")
        self.env["ANDROID_UPLOAD_KEY_ALIAS"] = "app"
        self.assertNotEqual(self.run_helper("--verify-only").returncode, 0)

    def test_verify_rejects_unsigned_wrong_signer_and_tampered_bundles(self):
        self.assertNotEqual(self.run_helper("--verify-only").returncode, 0)
        self.sign_as_app()
        self.assertNotEqual(self.run_helper("--verify-only").returncode, 0)
        result = self.run_helper()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.bundle, "a") as archive:
            archive.writestr("unsigned-new-entry", b"tampered")
        self.assertNotEqual(self.run_helper("--verify-only").returncode, 0)

    def test_failed_signing_preserves_input_and_does_not_expose_password(self):
        self.sign_as_app()
        original = self.bundle.read_bytes()
        self.env["ANDROID_UPLOAD_KEY_PASSWORD"] = "incorrect-secret-password"
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.bundle.read_bytes(), original)
        self.assertNotIn("incorrect-secret-password", result.stdout + result.stderr)
        self.assertEqual(list(self.bundle.parent.iterdir()), [self.bundle])

    def test_missing_credentials_never_fall_back_to_app_key(self):
        self.env["ANDROID_KEYSTORE_PATH"] = str(self.root / "app.jks")
        del self.env["ANDROID_UPLOAD_KEYSTORE_PATH"]
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ANDROID_UPLOAD_KEYSTORE_PATH", result.stderr)


if __name__ == "__main__":
    unittest.main()
