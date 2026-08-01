#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRIVACY = ROOT / "PRIVACY_POLICY.md"
CHECKLIST = ROOT / "docs/ios-first-release-checklist.md"
README = ROOT / "README.md"


class IOSReleaseDocumentationTests(unittest.TestCase):
    def test_privacy_policy_matches_shipped_credential_and_transport_behavior(self):
        policy = PRIVACY.read_text(encoding="utf-8")
        normalized_policy = policy.casefold()
        for required in (
            "Apple Keychain",
            "AfterFirstUnlockThisDeviceOnly",
            "transactional legacy migration",
            "authenticated IR operations require HTTPS",
            "anonymous public ranking requests",
            "HTTP difficulty-table",
        ):
            self.assertIn(required.casefold(), normalized_policy)
        self.assertNotIn("does not store the key in Apple Keychain", policy)
        self.assertNotIn("API key and gameplay data would not be protected", policy)

    def test_ios_first_release_checklist_covers_manual_app_store_gates(self):
        checklist = CHECKLIST.read_text(encoding="utf-8")
        normalized_checklist = checklist.casefold()
        for required in (
            "Privacy labels",
            "Age rating",
            "Support URL",
            "Privacy policy URL",
            "Export compliance",
            "Content rights",
            "Review notes",
            "iPhone screenshots",
            "iPad screenshots",
            "signed archive",
            "physical iPhone",
            "physical iPad",
            "privacy manifests are intentionally omitted",
        ):
            self.assertIn(required.casefold(), normalized_checklist)

    def test_readme_uses_the_active_mobile_release_workflow_badge(self):
        readme = README.read_text(encoding="utf-8")
        self.assertIn("mobile-beta-deploy.yml/badge.svg", readme)
        self.assertNotIn("ios-testflight.yml/badge.svg", readme)


if __name__ == "__main__":
    unittest.main()
