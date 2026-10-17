# Subsystem membership is explicit so a new test cannot silently miss focused runs.
# Names absent on this platform are intentionally ignored. See docs/testing/README.md.
include("${CMAKE_CURRENT_LIST_DIR}/TestGroups.cmake")

asobmashow_label_tests(area_core
    curl_redirect_policy_tests
    file_checksum_tests
    foundation_math_quaternion
    parallel_work_tests
    stable_hash_tests
    thread_compat_fallback_tests
    thread_compat_tests
    worker_pool_startup_failure_tests
)

asobmashow_label_tests(area_app
    app_database_initializer_tests
    application_event_queue_tests
    application_result_recovery_tests
    application_startup_tests
    application_ui_state_store_tests
    background_gameplay_dispatch_tests
    intro_scene_lifecycle_tests
    intro_scene_navigation_tests
    ios_application_lifecycle_tests
    ios_export_input_suppression_tests
    ios_foreground_ingress_tests
    scene_return_target_tests
    sdl_application_runtime_tests
    settings_event_lifetime_tests
)

asobmashow_label_tests(area_audio
    android_audio_latency_tests
    android_native_music_focus_tests
    android_sound_picker_admission_tests
    android_sound_picker_startup_tests
    audio_owner_removal_tests
    chart_audio_renderer_tests
    club_beat_tests
    decoded_asset_cache_tests
    foundation_av_audio_device
    foundation_av_audio_mix
    foundation_av_audio_resampling
    foundation_av_audio_wrapper_lifecycle
    foundation_av_jukebox_restore
    foundation_av_skin_sound_bundle_decode
    generate_select_sounds_tests
    ios_audio_session_handoff_tests
    ios_music_stream_timeline_tests
    ios_sound_picker_admission_tests
    ios_sound_picker_startup_tests
    jukebox_scheduler_wake_tests
    music_player_sleep_timer_tests
    music_player_status_tests
    paged_pcm_mix_tests
    paged_preview_source_tests
    prep_metronome_tests
    preview_stream_player_tests
    progressive_music_source_tests
    settings_preview_chart_tests
    settings_scene_preview_authority_tests
    settings_sound_set_lifecycle
)

asobmashow_label_tests(area_video
    ios_now_playing_tests
    music_player_video_lifecycle_tests
    skin_movie_catalog_types_tests
    skin_movie_clock_cleanup
    skin_movie_clock_delayed
    skin_movie_clock_immediate
    skin_movie_clock_seek
    video_decode_state_tests
    video_frame_layout_tests
    visual_catch_up
)

asobmashow_label_tests(area_input
    android_gyroscope_input_backend_tests
    android_input_timestamp_ingress_tests
    android_pointer_ingress_tests
    android_raw_touch_input_tests
    android_touch_input_tests
    custom_touch_cancel_tests
    desktop_realtime_ingress_tests
    foundation_input_capture
    foundation_input_gameplay
    foundation_input_gyroscope
    foundation_input_gyroscope_backend
    foundation_input_gyroscope_ios_adapter
    foundation_input_midi
    foundation_input_profile
    foundation_input_registry
    foundation_input_resolver
    foundation_input_timestamp
    foundation_input_windows_realtime_mapping
    guided_access_reminder_tests
    input_capture_startup_failure_tests
    ios_guided_access_status_tests
    ios_hardware_button_tests
    ios_realtime_ingress_tests
    ios_touch_gesture_router_tests
    ipad_hardware_button_tests
    lane_cover_scene_input_tests
    legacy_skin_touch_tests
    native_keyboard_input_tests
    realtime_gameplay_input_registration_tests
    realtime_touch_input_router_tests
    sdl_touch_input_source_tests
    start_select_control_tests
    ui_input_overflow_tests
)

asobmashow_label_tests(area_gameplay
    android_gameplay_orientation_tests
    beatoraja_hispeed_tests
    bounded_duration_graph_tests
    chart_viewer_lifecycle_tests
    chart_viewer_note_geometry_tests
    course_continuation_tests
    course_gameplay_session_builder_tests
    course_identity_tests
    gameplay_automatic_authority_tests
    gameplay_bga_target_tests
    gameplay_chart_entity_render_budget_tests
    gameplay_gauge_rules_tests
    gameplay_judge_rules_tests
    gameplay_playback_startup_tests
    gameplay_ruleset_policy_tests
    gameplay_ruleset_tests
    gameplay_scene_feature_off_compile_contract
    gameplay_score_state_tests
    gameplay_scroll_geometry_tests
    gameplay_simulation_tests
    gameplay_terminal_scene_tests
    gbattle_tests
    graph_result_tests
    graph_selector_tests
    graph_simulation_tests
    judgement_indicator_range_tests
    lane_cover_controls_tests
    lane_cover_number_geometry_tests
    lane_effects_tests
    play_option_summary_tests
    play_options_panel_view_tests
    playfield_chart_visual_model_tests
    portrait_playfield_framing_tests
    preparation_plan_tests
    realtime_gameplay_authority_policy_tests
    realtime_gameplay_worker_tests
    start_lane_indicator_geometry_tests
)

