#include "ModernResult.h"
#include "ModernResultRecallBuilder.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/ReplayCapabilities.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileLifecycle.h"
#include "replay/ReplayPlayback.h"
#include "replay/ReplaySetup.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef ASOBMASHOW_SOURCE_DIR
#error "ASOBMASHOW_SOURCE_DIR must identify the repository root"
#endif

template <typename T>
concept HasResultFact = requires(T value) { value.finalScore; } ||
                        requires(T value) { value.maxCombo; } ||
                        requires(T value) { value.finalGauge; } ||
                        requires(T value) { value.clearType; } ||
                        requires(T value) { value.createdAt; } ||
                        requires(T value) { value.attemptId; } ||
                        requires(T value) { value.resultFingerprint; };

template <typename T>
concept HasRawReplayCollection =
    requires(T value) { value.events; } || requires(T value) { value.input; } ||
    requires(T value) { value.touchSamples; } ||
    requires(T value) { value.laneCoverEvents; };

template <typename T>
concept HasReplayFileFact = requires(T value) { value.relativePath; } ||
                            requires(T value) { value.compressedSize; } ||
                            requires(T value) { value.codecVersion; } ||
                            requires(T value) { value.replayFile; };

static_assert(!HasResultFact<replay::ReplaySetup>);
static_assert(!HasRawReplayCollection<replay::ReplaySetup>);
static_assert(!HasResultFact<replay::ReplayPlaybackData>);
static_assert(!HasResultFact<replay::CourseReplayPlaybackData>);
static_assert(!HasResultFact<replay::ReplayCapabilities>);
static_assert(!HasResultFact<replay::ReplayChartDocument>);
static_assert(!HasResultFact<replay::ReplayCourseDocument>);
static_assert(!HasResultFact<replay::ReplayFileMetadata>);
static_assert(!HasRawReplayCollection<result_persistence::ChartScoreWrite>);
static_assert(!HasRawReplayCollection<result_persistence::ModernChartResult>);
static_assert(!HasRawReplayCollection<result_persistence::ModernCourseResult>);
static_assert(!HasRawReplayCollection<ir::IrSubmissionSnapshot>);
static_assert(!HasReplayFileFact<result_persistence::ChartScoreWrite>);
static_assert(!HasReplayFileFact<result_persistence::ModernChartResult>);
static_assert(!HasReplayFileFact<result_persistence::ModernCourseResult>);
static_assert(!HasReplayFileFact<ir::IrSubmissionSnapshot>);

namespace {

int failures = 0;

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void requireToken(const std::filesystem::path &path, std::string_view token,
                  std::string_view authority);

void requireOrderedWithin(const std::filesystem::path &path,
                          std::string_view scopeFirst,
                          std::string_view scopeLast, std::string_view first,
                          std::string_view second,
                          std::string_view authority) {
  const auto text = readText(path);
  const auto scopeFirstAt = text.find(scopeFirst);
  const auto scopeLastAt = text.find(scopeLast, scopeFirstAt);
  const auto firstAt = text.find(first, scopeFirstAt);
  const auto secondAt = text.find(second, scopeFirstAt);
  if (scopeFirstAt == std::string::npos || scopeLastAt == std::string::npos ||
      firstAt == std::string::npos || secondAt == std::string::npos ||
      firstAt >= secondAt || firstAt >= scopeLastAt || secondAt >= scopeLastAt) {
    std::cerr << "FAIL: " << authority << '\n';
    ++failures;
  }
}

void testSharedModernResultAuthorities() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/ModernResult.cpp",
               "result_contract::", "modern result fact authority");
  requireToken(root / "src/ir/IrSubmission.cpp",
               "result_contract::", "IR result fact authority");
  requireToken(root / "src/ModernResultRecallBuilder.cpp",
               "result_contract::compareChartIdentity",
               "modern recall identity agreement authority");
  requireToken(root / "src/replay/ReplaySetup.cpp",
               "result_contract::compareChartIdentity",
               "replay identity agreement authority");
  requireToken(root / "src/ir/IrRankingModels.cpp",
               "asobmshow::bms_metadata::normalizedHash",
               "chart hash normalization authority");
  constexpr std::array<std::string_view, 8> replaySetupConsumers{
      "src/ModernResultRecallBuilder.cpp",
      "src/replay/ChartReplayAgreement.cpp",
      "src/replay/ChartReplayConsumer.cpp",
      "src/replay/CourseReplayAgreement.cpp",
      "src/replay/CourseReplayConsumer.cpp",
      "src/replay/CourseReplayContext.cpp",
      "src/scene/ChartViewerScene.cpp",
      "src/scene/play/GamePlayScene.cpp",
  };
  for (const std::string_view consumer : replaySetupConsumers) {
    requireToken(root / consumer, "replaySetupLongNoteMode",
                 "result-backed replay setup long-note-mode authority");
  }

  constexpr std::array<std::string_view, 24> digestConsumers{
      "src/ChartLibraryScanner.cpp",
      "src/CourseIdentity.cpp",
      "src/ResultPersistenceModel.cpp",
      "src/ScoreProvenance.cpp",
      "src/bms_search/ArchiveDecision.cpp",
      "src/bms_search/ArchiveSupport.cpp",
      "src/bms_search/DownloadedArchiveWorkflow.cpp",
      "src/repositories/ReplayRepositoryIrRemoteScores.cpp",
      "src/repositories/ReplayRepositoryLegacyMigration.cpp",
      "src/ir/IrReceiptModels.cpp",
      "src/ir/IrRemoteScoreModels.cpp",
      "src/ir/IrRankingModels.cpp",
      "src/ir/IrRankingService.cpp",
      "src/ir/IrOutboxModels.cpp",
      "src/ir/IrScoreReconciliation.cpp",
      "src/ir/tachi/TachiBatchManual.cpp",
      "src/ir/tachi/BokutachiCacheStore.cpp",
      "src/ir/tachi/TachiDriver.cpp",
      "src/ir/tachi/TachiUserScoreParser.cpp",
      "src/ir/IrRankingModal.cpp",
      "src/practice/PracticeConfiguration.cpp",
      "src/practice/PracticeLaunchRequest.cpp",
      "src/practice/PracticePresetStore.cpp",
      "src/scene/ResultScene.h",
  };
  for (const std::string_view consumer : digestConsumers) {
    const auto path = root / consumer;
    requireToken(path, "canonical_digest::isCanonicalLowerHex",
                 "canonical chart digest authority");
  }
}

