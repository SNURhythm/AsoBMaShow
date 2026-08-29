#!/usr/bin/env python3
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FASTFILE = ROOT / "ios/Xcode/AsoBMaShow/fastlane/Fastfile"
WORKFLOW = ROOT / ".github/workflows/mobile-beta-deploy.yml"
VERIFY_SCRIPT = ROOT / "scripts/ios_release_verify.sh"
DEPLOY_SCRIPT = ROOT / "scripts/ios_firebase_deploy.sh"
GEMFILE_LOCK = ROOT / "ios/Xcode/AsoBMaShow/Gemfile.lock"

RELEASE_CRITICAL_SKIN_TESTS = {
    "skin_path_policy_tests": "skin_path_policy_tests",
    "skin_tree_snapshotter_tests": "skin_tree_snapshotter_tests",
    "skin_archive_importer_tests": "skin_archive_importer_tests",
    "skin_package_store_tests": "skin_package_store_tests",
    "skin_package_operation_service_tests": "skin_package_operation_service_tests",
    "skin_commit_coordinator_tests": "skin_commit_coordinator_tests",
    "skin_diagnostic_history_tests": "skin_diagnostic_history_tests",
    "lua_skin_file_system_tests": "lua_skin_file_system_tests",
    "lua_skin_runtime_tests": "lua_skin_runtime_tests",
    "lua_skin_binding_decoder_tests": "lua_skin_binding_decoder_tests",
    "lua_skin_table_decoder_tests": "lua_skin_table_decoder_tests",
    "lua_skin_host_modules_tests": "lua_skin_host_modules_tests",
    "skin_resource_catalog_tests": "skin_resource_catalog_tests",
    "skin_live_resource_counters_tests": "skin_live_resource_counters_tests",
    "skin_process_resident_memory_tests": "skin_process_resident_memory_tests",
    "gameplay_skin_validator_tests": "gameplay_skin_validator_tests",
    "play_skin_viewport_tests": "play_skin_viewport_tests",
    "skin_destination_evaluator_tests": "skin_destination_evaluator_tests",
    "beatoraja_skin_model_tests": "beatoraja_skin_model_tests",
    "skin_draw_command_tests": "skin_draw_command_tests",
    "skin_quad_batch_renderer_tests": "skin_quad_batch_renderer_tests",
    "skin_renderer_golden_tests": "skin_renderer_golden_tests",
    "playfield_visual_state_tests": "playfield_visual_state_tests",
    "playfield_projection_tests": "playfield_projection_tests",
    "play_skin_state_bridge_tests": "play_skin_state_bridge_tests",
    "play_skin_session_tests": "play_skin_session_tests",
    "playfield_presentation_coordinator_tests": "playfield_presentation_coordinator_tests",
    "builtin_playfield_presentation_tests": "builtin_playfield_presentation_tests",
    "gameplay_skin_integration_tests": "gameplay_skin_integration_tests",
    "skin_configuration_write_queue_tests": "skin_configuration_write_queue_tests",
    "realtime_touch_input_router_tests": "realtime_touch_input_router_tests",
    "play_skin_touch_geometry_tests": "play_skin_touch_geometry_tests",
    "gameplay_bga_target_tests": "gameplay_bga_target_tests",
    "bgfx_skin_texture_device_tests": "bgfx_skin_texture_device_tests",
    "app_settings_store_tests": "foundation_profile_settings",
    "profile_settings_persistence_tests": "foundation_profile_settings_persistence",
    "gameplay_skin_lifecycle_tests": "gameplay_skin_lifecycle_tests",
    "gameplay_skin_settings_tests": "gameplay_skin_settings_tests",
    "gameplay_skin_settings_presentation_tests": "gameplay_skin_settings_presentation_tests",
    "skin_performance_telemetry_tests": "skin_performance_telemetry_tests",
    "skin_overlay_digest_provider_tests": "skin_overlay_digest_provider_tests",
    "skin_acceptance_recorder_tests": "skin_acceptance_recorder_tests",
    "gameplay_skin_acceptance_controller_tests": "gameplay_skin_acceptance_controller_tests",
    "profile_switch_tests": "foundation_profile_switch",
    "profile_settings_controller_tests": "foundation_profile_controller",
    "profile_runtime_reapply_tests": "foundation_profile_runtime",
}

RELEASE_CRITICAL_PROFILE_TESTS = {
    "player_profile_manager_tests": {
        "foundation_profile_manager_bootstrap",
        "foundation_profile_manager_deletion",
        "foundation_profile_manager_integrity",
    },
    "profile_archive_tests": {
        "foundation_profile_archive_portable",
        "foundation_profile_archive_validation",
        "foundation_profile_archive_transactions",
        "foundation_profile_archive_faults",
    },
}