asobmashow_label_tests(area_practice
    chart_practice_range_tests
    gameplay_practice_boundary_tests
    gameplay_practice_input_boundary_tests
    practice_analytics_presentation_tests
    practice_analytics_tests
    practice_configuration_tests
    practice_launch_tests
    practice_panel_message_tests
    practice_preset_store_tests
    practice_result_flow_tests
    practice_result_model_tests
    practice_rule_override_tests
    practice_session_tests
)

asobmashow_label_tests(area_results
    chart_record_actions_tests
    chart_record_filters_tests
    course_record_actions_tests
    legacy_result_summary_tests
    main_menu_records_lifecycle
    modern_result_recall_tests
    modern_result_tests
    record_file_actions_tests
    remote_result_scene_tests
    replay_record_filters_tests
    replay_record_task_tests
    replay_records_modal_tests
    replay_summary_list_tests
    replay_summary_list_view_tests
    result_contract_tests
    result_gauge_history_tests
    result_image_exporter_partial_tests
    result_layout_geometry_tests
    result_persistence_flow_audit
    result_persistence_model_tests
    result_presentation_model_tests
    result_record_list_view_tests
    result_record_summary_tests
    result_records_loader_tests
    score_cache_query_tests
)

asobmashow_label_tests(area_replay
    android_replay_codec_tests
    beatoraja_replay_codec_tests
    beatoraja_replay_path_tests
    best_replay_load_tests
    best_replay_resolver_tests
    chart_replay_context_tests
    chart_replay_persistence_tests
    course_replay_capture_tests
    course_replay_consumer_tests
    course_replay_context_tests
    course_replay_persistence_tests
    historical_replay_compatibility_tests
    replay_capabilities_tests
    replay_codec_primitive_tests
    replay_codec_store_contract_tests
    replay_export_cancellation_boundary
    replay_export_job_tests
    replay_file_action_service_tests
    replay_file_association_coordinator_tests
    replay_file_lifecycle_tests
    replay_file_reconciler_tests
    replay_file_store_tests
    replay_format_authority_tests
    replay_input_recorder_tests
    replay_keysound_schedule_tests
    replay_legacy_migration_tests
    replay_limits_tests
    replay_pixel_converter_tests
    replay_playback_driver_tests
    replay_playback_tests
    replay_profile_transfer_tests
    replay_repository_modern_chart_tests
    replay_repository_modern_course_tests
    replay_setup_tests
)

asobmashow_label_tests(area_ir
    bokutachi_cache_store_tests
    ir_account_lookup_service_tests
    ir_driver_tests
    ir_external_url_service_tests
    ir_http_client_tests
    ir_ranking_modal_tests
    ir_ranking_service_tests
    ir_receipt_models_tests
    ir_remote_score_models_tests
    ir_result_presentation_tests
    ir_saved_result_batch_upload_tests
    ir_saved_result_upload_tests
    ir_score_import_projection_tests
    ir_score_reconciliation_tests
    ir_settings_presentation_tests
    ir_submission_service_tests
    ir_submission_snapshot_tests
    ir_upload_candidate_list_view_tests
    ir_upload_candidates_tests
    ir_uploads_controller_tests
    ir_uploads_flow_audit
    main_menu_ranking_flow_audit
    settings_ir_scene_tests
    tachi_batch_manual_tests
    tachi_driver_tests
    tachi_ranking_parser_tests
    tachi_user_score_parser_tests
)

asobmashow_label_tests(area_library
    android_documents_activity_tests
    android_documents_mutability_tests
    android_documents_mutation_guard_tests
    android_documents_provider_tests
    android_folder_import_tests
    android_folder_picker_lifecycle_tests
    android_library_picker_startup_tests
    android_native_storage_paths_tests
    android_temporary_storage_tests
    chart_library_operations_tests
    chart_library_scanner_tests
    chart_library_task_service_tests
    chart_meta_index_order_tests
    chart_repository_tests
    chart_scan_startup_failure_tests
    chart_scan_work_scheduler_tests
    chart_selector_query_tests
    difficulty_table_importer_tests
    difficulty_table_url_completion_tests
    foundation_platform_document_handoff
    ios_library_picker_startup_tests
    main_menu_library_tests
    music_playlist_repository_tests
    settings_library_lifecycle_tests
    settings_library_task_tests
)

