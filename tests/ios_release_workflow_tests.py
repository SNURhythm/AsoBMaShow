#!/usr/bin/env python3
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FASTFILE = ROOT / "ios/Xcode/AsoBMaShow/fastlane/Fastfile"
WORKFLOW = ROOT / ".github/workflows/mobile-beta-deploy.yml"
VERIFY_SCRIPT = ROOT / "scripts/ios_release_verify.sh"
DEPLOY_SCRIPT = ROOT / "scripts/ios_firebase_deploy.sh"


class IOSReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fastfile = FASTFILE.read_text(encoding="utf-8")
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.verify_script = VERIFY_SCRIPT.read_text(encoding="utf-8")

    def test_distribution_lanes_are_explicit_and_cannot_route_by_accident(self):
        self.assertIn("lane :firebase do", self.fastfile)
        self.assertIn("lane :testflight_release do", self.fastfile)
        self.assertNotIn("lane :beta do", self.fastfile)
        self.assertIn("github_pull_request_to_develop?", self.fastfile)
        self.assertIn("github_pull_request?", self.fastfile)

    def test_testflight_build_number_is_allocated_inside_serialized_lane(self):
        lane = self.fastfile.split("lane :testflight_release do", 1)[1]
        self.assertIn(
            "latest_testflight_build_number(initial_build_number: 0) + 1",
            lane,
        )
        self.assertIn("upload_to_testflight", lane)

    def test_workflow_verifies_before_either_ios_distribution(self):
        self.assertIn("ios-verify:", self.workflow)
        self.assertIn("./scripts/ios_release_verify.sh", self.workflow)
        for job in ("ios-firebase", "ios-testflight"):
            section = self.workflow.split(f"  {job}:", 1)[1]
            self.assertIn("needs: ios-verify", section)

    def test_testflight_distribution_is_non_canceling_and_globally_serialized(self):
        section = self.workflow.split("  ios-testflight:", 1)[1]
        self.assertIn("group: ios-testflight-distribution", section)
        self.assertIn("cancel-in-progress: false", section)

    def test_pull_requests_can_only_select_firebase(self):
        firebase = self.workflow.split("  ios-firebase:", 1)[1].split(
            "  ios-testflight:", 1
        )[0]
        testflight = self.workflow.split("  ios-testflight:", 1)[1].split(
            "  android-firebase:", 1
        )[0]
        self.assertIn("github.event_name == 'pull_request'", firebase)
        self.assertIn("github.base_ref == 'develop'", firebase)
        self.assertNotIn("pull_request", testflight)
        self.assertIn("fastlane ios firebase", firebase)
        self.assertIn("fastlane ios testflight_release", testflight)

    def test_verification_dry_run_contains_no_distribution_action(self):
        result = subprocess.run(
            [str(VERIFY_SCRIPT), "--dry-run"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        output = result.stdout
        self.assertIn("ios_build_setup_tests.py", output)
        self.assertIn("ios_release_workflow_tests.py", output)
        self.assertIn("ios_artifact_audit_tests.py", output)
        self.assertIn("ios_release_documentation_tests.py", output)
        self.assertIn("ios_artifact_audit.sh", output)
        self.assertIn("--build-only", output)
        self.assertNotIn("upload_to_testflight", output)
        self.assertNotIn("firebase_app_distribution", output)
        self.assertNotIn("fastlane ios", output)

    def test_release_verifier_includes_math_regressions(self):
        self.assertIn("quaternion_math_tests", self.verify_script)
        self.assertIn("foundation_math_quaternion", self.verify_script)

    def test_local_firebase_wrapper_calls_only_the_firebase_lane(self):
        script = DEPLOY_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("fastlane ios firebase", script)
        self.assertNotIn("fastlane ios beta", script)
        self.assertIn("IOS_BUILD_OUTPUT_PATH_FILE", script)


if __name__ == "__main__":
    unittest.main()