void testSharedIrProviderIdentityAuthority() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 11> consumers{
      "src/ResultRecordSummary.cpp",
      "src/ir/IrCredentialStore.cpp",
      "src/ir/IrDriver.cpp",
      "src/ir/IrOutboxModels.cpp",
      "src/ir/IrRankingModels.cpp",
      "src/ir/IrReceiptModels.cpp",
      "src/ir/IrScoreReconciliation.cpp",
      "src/ir/IrUploadCandidates.cpp",
      "src/repositories/ReplayRepositoryIrOutbox.cpp",
      "src/repositories/ReplayRepositoryIrRemoteScores.cpp",
      "src/repositories/ScoreRepositoryIrImport.cpp",
  };
  for (const std::string_view consumer : consumers) {
    const auto path = root / consumer;
    requireToken(path, "ir::isValidProviderId",
                 "IR provider identity authority");
  }
}

void testSharedMaximumScoreAuthority() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 20> consumers{
      "src/ModernResult.cpp",
      "src/ModernResultRecallBuilder.cpp",
      "src/ResultRecordSummary.cpp",
      "src/ReplayAutoPlay.h",
      "src/GBattleMode.h",
      "src/ir/IrRankingModels.cpp",
      "src/ir/IrRankingModal.cpp",
      "src/ir/IrRemoteScoreModels.cpp",
      "src/ir/tachi/TachiEligibility.cpp",
      "src/ir/tachi/TachiDriver.cpp",
      "src/ir/tachi/TachiRankingParser.cpp",
      "src/ir/tachi/TachiUserScoreParser.cpp",
      "src/replay/ReplayPlaybackMaterializer.cpp",
      "src/repositories/ScoreRepositoryIrImport.cpp",
      "src/repositories/ScoreRepositoryQueries.cpp",
      "src/scene/ChartViewerScene.cpp",
      "src/scene/MainMenuScene.cpp",
      "src/scene/ResultPresentationModel.cpp",
      "src/scene/ResultScene.cpp",
      "src/scene/play/Pacemaker.h",
  };
  for (const std::string_view consumer : consumers) {
    const auto path = root / consumer;
    requireToken(path, "result_contract::maximumScoreForNotes",
                 "maximum-score arithmetic authority");
  }
}