asobmashow_label_tests(area_archives
    archive_asset_batch_loader_tests
    archive_asset_pipeline_tests
    archive_bounded_reader_tests
    archive_file_concurrency_tests
    archive_index_build_coordinator_tests
    archive_reference_reader_tests
    archive_source_access_tests
    archive_unicode_artwork_extract_tests
    archive_unicode_artwork_missing_tests
    archive_unicode_artwork_read_tests
    archive_unzip_modal_tests
    archive_unzip_operation_tests
    settings_cache_maintenance_tests
    temporary_archive_cache_tests
    unzip_marker_io_tests
    unzip_output_tests
    windows_extraction_path_policy_tests
)

asobmashow_label_tests(area_downloads
    android_metadata_post_cancellation_tests
    find_bms_archive_flow_audit
    find_bms_download_tests
    find_bms_ios_file_bridge_tests
    find_bms_metadata_loopback_transport_tests
    find_bms_metadata_transport_tests
    find_bms_task_tests
    find_bms_verification_guard_tests
    find_bms_verification_tests
)

asobmashow_label_tests(area_profiles
    durable_payload_contract_tests
    foundation_profile_archive_faults
    foundation_profile_archive_portable
    foundation_profile_archive_transactions
    foundation_profile_archive_validation
    foundation_profile_controller
    foundation_profile_export_staging
    foundation_profile_manager_bootstrap
    foundation_profile_manager_deletion
    foundation_profile_manager_integrity
    foundation_profile_runtime
    foundation_profile_settings
    foundation_profile_settings_persistence
    foundation_profile_switch
    foundation_provenance_contract
    foundation_provenance_db
    ir_credential_migration_tests
    ir_credential_store_tests
    pending_ir_credential_cleanup_tests
    profile_database_ownership_tests
)

asobmashow_label_tests(area_settings
    foundation_av_settings
    foundation_av_settings_ui
    main_menu_settings_anchor_audit
    settings_audio_video_scene_tests
    settings_button_layout_tests
    settings_gameplay_skin_initialization_contract
    settings_note_editing_tests
    settings_rotation_edit_tests
    settings_virtual_controller_ui_contract
)

asobmashow_label_tests(area_music_select
    beatoraja_music_select_skin_ledger
    beatoraja_music_select_skin_ledger_evidence_contract
    chart_details_view_tests
    chart_list_item_view_tests
    chart_meta_page_cache_tests
    chart_preload_worker_tests
    decide_loading_overlay_tests
    main_menu_chart_list_lifecycle_audit
    main_menu_preview_controller_tests
    main_menu_preview_lifecycle
    music_select_bar_manager_tests
    music_select_bar_renderer_tests
    music_select_directory_loader_tests
    music_select_directory_request_tests
    music_select_error_flow_contract
    music_select_event_controller_tests
    music_select_export_preload
    music_select_external_actions_tests
    music_select_favorite_tests
    music_select_folder_status_loader_tests
    music_select_input_binding_adapter_tests
    music_select_input_processor_tests
    music_select_launch_policy_tests
    music_select_paged_songs_tests
    music_select_preview_tests
    music_select_property_projection_tests
    music_select_ranking_tests
    music_select_replay_slot_tests
    music_select_repository_projection_tests
    music_select_search_history_tests
    music_select_settings_runtime
    music_select_skin_model_tests
    music_select_skin_state_bridge_tests
    music_select_song_index_tests
    music_select_sql_songs_tests
    music_select_system_sound_tests
    music_select_toolbar_view_tests
)

asobmashow_label_tests(area_skin_assets
    android_skin_directory_import_tests
    gameplay_skin_acceptance_controller_tests
    gameplay_skin_lifecycle_tests
    gameplay_skin_loading_benchmark_tests
    gameplay_skin_settings_presentation_tests
    gameplay_skin_settings_tests
    gameplay_skin_source_format_tests
    gameplay_skin_traits_tests
    gameplay_skin_validator_tests
    skin_acceptance_recorder_tests
    skin_archive_importer_tests
    skin_archive_importer_windows_contract
    skin_commit_coordinator_tests
    skin_configuration_write_queue_tests
    skin_decode_cache_tests
    skin_diagnostic_history_tests
    skin_live_resource_counters_tests
    skin_overlay_digest_provider_tests
    skin_package_operation_service_tests
    skin_package_store_tests
    skin_package_store_windows_contract
    skin_path_policy_tests
    skin_performance_telemetry_tests
    skin_process_resident_memory_tests
    skin_resource_catalog_tests
    skin_text_decode_cache_tests
    skin_tree_snapshotter_tests
    skin_tree_snapshotter_windows_contract
)