RELEASE_CRITICAL_PROFILE_INVALID_TESTS = {
    "profile_manager_invalid_shard",
    "profile_archive_invalid_shard",
}


def native_test_targets(script):
    block = re.search(r"NATIVE_TEST_TARGETS=\(\n(.*?)\n\)", script, re.DOTALL)
    if block is None:
        raise AssertionError("NATIVE_TEST_TARGETS block is missing")
    return re.findall(r"^  ([a-z0-9_]+)$", block.group(1), re.MULTILINE)


class IOSReleaseWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fastfile = FASTFILE.read_text(encoding="utf-8")
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.verify_script = VERIFY_SCRIPT.read_text(encoding="utf-8")
        cls.gemfile_lock = GEMFILE_LOCK.read_text(encoding="utf-8")

    def test_release_tool_dependencies_include_known_security_fixes(self):
        minimum_versions = {
            "activesupport": (7, 2, 3, 1),
            "concurrent-ruby": (1, 3, 7),
            "excon": (1, 5, 0),
            "faraday": (1, 10, 6),
            "fastlane": (2, 238, 0),
            "json": (2, 19, 9),
            "jwt": (2, 10, 3),
        }
        for gem_name, minimum in minimum_versions.items():
            with self.subTest(gem=gem_name):
                match = re.search(
                    rf"^    {re.escape(gem_name)} \((\d+(?:\.\d+)+)\)$",
                    self.gemfile_lock,
                    flags=re.MULTILINE,
                )
                self.assertIsNotNone(match, f"{gem_name} is not locked")
                locked = tuple(int(part) for part in match.group(1).split("."))
                width = max(len(locked), len(minimum))
                self.assertGreaterEqual(
                    locked + (0,) * (width - len(locked)),
                    minimum + (0,) * (width - len(minimum)),
                )

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

    def test_testflight_audits_the_final_signed_ipa_before_upload(self):
        lane = self.fastfile.split("lane :testflight_release do", 1)[1].split(
            "\n  end\nend", 1
        )[0]
        post_build = lane.index("temporary_fix_ios_post_build")
        artifact_audit = lane.index("audit_distribution_artifact")
        upload = lane.index("upload_to_testflight")

        self.assertLess(post_build, artifact_audit)
        self.assertLess(artifact_audit, upload)
        self.assertIn("SharedValues::IPA_OUTPUT_PATH", lane)
        self.assertIn("--require-signature", self.fastfile)

    def test_distribution_builds_embed_and_audit_the_checkout_identity(self):
        self.assertIn("def release_build_identity", self.fastfile)
        self.assertIn('"rev-parse", "HEAD"', self.fastfile)
        self.assertIn('"status", "--porcelain"', self.fastfile)
        self.assertIn("def build_identity_xcargs", self.fastfile)
        self.assertIn("def audit_distribution_artifact", self.fastfile)

        for lane_name in ("firebase", "testflight_release"):
            with self.subTest(lane=lane_name):
                lane = self.fastfile.split(f"lane :{lane_name} do", 1)[1].split(
                    "\n  end\nend", 1
                )[0]
                self.assertIn("release_build_identity", lane)
                self.assertIn("build_identity_xcargs", lane)
                self.assertIn("audit_distribution_artifact", lane)

    def test_firebase_pr_bypasses_release_verification_but_testflight_does_not(self):
        verify = self.workflow.split("  ios-verify:", 1)[1].split(
            "  ios-firebase:", 1
        )[0]
        firebase = self.workflow.split("  ios-firebase:", 1)[1].split(
            "  ios-testflight:", 1
        )[0]
        testflight = self.workflow.split("  ios-testflight:", 1)[1].split(
            "  android-firebase:", 1
        )[0]

        self.assertIn("./scripts/ios_release_verify.sh", verify)
        self.assertIn(
            "github.event_name != 'pull_request' || github.base_ref != 'develop'",
            verify,
        )
        self.assertNotIn("needs: ios-verify", firebase)
        self.assertIn("needs: ios-verify", testflight)

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

    def test_fresh_custom_verifier_build_directory_is_configured_in_place(self):
        with tempfile.TemporaryDirectory() as temp:
            build_dir = Path(temp) / "fresh-build"
            env = dict(os.environ)
            env["IOS_RELEASE_CMAKE_BUILD_DIR"] = str(build_dir)
            result = subprocess.run(
                [str(VERIFY_SCRIPT), "--dry-run"],
                cwd=ROOT,
                env=env,
                check=True,
                text=True,
                capture_output=True,
            )

        configure = next(
            line for line in result.stdout.splitlines()
            if line.startswith("+ cmake ") and "--preset debug" in line
        )
        self.assertIn(f"-B {build_dir}", configure)
        self.assertIn(f"cmake --build {build_dir}", result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "zsh is required")
    def test_verification_dry_run_is_identical_under_bash_and_direct_zsh(self):
        outputs = []
        for shell in ("bash", "zsh"):
            result = subprocess.run(
                [shell, str(VERIFY_SCRIPT), "--dry-run"],
                cwd=ROOT,
                check=True,
                text=True,
                capture_output=True,
            )
            outputs.append(result.stdout)
            self.assertEqual(result.stderr, "")

        self.assertEqual(outputs[0], outputs[1])
        self.assertIn(f"cmake --build {ROOT / 'cmake-build-debug'}", outputs[1])

    def test_release_verifier_includes_math_regressions(self):
        self.assertIn("quaternion_math_tests", self.verify_script)
        self.assertIn("foundation_math_quaternion", self.verify_script)

    def test_release_verifier_includes_durable_credential_cleanup(self):
        self.assertGreaterEqual(
            self.verify_script.count("pending_ir_credential_cleanup_tests"),
            2,
            "the cleanup regression must be built and selected by CTest",
        )

    def test_release_verifier_builds_and_runs_release_critical_skin_surface(self):
        for target, registered_test in RELEASE_CRITICAL_SKIN_TESTS.items():
            with self.subTest(target=target):
                self.assertIn(target, self.verify_script)
                self.assertIn(registered_test, self.verify_script)
        for registered_audit in (
            "image_fade_shader_audit",
            "shader_compile_workflow_audit",
        ):
            with self.subTest(registered_audit=registered_audit):
                self.assertIn(registered_audit, self.verify_script)

    def test_release_verifier_selects_complete_profile_shard_collections(self):
        pattern = re.search(
            r"NATIVE_CTEST_PATTERN='([^']+)'", self.verify_script
        ).group(1)
        registered_names = set(pattern.removeprefix("^(").removesuffix(")$").split("|"))
        profile_family = {
            name
            for name in registered_names
            if name.startswith(("foundation_profile_manager", "foundation_profile_archive"))
        }
        expected_profile_names = set().union(*RELEASE_CRITICAL_PROFILE_TESTS.values())
        self.assertEqual(profile_family, expected_profile_names)
        self.assertNotEqual(
            profile_family | {"foundation_profile_manager", "foundation_profile_archive"},
            expected_profile_names,
        )
        for target, expected_names in RELEASE_CRITICAL_PROFILE_TESTS.items():
            with self.subTest(target=target):
                self.assertIn(target, self.verify_script)
        self.assertTrue(
            RELEASE_CRITICAL_PROFILE_INVALID_TESTS.isdisjoint(registered_names)
        )

    def test_release_verifier_runs_native_checks_in_parallel(self):
        self.assertIn(
            'run ctest --test-dir "${BUILD_DIR}" \\\n'
            '  -R "${NATIVE_CTEST_PATTERN}" \\\n'
            '  --output-on-failure \\\n'
            '  --parallel "${BUILD_JOBS}"',
            self.verify_script,
        )

    def test_release_verifier_builds_each_profile_test_once_in_native_targets(self):
        duplicate = self.verify_script.replace(
            "  profile_archive_tests\n",
            "  profile_archive_tests\n  profile_archive_tests\n",
            1,
        )
        self.assertEqual(
            native_test_targets(duplicate).count("profile_archive_tests"),
            2,
            "the NATIVE_TEST_TARGETS parser detects an artificial duplicate",
        )

        targets = native_test_targets(self.verify_script)
        for target in RELEASE_CRITICAL_PROFILE_TESTS:
            with self.subTest(target=target):
                self.assertEqual(
                    targets.count(target),
                    1,
                    "each profile executable occurs once in NATIVE_TEST_TARGETS",
                )

    def test_local_firebase_wrapper_calls_only_the_firebase_lane(self):
        script = DEPLOY_SCRIPT.read_text(encoding="utf-8")
        self.assertIn("fastlane ios firebase", script)
        self.assertNotIn("fastlane ios beta", script)
        self.assertIn("IOS_BUILD_OUTPUT_PATH_FILE", script)


if __name__ == "__main__":
    unittest.main()