void testActivatedChartConsumersUseTheSharedPipeline() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/repositories/ReplayRepository.cpp",
               "GetResolvedProfileRoot",
               "profile-contained replay ownership root");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "makeRuntimeChartReplayConsumer",
               "modern Watch, G-Battle, recall, and video consumer");
  requireToken(root / "src/scene/MainMenuScene.cpp", "replayLoadThread",
               "non-blocking modern replay action preparation");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "queueReplayLoadCompletion",
               "main-thread replay action completion boundary");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "ReplayFileActionService",
               "modern Records deferred replay availability inspection");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "finishReplayLoadFailure",
               "precise replay consumer failure presentation");
  requireToken(root / "src/scene/MainMenuScene.cpp", "loaded.diagnostic",
               "replay consumer diagnostic propagation");
  requireToken(root / "src/scene/ChartViewerScene.cpp",
               "makeRuntimeChartReplayConsumer",
               "modern practice ghost consumer");
  requireToken(root / "src/scene/ChartViewerScene.cpp",
               "ReplayFileActionService",
               "deferred practice ghost file inspection");
  requireToken(root / "src/replay/ChartReplayConsumer.cpp",
               "compareReplayChartIdentity",
               "single prepared-chart identity agreement boundary");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "result_recall::BuildChartResult",
               "replay-independent modern result recall");
  requireToken(root / "src/scene/MainMenuScene.cpp", "ModernChartLoader",
               "single-parse modern result recall when BRD is available");
  requireToken(root / "src/scene/MainMenuScene.cpp", "currentChartPath",
               "modern result recall does not resolve the current chart "
               "location");
  requireToken(root / "src/scene/ResultScene.cpp",
               "modernReplayAttemptId",
               "modern practice replay identity");
  const auto profileReconciliation =
      root / "src/replay/ReplayProfileReconciliation.cpp";
  requireToken(profileReconciliation,
               "loadAgreedModernReplayTombstoneInventory",
               "shared tombstone-only replay cleanup inventory");
  requireToken(profileReconciliation,
               "removeTombstonedEntryIfMatches",
               "tombstone cleanup ownership agreement");
  requireToken(profileReconciliation, "removeIfMatches",
               "tombstone byte-identity verification");
  requireToken(root / "src/main.cpp", "reconcileProfileReplayFiles",
               "startup shared replay cleanup boundary");
  requireToken(root / "src/ProfileSessionCoordinator.cpp",
               "reconcileProfileReplayFiles",
               "profile switch shared replay cleanup boundary");
  requireToken(root / "src/replay/ReplayFileAssociationCoordinator.cpp",
               "recordInstallIntent",
               "pre-install replay ownership journal");
  requireToken(root / "src/PlayerProfileManager.cpp",
               "loadAgreedModernReplayFileInventory",
               "profile transfer complete replay ownership inventory");

}

void testCourseContinuationAndConsumerBoundaries() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/replay/CourseContinuation.cpp",
               "advanceCourseContinuation",
               "course continuation transition authority");
  requireToken(root / "src/replay/CourseReplayConsumer.cpp",
               "advanceCourseContinuation",
               "course replay materialization continuation authority");
  requireToken(root / "src/scene/play/GamePlayScene.cpp",
               "advanceCourseContinuation",
               "live course continuation authority");
  requireToken(root / "src/ReplayVideoExporter.cpp",
               "CourseReplayConsumer",
               "modern course video consumer");

}

void testReplayExportUsesPreparedGameplayBgaFrames() {
  const std::filesystem::path exporter =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "src/ReplayVideoExporter.cpp";
  requireToken(exporter, "GameplayBgaMissStateTracker",
               "replay-export BGA miss-state authority");
  requireToken(exporter, "prepareVisualFrameAt",
               "prepared replay-export BGA frame authority");
  requireToken(exporter, "submitFullscreen",
               "replay-export fullscreen BGA submission authority");
}

void testNormalReplayExportUsesPreparedPresentation() {
  const std::filesystem::path exporter =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "src/ReplayVideoExporter.cpp";
  const std::filesystem::path preflight =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "src/scene/play/ReplayVideoGameplayPreflight.cpp";
  constexpr std::string_view normalExportStart =
      "ReplayVideoExporter::Export(ApplicationContext &context,";
  constexpr std::string_view normalExportEnd =
      "ReplayVideoExporter::ExportCourseReplay(";
  requireOrderedWithin(exporter, normalExportStart, normalExportEnd,
                       "preflightReplayGameplayPresentation(",
                       "writeReplayAudioTrack(",
                       "normal Export preflights skins before audio work");
  requireOrderedWithin(exporter, normalExportStart, normalExportEnd,
                       "preflightReplayGameplayPresentation(",
                       "renderReplayVideoToMp4(",
                       "normal Export preflights skins before MP4 work");
  requireOrderedWithin(exporter, normalExportStart, normalExportEnd,
                       "preflightReplayGameplayPresentation(",
                       "ensureReplayExportDirectoryError(",
                       "normal Export preflights skins before output work");
  requireOrderedWithin(exporter, normalExportStart, normalExportEnd,
                       "preflightReplayGameplayPresentation(",
                       "RequestIOSPhotoAddAuthorization(",
                       "normal Export preflights skins before Photos work");
  requireOrderedWithin(exporter, normalExportStart, normalExportEnd,
                       "runPreflightGatedNormalExport(",
                       "writeReplayAudioTrack(",
                       "normal Export gates audio work on its preflight result");
  requireToken(exporter, "ReplayPlayfieldPresentation",
               "normal replay uses the coordinator-backed presentation adapter");
  requireToken(exporter, "releaseDueClassicLongNoteTails",
               "normal replay delegates classic LN auto-release to its adapter");
  requireToken(preflight, ".replayData = &replay",
               "normal replay preserves built-in ghost and miss-marker input");
  requireToken(exporter, "replay_video_export::skinExportFailureMessage(",
               "normal replay safely reports presentation frame failures");
  requireToken(exporter, "presentation.frame_failure_missing",
               "normal replay reports a diagnostic-free frame failure safely");
}

