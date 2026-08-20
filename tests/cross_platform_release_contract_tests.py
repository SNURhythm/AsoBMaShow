#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class CrossPlatformReleaseContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = read("CMakeLists.txt")
        cls.src_cmake = read("src/CMakeLists.txt")
        cls.info_plist = read("Info.plist")
        cls.macos_init = read("scripts/macos_init.sh")
        cls.macos_workflow = read(".github/workflows/macos-build.yml")
        cls.mobile_workflow = read(".github/workflows/mobile-beta-deploy.yml")
        cls.android_gradle = read("android/app/build.gradle")
        cls.android_manifest = read("android/app/src/main/AndroidManifest.xml")
        cls.android_deploy = read("scripts/android_firebase_deploy.sh")
        cls.main = read("src/main.cpp")
        cls.audio_decoder = read("src/audio/decoder.cpp")
        cls.replay_store = read("src/replay/ReplayFileStore.cpp")
        cls.macos_triplet = read("vcpkg-triplets/arm64-osx-asobmashow.cmake")
        cls.play_skin_state_bridge = read(
            "src/skin/beatoraja/PlaySkinStateBridge.cpp"
        )
        cls.skin_2d_renderer = read("src/skin/beatoraja/Skin2DRenderer.cpp")
        cls.skin_destination_evaluator = read(
            "src/skin/beatoraja/SkinDestinationEvaluator.cpp"
        )
        cls.msvc_test_diagnostics = read("tests/support/MsvcTestDiagnostics.cpp")

    def test_public_version_is_0_0_1_on_desktop_and_android(self):
        self.assertRegex(
            self.cmake,
            r"project\(AsoBMaShow VERSION 0\.0\.1 LANGUAGES C CXX\)",
        )
        self.assertIn("<string>@PROJECT_VERSION@</string>", self.info_plist)
        self.assertIn("findProperty('androidVersionName') ?: '0.0.1'", self.android_gradle)
        self.assertIn(
            '"-DASOBMASHOW_APPLICATION_VERSION=${androidVersionNameText}"',
            self.android_gradle,
        )
        self.assertIn(
            'ASOBMASHOW_APPLICATION_VERSION "${PROJECT_VERSION}" CACHE STRING',
            self.cmake,
        )
        self.assertIn('export ANDROID_VERSION_NAME="0.0.1"', self.android_deploy)
        self.assertNotIn('ANDROID_VERSION_NAME="1.0.${ANDROID_VERSION_CODE}"', self.android_deploy)

    def test_android_automatic_build_identity_has_second_resolution(self):
        version_function = self.android_deploy.split(
            "android_timestamp_version_code()", 1
        )[1].split("apply_cli_overrides()", 1)[0]
        self.assertIn("date -u +%s", version_function)
        self.assertNotIn("+%y%j%H%M", version_function)

        android_job = self.mobile_workflow.split("  android-firebase:", 1)[1]
        self.assertIn("group: android-firebase-distribution", android_job)
        self.assertIn("cancel-in-progress: false", android_job)

    def test_android_enables_libcxx_stop_token_support(self):
        android_target = self.cmake.split(
            "target_sources(main PRIVATE "
            "$<TARGET_OBJECTS:asobmashow_build_identity>)",
            1,
        )[1].split(
            "target_compile_definitions(main PRIVATE\n"
            "    ASOBMASHOW_ENABLE_PERF_TELEMETRY",
            1,
        )[0]
        self.assertIn("_LIBCPP_ENABLE_EXPERIMENTAL", android_target)

    def test_android_launcher_uses_the_approved_application_icon(self):
        self.assertIn('android:icon="@mipmap/ic_launcher"', self.android_manifest)
        icon = ROOT / "android/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png"
        self.assertTrue(icon.is_file())
        self.assertGreater(icon.stat().st_size, 1024)

    def test_macos_release_has_an_explicit_overridable_minimum(self):
        self.assertIn(
            'MACOS_DEPLOYMENT_TARGET="${ASOBMASHOW_MACOS_DEPLOYMENT_TARGET:-13.0}"',
            self.macos_init,
        )
        self.assertIn('export MACOSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"', self.macos_init)
        self.assertIn('-DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"', self.macos_init)
        self.assertIn("-DVCPKG_TARGET_TRIPLET=arm64-osx-asobmashow", self.macos_init)
        self.assertIn('-DVCPKG_OVERLAY_TRIPLETS="${ROOT_DIR}/vcpkg-triplets"', self.macos_init)
        self.assertIn('set(VCPKG_OSX_DEPLOYMENT_TARGET "12.0")', self.macos_triplet)

    def test_macos_bundle_uses_imported_zlib_and_an_icon(self):
        self.assertIn("find_package(ZLIB REQUIRED)", self.cmake)
        self.assertIn("ZLIB::ZLIB", self.cmake)
        self.assertNotRegex(self.cmake, r"\bbz2 z iconv\b")
        self.assertIn("$<$<CONFIG:Release>:LINKER:-fatal_warnings>", self.cmake)
        static_release_bundle_guard = (
            'if (BUILD_MACOS_BUNDLE\n'
            '        AND CMAKE_BUILD_TYPE STREQUAL "Release"\n'
            '        AND VCPKG_TARGET_TRIPLET STREQUAL "arm64-osx-asobmashow")'
        )
        self.assertIn(static_release_bundle_guard, self.cmake)
        guarded_rpath = self.cmake.split(static_release_bundle_guard, 1)[1].split(
            "endif()", 1
        )[0]
        self.assertIn(
            "set_target_properties(main PROPERTIES SKIP_BUILD_RPATH TRUE)",
            guarded_rpath,
        )
        self.assertIn("MACOSX_BUNDLE_ICON_FILE", self.cmake)
        self.assertIn("<key>CFBundleIconFile</key>", self.info_plist)
        self.assertIn("@MACOS_BUNDLE_ICON_FILE@", self.info_plist)

    def test_release_build_does_not_force_debug_information(self):
        common_non_msvc = self.cmake.split("if (MSVC)", 1)[1].split(
            "# ------------------------------------------------------------------------------\n# Dependency Management",
            1,
        )[0]
        self.assertNotRegex(common_non_msvc, r"add_compile_options\([^\)]*\s-g(?:\s|\))")

    def test_msvc_parallel_builds_serialize_compiler_pdb_writes(self):
        msvc = self.cmake.split("if (MSVC)", 1)[1].split("else ()", 1)[0]
        self.assertRegex(msvc, r"add_compile_options\([^\)]*/MP")
        self.assertRegex(msvc, r"add_compile_options\([^\)]*/FS")

    def test_skin_runtime_integer_arithmetic_is_msvc_portable(self):
        for source in (
            self.play_skin_state_bridge,
            self.skin_2d_renderer,
            self.skin_destination_evaluator,
        ):
            self.assertNotIn("__int128", source)

    def test_msvc_test_failures_stay_in_the_cli(self):
        for token in (
            "_CrtSetReportMode",
            "_CRTDBG_FILE_STDERR",
            "_set_abort_behavior",
            "SEM_NOGPFAULTERRORBOX",
        ):
            self.assertIn(token, self.msvc_test_diagnostics)
        register_function = self.cmake.split(
            "function(asobmashow_register_test target_name)", 1
        )[1].split("endfunction()", 1)[0]
        self.assertIn("tests/support/MsvcTestDiagnostics.cpp", register_function)
        self.assertIn(
            "target_sources(skin_package_operation_service_tests PRIVATE",
            self.cmake,
        )
        self.assertIn(
            "target_sources(skin_commit_coordinator_tests PRIVATE", self.cmake
        )

    def test_msvc_app_failures_stay_in_the_cli(self):
        diagnostics_path = ROOT / "src/MsvcCliDiagnostics.cpp"
        self.assertTrue(diagnostics_path.is_file())
        diagnostics = diagnostics_path.read_text(encoding="utf-8")
        for token in (
            "_CrtSetReportMode",
            "_CRTDBG_FILE_STDERR",
            "_set_abort_behavior",
            "SEM_NOGPFAULTERRORBOX",
        ):
            self.assertIn(token, diagnostics)
        self.assertIn("MsvcCliDiagnostics.cpp", self.src_cmake)

    def test_release_test_binaries_keep_assertion_based_fixture_setup(self):
        register_function = self.cmake.split(
            "function(asobmashow_register_test target_name)", 1
        )[1].split("endfunction()", 1)[0]
        self.assertIn("target_compile_options(${target_name} PRIVATE -UNDEBUG)", register_function)

    def test_performance_telemetry_is_compile_time_opt_in(self):
        self.assertIn("option(ASOBMASHOW_ENABLE_PERF_TELEMETRY", self.cmake)
        self.assertIn("ASOBMASHOW_ENABLE_PERF_TELEMETRY=$<BOOL:", self.cmake)
        self.assertIn(
            "#ifndef ASOBMASHOW_ENABLE_PERF_TELEMETRY\n"
            "#define ASOBMASHOW_ENABLE_PERF_TELEMETRY 0\n"
            "#endif",
            self.main,
        )
        self.assertIn("#if ASOBMASHOW_ENABLE_PERF_TELEMETRY", self.main)
        self.assertNotIn("constexpr bool kEnablePerfTelemetry = true", self.main)

    def test_macos_shader_inputs_have_a_real_build_dependency(self):
        self.assertIn("add_custom_target(copy_macos_bundle_shaders", self.cmake)
        self.assertIn("add_dependencies(main copy_macos_bundle_shaders)", self.cmake)
        target_post_build = re.findall(
            r"add_custom_command\(\s*TARGET main POST_BUILD(?P<body>.*?)\n\s*\)",
            self.cmake,
            flags=re.DOTALL,
        )
        self.assertTrue(target_post_build)
        self.assertTrue(all("DEPENDS" not in body for body in target_post_build))

    def test_first_party_portability_diagnostics_are_type_correct(self):
        self.assertIn("PRId64", self.audio_decoder)
        initializer = self.replay_store.split(
            "outcome.observedMetadata = ReplayFileMetadata{", 1
        )[1].split("};", 1)[0]
        self.assertLess(initializer.index(".sha256"), initializer.index(".compressedSize"))

    def test_macos_ci_runs_tests_and_audits_before_packaging(self):
        build_job = self.macos_workflow.split("jobs:", 1)[1]
        ctest_index = build_job.index("ctest --test-dir")
        audit_index = build_job.index("scripts/macos_artifact_audit.sh")
        zip_index = build_job.index("ditto -c -k")
        self.assertLess(ctest_index, audit_index)
        self.assertLess(audit_index, zip_index)
        self.assertIn("tests/cross_platform_release_contract_tests.py", build_job)
        self.assertIn("tests/macos_artifact_audit_tests.py", build_job)

    def test_macos_ci_pins_vcpkg_and_has_strict_tag_release_gates(self):
        workflow_scope = self.macos_workflow.split("jobs:", 1)[0]
        build_job = self.macos_workflow.split("jobs:", 1)[1]
        self.assertNotIn("runner.temp", workflow_scope)
        self.assertNotIn("${{ runner.temp }}", build_job)
        self.assertIn(
            'ASOBMASHOW_MACOS_BUILD_DIR=$RUNNER_TEMP/asobmashow-macos-'
            '$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT',
            build_job,
        )
        self.assertIn('>> "$GITHUB_ENV"', build_job)
        self.assertIn("VCPKG_TOOL_COMMIT:", self.macos_workflow)
        self.assertIn("GITHUB_RUN_ATTEMPT", self.macos_workflow)
        self.assertRegex(self.macos_workflow, r"git .*checkout --detach")
        self.assertNotIn("git pull", self.macos_workflow)
        self.assertIn("codesign --force --deep --options runtime", self.macos_workflow)
        self.assertIn("xcrun notarytool submit", self.macos_workflow)
        self.assertIn("xcrun stapler staple", self.macos_workflow)
        self.assertIn("--require-signature", self.macos_workflow)
        self.assertIn("--require-gatekeeper", self.macos_workflow)

    def test_firebase_android_lane_remains_fast_iteration(self):
        android_job = self.mobile_workflow.split("  android-firebase:", 1)[1]
        self.assertNotIn("ctest --test-dir", android_job)
        self.assertNotIn("scripts/macos_artifact_audit.sh", android_job)


if __name__ == "__main__":
    unittest.main()