asobmashow_label_tests(area_skin_runtime
    beatoraja_gameplay_cross_format_tests
    beatoraja_gameplay_oracle
    beatoraja_gameplay_skin_ledger
    beatoraja_gameplay_skin_ledger_evidence_contract
    beatoraja_skin_model_tests
    beatoraja_skin_reference_contract
    gameplay_skin_ir_scene_tests
    gameplay_skin_session_factory_tests
    json_gameplay_skin_decoder_tests
    lr2_gameplay_skin_decoder_tests
    lr2_skin_csv_parser_tests
    lua_music_select_skin_decoder_tests
    lua_skin_binding_decoder_tests
    lua_skin_binding_live_integration_tests
    lua_skin_coroutine_callback
    lua_skin_coroutine_header
    lua_skin_file_system_tests
    lua_skin_foundation_http_transport_tests
    lua_skin_gauge_live_integration_tests
    lua_skin_host_modules_tests
    lua_skin_http_transport_tests
    lua_skin_java_pattern_foundation_oracle_tests
    lua_skin_java_pattern_oracle_tests
    lua_skin_note_live_integration_tests
    lua_skin_runtime_tests
    lua_skin_table_decoder_tests
    lua_skin_text_graph_live_integration_tests
    play_skin_session_tests
    play_skin_state_bridge_tests
    playfield_presentation_coordinator_tests
    playfield_projection_tests
    playfield_visual_state_tests
    replay_playfield_presentation_tests
    replay_result_skin_tests
    result_skin_export_tests
    result_skin_ir_scene_tests
    result_skin_ir_tests
    result_skin_timeout_tests
)

asobmashow_label_tests(area_rendering
    android_display_refresh_rate_tests
    android_window_presentation_tests
    artwork_glow_tests
    bgfx_metal_readback_tests
    bgfx_platform_data_tests
    bgfx_skin_texture_device_tests
    bgfx_unix_platform_data_tests
    bgfx_vulkan_msaa_tests
    bgfx_vulkan_readback_tests
    bgfx_vulkan_swapchain_tests
    builtin_renderer_characterization_tests
    decoded_image_cache_tests
    foundation_av_android_native_window_owner
    foundation_av_android_render_suspend
    foundation_av_display
    foundation_av_display_orientation_sync
    foundation_av_frame_pacer
    foundation_av_ios_display_orientation_sync
    foundation_av_sdl_display
    gameplay_bga_sampling_metal_tests
    gameplay_builtin_image_batch
    image_alpha_bounds_tests
    image_decode_coordinator_tests
    image_decode_startup_failure_tests
    image_fade_shader_audit
    image_fade_tests
    image_file_decoder_tests
    image_view_fade_tests
    latency_telemetry_tests
    numeric_glyph_atlas_tests
    numeric_glyph_model_validator_tests
    play_skin_touch_geometry_tests
    play_skin_viewport_tests
    presentation_orientation_state_tests
    replay_video_ui_batch_contract
    skin_cover_normalization_tests
    skin_destination_evaluator_tests
    skin_draw_command_tests
    skin_gauge_node_expansion_tests
    skin_judge_normalization_tests
    skin_judge_number_normalization_tests
    skin_note_lane_geometry_normalization_tests
    skin_note_line_normalization_tests
    skin_note_normalization_tests
    skin_object_resolution_precedence_tests
    skin_quad_batch_renderer_tests
    skin_renderer_golden_tests
    skin_text_graph_normalization_tests
    text_view_font_lifetime_tests
    text_view_metal_tests
    ui_batch_android_upload_metal_tests
    ui_batch_metal_tests
    ui_batch_renderer_tests
    ui_batch_upload_cache_tests
)