void testCourseReplayExportUsesPreparedPresentations() {
  const std::filesystem::path exporter =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "src/ReplayVideoExporter.cpp";
  constexpr std::string_view courseExportStart =
      "ReplayVideoExportResult exportCourseReplayImpl(";
  constexpr std::string_view courseExportEnd =
      "ReplayVideoExporter::ExportCourseReplay(ApplicationContext &context,";
  requireOrderedWithin(exporter, courseExportStart, courseExportEnd,
                       "preflightCourseReplayGameplayPresentations(",
                       "writeReplayAudioTrack(",
                       "every course skin is preflighted before stage audio");
  requireOrderedWithin(exporter, courseExportStart, courseExportEnd,
                       "preflightCourseReplayGameplayPresentations(",
                       "writeCourseReplayAudioTrack(",
                       "every course skin is preflighted before mux audio");
  requireToken(exporter, "stage.selectedSkinTiming",
               "course preflight retains only immutable skin timing");
  requireToken(exporter, "stage.gameplayPresentation.reset()",
               "course exporter releases each stage presentation");
}

void requireToken(const std::filesystem::path &path, std::string_view token,
                  std::string_view authority) {
  if (!readText(path).contains(token)) {
    std::cerr << "FAIL: " << path.filename().string() << " bypasses shared "
              << authority << '\n';
    ++failures;
  }
}

void testSharedFormatAuthorities() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/replay/ReplaySetup.cpp",
               "validReplayPlayerOptionName", "replay option authority");
  requireToken(root / "src/replay/ReplayPlayback.cpp", "replayKeyModeLayout",
               "replay key-mode authority");
  requireToken(root / "src/replay/BeatorajaReplayCodec.cpp",
               "projectedBeatorajaReplayOptionIndex",
               "stock option projection authority");
  requireToken(root / "src/replay/ReplayFileStore.cpp",
               "isCanonicalReplayRelativePath", "replay path authority");
  requireToken(root / "src/replay/ReplayFileActionService.cpp",
               "removeObservedIfMatches",
               "ownership-safe observed replay cleanup authority");
  requireToken(root / "src/PlayOptionUtils.h",
               "kPlayOptions = replay::kBeatorajaReplayOptions",
               "application and replay option table");
  requireToken(root / "src/AppSettings.cpp", "beatorajaReplayOptionIndex",
               "settings and replay option validation");
}

void testSharedAssistClearMarkAuthority() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/scene/play/GameplayGaugeTypes.h",
               "assistClearMarkRequired", "assist clear-mark authority");
  constexpr std::array<std::string_view, 5> consumers{
      "src/scene/play/GamePlayScene.cpp",
      "src/replay/ReplayPlaybackMaterializer.cpp",
      "src/ReplayResultStateBuilder.cpp",
      "src/ModernResultRecallBuilder.cpp",
      "src/ReplayAutoPlay.h",
  };
  for (std::string_view consumer : consumers) {
    const auto path = root / consumer;
    requireToken(path, "clear_policy::assistClearMarkRequired",
                 "assist clear-mark authority");
  }
}

void testReplayIdentityParsingUsesSavedRandomBranch() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto chartConsumer =
      root / "src/replay/ChartReplayConsumerRuntime.cpp";
  requireToken(chartConsumer, "prepareReplayChart",
               "single BRD-setup chart preparation authority");

  const auto courseConsumer =
      root / "src/replay/CourseReplayConsumerRuntime.cpp";
  requireToken(courseConsumer, "savedChartRandomParseSetup",
               "saved random-branch identity parse authority");
  requireToken(root / "src/ModernResultRecallBuilder.cpp",
               "savedChartRandomParseSetup",
               "saved random-branch result recall authority");
}

