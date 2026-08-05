#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${IOS_RELEASE_CMAKE_BUILD_DIR:-${ROOT_DIR}/cmake-build-debug}"
BUILD_JOBS="${IOS_RELEASE_BUILD_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 6)}"
DRY_RUN=0

NATIVE_TEST_TARGETS=(
  chart_repository_tests
  ir_credential_store_tests
  ir_credential_migration_tests
  pending_ir_credential_cleanup_tests
  video_frame_layout_tests
  video_decode_state_tests
  decoded_image_cache_tests
  image_decode_coordinator_tests
  jukebox_restore_tests
  quaternion_math_tests
  skin_path_policy_tests
  skin_tree_snapshotter_tests
  skin_archive_importer_tests
  skin_package_store_tests
  skin_package_operation_service_tests
  skin_commit_coordinator_tests
  skin_diagnostic_history_tests
  lua_skin_file_system_tests
  lua_skin_runtime_tests
  lua_skin_binding_decoder_tests
  lua_skin_table_decoder_tests
  lua_skin_host_modules_tests
  skin_resource_catalog_tests
  skin_live_resource_counters_tests
  skin_process_resident_memory_tests
  gameplay_skin_validator_tests
  play_skin_viewport_tests
  skin_destination_evaluator_tests
  beatoraja_skin_model_tests
  skin_draw_command_tests
  skin_quad_batch_renderer_tests
  skin_renderer_golden_tests
  playfield_visual_state_tests
  playfield_projection_tests
  play_skin_state_bridge_tests
  play_skin_session_tests
  playfield_presentation_coordinator_tests
  builtin_playfield_presentation_tests
  gameplay_skin_integration_tests
  skin_configuration_write_queue_tests
  realtime_touch_input_router_tests
  play_skin_touch_geometry_tests
  gameplay_bga_target_tests
  bgfx_skin_texture_device_tests
  app_settings_store_tests
  profile_settings_persistence_tests
  gameplay_skin_lifecycle_tests
  gameplay_skin_settings_tests
  gameplay_skin_settings_presentation_tests
  skin_performance_telemetry_tests
  skin_overlay_digest_provider_tests
  skin_acceptance_recorder_tests
  gameplay_skin_acceptance_controller_tests
  player_profile_manager_tests
  profile_switch_tests
  profile_archive_tests
  profile_settings_controller_tests
  profile_runtime_reapply_tests
)

NATIVE_CTEST_PATTERN='^(chart_repository_tests|ir_credential_store_tests|ir_credential_migration_tests|pending_ir_credential_cleanup_tests|video_frame_layout_tests|video_decode_state_tests|decoded_image_cache_tests|image_decode_coordinator_tests|foundation_av_jukebox_restore|foundation_math_quaternion|skin_path_policy_tests|skin_tree_snapshotter_tests|skin_archive_importer_tests|skin_package_store_tests|skin_package_operation_service_tests|skin_commit_coordinator_tests|skin_diagnostic_history_tests|lua_skin_file_system_tests|lua_skin_runtime_tests|lua_skin_binding_decoder_tests|lua_skin_table_decoder_tests|lua_skin_host_modules_tests|skin_resource_catalog_tests|skin_live_resource_counters_tests|skin_process_resident_memory_tests|gameplay_skin_validator_tests|play_skin_viewport_tests|skin_destination_evaluator_tests|beatoraja_skin_model_tests|skin_draw_command_tests|skin_quad_batch_renderer_tests|skin_renderer_golden_tests|image_fade_shader_audit|shader_compile_workflow_audit|playfield_visual_state_tests|playfield_projection_tests|play_skin_state_bridge_tests|play_skin_session_tests|playfield_presentation_coordinator_tests|builtin_playfield_presentation_tests|gameplay_skin_integration_tests|skin_configuration_write_queue_tests|realtime_touch_input_router_tests|play_skin_touch_geometry_tests|gameplay_bga_target_tests|bgfx_skin_texture_device_tests|foundation_profile_settings|foundation_profile_settings_persistence|gameplay_skin_lifecycle_tests|gameplay_skin_settings_tests|gameplay_skin_settings_presentation_tests|skin_performance_telemetry_tests|skin_overlay_digest_provider_tests|skin_acceptance_recorder_tests|gameplay_skin_acceptance_controller_tests|foundation_profile_manager|foundation_profile_switch|foundation_profile_archive|foundation_profile_controller|foundation_profile_runtime)$'

usage() {
  cat <<'USAGE'
Usage: scripts/ios_release_verify.sh [--dry-run]

Runs release-critical native tests, iOS release-contract tests, and an unsigned
iOS build. It never archives, signs, or uploads a distribution artifact.
USAGE
}

if [ "${1:-}" = "--dry-run" ]; then
  DRY_RUN=1
  shift
fi
if [ "$#" -ne 0 ]; then
  usage >&2
  exit 2
fi

run() {
  if [ "${DRY_RUN}" -eq 1 ]; then
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

cd "${ROOT_DIR}"
if [ "${DRY_RUN}" -eq 1 ] || [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
  run cmake --preset debug -B "${BUILD_DIR}"
fi
run cmake --build "${BUILD_DIR}" --target \
  "${NATIVE_TEST_TARGETS[@]}" -j "${BUILD_JOBS}"
run ctest --test-dir "${BUILD_DIR}" \
  -R "${NATIVE_CTEST_PATTERN}" \
  --output-on-failure
run python3 tests/ios_build_setup_tests.py
run python3 tests/ios_release_workflow_tests.py
run python3 tests/ios_artifact_audit_tests.py
run python3 tests/ios_release_documentation_tests.py
if [ "${DRY_RUN}" -eq 1 ]; then
  run scripts/ios_firebase_deploy.sh --build-only
  run scripts/ios_artifact_audit.sh IOS_BUILD_OUTPUT_APP_PATH
else
  BUILD_OUTPUT_PATH_FILE="$(mktemp "${TMPDIR:-/tmp}/asobmashow-ios-build-path.XXXXXX")"
  trap 'rm -f "${BUILD_OUTPUT_PATH_FILE}"' EXIT
  IOS_BUILD_OUTPUT_PATH_FILE="${BUILD_OUTPUT_PATH_FILE}" \
    scripts/ios_firebase_deploy.sh --build-only
  [ -s "${BUILD_OUTPUT_PATH_FILE}" ] || {
    echo "iOS build did not publish its output app path" >&2
    exit 1
  }
  APP_PATH="${IOS_RELEASE_APP_PATH:-$(<"${BUILD_OUTPUT_PATH_FILE}")}"
  scripts/ios_artifact_audit.sh "${APP_PATH}"
fi
