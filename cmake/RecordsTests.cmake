# Shared production dependencies for Records integration tests. Compile once:
# each executable supplies independent repository fixtures and assertions.
add_library(records_test_support STATIC
    src/ArchiveFile.cpp
    src/archive/TemporaryCache.cpp
    src/archive/UnzipOutput.cpp
    src/AtomicFile.cpp
    src/CourseIdentity.cpp
    src/FileChecksum.cpp
    src/MinizBridge.c
    src/ModernResult.cpp
    src/ModernResultRecallBuilder.cpp
    src/ReplayResultStateBuilder.cpp
    src/ResultPersistenceModel.cpp
    src/ResultRecordSummary.cpp
    src/ScoreProvenance.cpp
    src/Utils.cpp
    src/Uuid.cpp
    src/bms_parser.cpp
    src/ir/IrOutboxModels.cpp
    src/ir/IrProfileSettings.cpp
    src/ir/IrReceiptModels.cpp
    src/ir/IrRemoteScoreModels.cpp
    src/ir/IrScoreReconciliation.cpp
    src/ir/IrSettingsPresentation.cpp
    src/ir/IrSubmission.cpp
    src/ir/IrSubmissionModern.cpp
    src/ir/IrSubmissionSnapshot.cpp
    src/ir/IrUploadCandidates.cpp
    src/ir/tachi/TachiEligibility.cpp
    src/path.cpp
    src/replay/Base64Url.cpp
    src/replay/BeatorajaReplayCodec.cpp
    src/replay/BeatorajaReplayPath.cpp
    src/replay/ChartReplayAgreement.cpp
    src/replay/ChartReplayConsumer.cpp
    src/replay/ChartReplayConsumerRuntime.cpp
    src/replay/ChartReplayContext.cpp
    src/replay/CourseContinuation.cpp
    src/replay/CourseReplayAgreement.cpp
    src/replay/CourseReplayConsumer.cpp
    src/replay/CourseReplayConsumerRuntime.cpp
    src/replay/CourseReplayContext.cpp
    src/replay/GzipCodec.cpp
    src/replay/ReplayCapabilities.cpp
    src/replay/ReplayFileActionService.cpp
    src/replay/ReplayFileAssociationCoordinator.cpp
    src/replay/ReplayFileLifecycle.cpp
    src/replay/ReplayFileReconciler.cpp
    src/replay/ReplayFileStore.cpp
    src/replay/ReplayPlayback.cpp
    src/replay/ReplayPlaybackDriver.cpp
    src/replay/ReplayPlaybackMaterializer.cpp
    src/replay/ReplayProfileInventory.cpp
    src/replay/ReplayReferenceAgreement.cpp
    src/replay/ReplaySetup.cpp
    src/replay/ReplaySetupAdapter.cpp
    src/replay/ReplaySetupProvenance.cpp
    src/repositories/ChartStorageIdentity.cpp
    src/repositories/ReplayRepository.cpp
    src/repositories/ReplayRepositoryIrOutbox.cpp
    src/repositories/ReplayRepositoryIrRemoteScores.cpp
    src/repositories/ReplayRepositoryLegacyMigration.cpp
    src/repositories/ReplayRepositoryLegacySummaries.cpp
    src/repositories/ReplayRepositoryModernResults.cpp
    src/repositories/ReplayRepositoryRecords.cpp
    src/repositories/ReplayRepositorySchema.cpp
    src/scene/ChartRecordActions.cpp
    src/scene/CourseRecordActions.cpp
    src/scene/ResultRecordsLoader.cpp
    src/scene/play/CompiledGameplayJudge.cpp
    src/scene/play/GameplayCandidateRules.cpp
    src/scene/play/GameplayDefinition.cpp
    src/scene/play/GameplayGaugeRules.cpp
    src/scene/play/GameplayJudgeRules.cpp
    src/scene/play/GameplayNoteJudgeRole.cpp
    src/scene/play/GameplayRulesetPolicy.cpp
    src/scene/play/GameplaySimulation.cpp
    src/scene/play/Judge.cpp
    src/scene/play/PlayfieldChartVisualModel.cpp
    src/scene/play/SkinGameplayGraphState.cpp
    src/sqlite3.c
)
target_include_directories(records_test_support PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(records_test_support PUBLIC cxx_std_23)
target_link_libraries(records_test_support PUBLIC ${COMMON_LIBS})
if(TARGET SDL2::SDL2)
    target_link_libraries(records_test_support PUBLIC SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(records_test_support PUBLIC SDL2::SDL2-static)
elseif(TARGET SDL2)
    target_link_libraries(records_test_support PUBLIC SDL2)
endif()
if(APPLE)
    target_link_libraries(records_test_support PUBLIC iconv)
endif()
if(WIN32)
    target_link_libraries(records_test_support PUBLIC shell32 ole32)
endif()
foreach(records_test IN ITEMS chart_record_actions course_record_actions result_records_loader)
    add_executable(${records_test}_tests tests/${records_test}_tests.cpp)
    target_link_libraries(${records_test}_tests PRIVATE records_test_support)
    asobmashow_register_test(${records_test}_tests)
    asobmashow_stage_sevenzip_runtime(${records_test}_tests)
endforeach()

if(NOT IOS AND NOT PLATFORM_ANDROID)
    add_executable(record_file_actions_tests
        tests/record_file_actions_tests.cpp
        src/scene/RecordFileActions.cpp
        src/PlatformDocumentHandoff.cpp
        src/skin/package/SkinPathPolicy.cpp
    )
    target_link_libraries(record_file_actions_tests PRIVATE
        records_test_support TinyFileDialogs utf8proc::utf8proc)
    if(WIN32)
        target_link_libraries(record_file_actions_tests PRIVATE advapi32)
    endif()
    asobmashow_register_test(record_file_actions_tests)
    asobmashow_stage_sevenzip_runtime(record_file_actions_tests)
endif()