void testBestPacemakerUsesTheRetainedReplayConsumer() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/scene/play/GamePlayScene.cpp",
               "bestReplayLoadThread",
               "non-blocking live BEST replay progression consumer");
  requireToken(root / "src/scene/play/GamePlayScene.cpp",
               "startBestReplayLoad",
               "shared asynchronous BEST replay loading boundary");
  requireToken(root / "src/ResultPresentationUtils.h",
               "makeRuntimeBestReplayResolver",
               "BEST replay resolution authority");
  requireToken(root / "src/ResultPresentationUtils.h",
               "replayForBestSnapshotChart",
               "live BEST replay resolution authority");
  constexpr std::array<std::string_view, 2> consumers{
      "src/ReplayVideoExporter.cpp",
      "src/ResultImageExporter.cpp",
  };
  for (std::string_view consumer : consumers) {
    const auto path = root / consumer;
    requireToken(path, "replayForPreviousBestChart",
                 "retained BEST replay resolution");
  }
  requireToken(root / "src/ReplayVideoExporter.cpp", "bestScoreReplay.get()",
               "replay-export personal-best progression");
  requireToken(root / "src/ResultImageExporter.cpp", "bestReplay.get()",
               "result-image BEST replay progression");

  const auto gameplay = root / "src/scene/play/GamePlayScene.cpp";
  requireToken(gameplay, "makeRuntimeBestReplayResolver",
               "background BEST replay resolution authority");
  requireToken(gameplay, "pendingBestReplay",
               "main-thread BEST replay application boundary");
}

void testHistoryPresentationUsesBoundedCompleteListsAndCheapFileProbes() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto mainMenu = root / "src/scene/MainMenuScene.cpp";
  requireToken(mainMenu, "kMaximumLegacyResultSummaryRows",
               "complete bounded legacy Records history");
  requireToken(mainMenu, "kMaximumModernChartHistoryRows",
               "complete bounded chart Records history");
  requireToken(mainMenu, "kMaximumModernCourseHistoryRows",
               "complete bounded course Records history");
  requireToken(mainMenu, "replayActions.probe(modern.replayFile)",
               "non-materializing Records replay availability probe");
  const std::string menuText = readText(mainMenu);
  const std::size_t irRead =
      menuText.find("ListIrUploadRecordsForChart(");
  const std::size_t irReadEnd = menuText.find(");", irRead);
  if (irRead == std::string::npos || irReadEnd == std::string::npos ||
      !std::string_view(menuText)
           .substr(irRead, irReadEnd - irRead)
           .contains("kMaximumModernChartHistoryRows")) {
    std::cerr << "FAIL: displayed chart attempts and their IR state must use "
                 "the same bounded history limit\n";
    ++failures;
  }

  const auto chartViewer = root / "src/scene/ChartViewerScene.cpp";
  requireToken(chartViewer, "kMaximumModernChartHistoryRows",
               "complete bounded practice ghost history");
  requireToken(chartViewer, "replayActions.probe(record.replayFile)",
               "non-materializing practice replay availability probe");
}

void testReplayModalOwnsBackgroundLoadLifetime() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto header = root / "src/scene/MainMenuScene.h";
  requireToken(header, "replayLoadInProgress",
               "replay modal background-load lifetime");
  const auto menu = root / "src/scene/MainMenuScene.cpp";
  requireToken(menu, "replayLoadInProgress.load()",
               "replay modal operation guard");
  requireToken(menu, "button->setEnabled(enabled)",
               "themed action state blocks input as well as restyling it");
}

} // namespace

int main() {
  testSharedFormatAuthorities();
  testSharedAssistClearMarkAuthority();
  testReplayIdentityParsingUsesSavedRandomBranch();
  testBestPacemakerUsesTheRetainedReplayConsumer();
  testHistoryPresentationUsesBoundedCompleteListsAndCheapFileProbes();
  testReplayModalOwnsBackgroundLoadLifetime();
  testSharedModernResultAuthorities();
  testSharedIrProviderIdentityAuthority();
  testSharedMaximumScoreAuthority();
  testActivatedChartConsumersUseTheSharedPipeline();
  testCourseContinuationAndConsumerBoundaries();
  testReplayExportUsesPreparedGameplayBgaFrames();
  testNormalReplayExportUsesPreparedPresentation();
  testCourseReplayExportUsesPreparedPresentations();
  if (failures != 0) {
    std::cerr << failures << " replay contract boundary test(s) failed\n";
    return 1;
  }
  std::cout << "replay contract boundary tests passed\n";
  return 0;
}