asobmashow_label_tests(area_ui
    android_locale_tests
    button_disabled_metal_tests
    button_enabled_tests
    context_menu_view_tests
    dropdown_view_tests
    ios_native_text_editor_sync_audit
    localization_catalog_tests
    localization_tests
    main_menu_tabs_tests
    newcomer_tutorial_view_tests
    responsive_scene_layout_tests
    scene_language_lifecycle_tests
    scene_presentation_activity_tests
    scroll_momentum_tests
    scroll_offset_precision
    scrollbar_fade_tests
    text_animation_timing_tests
    text_input_box_tests
    ui_presentation_schedule_tests
    ui_scale_tests
    utf8_tests
    utility_tab_resume_tests
    view_layout_tests
    view_layout_threading_tests
)

asobmashow_label_tests(area_build
    android_dependency_cache_tests
    build_identity_refresh_contract
    fixture_compiler_environment_tests
    ledger_test_evidence_tests
    shader_compile_workflow_audit
    shared_test_sources_tests
    test_groups_tests
    windows_skin_shader_artifact_contract
)

# A few boundary tests are useful from either owning subsystem.
asobmashow_label_tests(area_settings storage_policy_tests)
asobmashow_label_tests(area_library storage_policy_tests)
asobmashow_label_tests(area_library storage_inventory_tests)
asobmashow_label_tests(area_library storage_file_transaction_tests)
asobmashow_label_tests(area_audio storage_inventory_tests)
asobmashow_label_tests(area_video storage_inventory_tests)
asobmashow_label_tests(area_ir
    ir_credential_store_tests ir_credential_migration_tests pending_ir_credential_cleanup_tests
    result_skin_ir_tests result_skin_ir_scene_tests gameplay_skin_ir_scene_tests)
asobmashow_label_tests(area_replay
    replay_result_skin_tests replay_playfield_presentation_tests replay_video_ui_batch_contract
    replay_record_task_tests replay_summary_list_tests replay_record_filters_tests
    replay_summary_list_view_tests replay_records_modal_tests)
asobmashow_label_tests(area_results
    result_skin_export_tests result_skin_ir_tests result_skin_ir_scene_tests result_skin_timeout_tests)
asobmashow_label_tests(area_music_select lua_music_select_skin_decoder_tests)

# Platform labels select host-side contracts/bridges, not device builds.
get_property(asobmashow_grouped_tests DIRECTORY PROPERTY TESTS)
foreach(platform ios android windows)
    foreach(test_name IN LISTS asobmashow_grouped_tests)
        if(test_name MATCHES "(^|_)${platform}(_|$)")
            asobmashow_label_tests(platform_${platform} "${test_name}")
        endif()
    endforeach()
endforeach()
asobmashow_label_tests(platform_ios guided_access_reminder_tests ipad_hardware_button_tests)

# Most native tests are mapped by asobmashow_register_test or share their
# target's name. These wrappers/shards have a different command or test name.
foreach(test_name archive_bounded_reader_tests archive_reference_reader_tests
        archive_unicode_artwork_read_tests archive_unicode_artwork_missing_tests
        archive_unicode_artwork_extract_tests)
    asobmashow_test_build_targets(${test_name} archive_file_concurrency_tests)
endforeach()
asobmashow_test_build_targets(music_select_settings_runtime lua_music_select_skin_decoder_tests)
asobmashow_test_build_targets(beatoraja_gameplay_oracle beatoraja_gameplay_cross_format_tests)
asobmashow_test_build_targets(beatoraja_gameplay_skin_ledger_evidence_contract json_gameplay_skin_decoder_tests)
foreach(test_name lua_skin_coroutine_header lua_skin_coroutine_callback)
    asobmashow_test_build_targets(${test_name} lua_skin_coroutine_tests)
endforeach()
asobmashow_test_build_targets(find_bms_metadata_loopback_transport_tests find_bms_metadata_transport_tests)
foreach(test_name find_bms_verification_guard_tests find_bms_verification_tests)
    asobmashow_test_build_targets(${test_name} find_bms_download_tests)
endforeach()
asobmashow_test_build_targets(lua_skin_java_pattern_oracle_tests lua_skin_java_pattern_tests)
asobmashow_test_build_targets(lua_skin_java_pattern_foundation_oracle_tests lua_skin_java_pattern_foundation_tests)
asobmashow_test_build_targets(visual_catch_up visual_catch_up_tests)
foreach(scenario immediate delayed seek cleanup)
    asobmashow_test_build_targets(skin_movie_clock_${scenario} skin_movie_clock_tests)
endforeach()
asobmashow_test_build_targets(gameplay_bga_sampling_metal_tests gameplay_bga_target_tests)
asobmashow_test_build_targets(ui_scale_tests yogacore)

asobmashow_finalize_test_groups()
