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

template <std::size_t Size>
void rejectTokens(const std::filesystem::path &path,
                  const std::array<std::string_view, Size> &tokens) {
  const std::string text = readText(path);
  for (std::string_view token : tokens) {
    if (text.contains(token)) {
      std::cerr << "FAIL: " << path.filename().string()
                << " contains forbidden boundary token " << token << '\n';
      ++failures;
    }
  }
}

void requireToken(const std::filesystem::path &path, std::string_view token,
                  std::string_view authority);

void testModernResultAndSnapshotBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> forbidden{
      "ReplayData",      "ReplayPlaybackData", "ReplayFileMetadata",
      "ReplayFileStore", "ReplayRepository",   "sqlite3",
      "IrOutbox",        "StageReceipt",       ".events",
      "touchSamples",    "laneCoverEvents",    "materialize",
  };
  rejectTokens(root / "src/ModernResult.h", forbidden);
  rejectTokens(root / "src/ModernResult.cpp", forbidden);
  rejectTokens(root / "src/ir/IrSubmissionSnapshot.h", forbidden);
  rejectTokens(root / "src/ir/IrSubmissionSnapshot.cpp", forbidden);
  rejectTokens(root / "src/ModernResultRecallBuilder.h", forbidden);
  rejectTokens(root / "src/ModernResultRecallBuilder.cpp", forbidden);

  constexpr std::array<std::string_view, 4> materializationForbidden{
      "BuildResultState",
      "prepareReplayChart",
      "parseChartForReplay",
      "replay.events",
  };
  rejectTokens(root / "src/ModernResultRecallBuilder.cpp",
               materializationForbidden);

  requireToken(root / "src/ResultPersistenceModel.cpp",
               "projectModernResultFromLegacyAttempt",
               "explicitly named legacy result adapter");
  requireToken(root / "src/ir/IrSubmissionLegacyAdapter.cpp",
               "ChartResultAttempt", "explicitly isolated legacy IR adapter");
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
}

void testActivatedChartConsumersUseTheSharedPipeline() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/repositories/ReplayRepository.cpp",
               "GetResolvedProfileRoot",
               "profile-contained replay ownership root");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "makeRuntimeChartReplayConsumer",
               "modern Watch, G-Battle, recall, and video consumer");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "makeParsedChartReplayFacts",
               "modern Records replay availability projection");
  requireToken(root / "src/scene/ChartViewerScene.cpp",
               "makeRuntimeChartReplayConsumer",
               "modern practice ghost consumer");
  requireToken(root / "src/scene/ChartViewerScene.cpp",
               "makeParsedChartReplayFacts",
               "modern practice replay availability projection");
  requireToken(root / "src/replay/ChartReplayConsumer.cpp",
               "makeParsedChartReplayFacts",
               "modern replay consumer selected-chart projection");
  requireToken(root / "src/scene/MainMenuScene.cpp",
               "result_recall::BuildChartResult",
               "replay-independent modern result recall");
  requireToken(root / "src/scene/ResultScene.cpp",
               "modernReplayAttemptId",
               "modern practice replay identity");

  constexpr std::array<std::string_view, 3> forbidden{
      "BeatorajaReplayCodec", "ReplayFileStore", "readVerified"};
  rejectTokens(root / "src/scene/MainMenuScene.cpp", forbidden);
  rejectTokens(root / "src/scene/ChartViewerScene.cpp", forbidden);
  rejectTokens(root / "src/scene/ResultScene.cpp", forbidden);
  rejectTokens(root / "src/ReplayVideoExporter.cpp", forbidden);

  constexpr std::array<std::string_view, 1> ownershipForbidden{
      "GetResolvedDatabasePath"};
  rejectTokens(root / "src/context.h", ownershipForbidden);
  rejectTokens(root / "src/scene/MainMenuScene.cpp", ownershipForbidden);
  rejectTokens(root / "src/scene/ChartViewerScene.cpp", ownershipForbidden);
  rejectTokens(root / "src/replay/ChartReplayContext.cpp",
               ownershipForbidden);
  rejectTokens(root / "src/replay/ChartReplayPersistence.cpp",
               ownershipForbidden);
  rejectTokens(root / "src/replay/ChartReplayConsumerRuntime.cpp",
               ownershipForbidden);
}

void requireToken(const std::filesystem::path &path, std::string_view token,
                  std::string_view authority) {
  if (!readText(path).contains(token)) {
    std::cerr << "FAIL: " << path.filename().string() << " bypasses shared "
              << authority << '\n';
    ++failures;
  }
}

void testPlaybackSetupBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> forbidden{
      "ReplayData",   "ScoreProvenance",   "ResultPersistence",
      "IrSubmission", "ReplayRepository",  "sqlite3",
      "attemptId",    "resultFingerprint", "finalScore",
      "maxCombo",     "finalGauge",        "clearType",
  };
  rejectTokens(root / "src/replay/ReplaySetup.h", forbidden);
  rejectTokens(root / "src/replay/ReplaySetup.cpp", forbidden);
  rejectTokens(root / "src/replay/ReplayPlayback.h", forbidden);
  rejectTokens(root / "src/replay/ReplayPlayback.cpp", forbidden);
  rejectTokens(root / "src/replay/ReplayLimits.h", forbidden);
}

void testCapabilityPolicyBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> forbidden{
      "ReplayData",  "ScoreProvenance",  "ResultPersistence",
      "IrOutbox",    "ReplayRepository", "sqlite3",
      "filesystem",  "fstream",          "GamePlayScene",
      "ResultScene", "ProfileArchive",   "ReplayFileStore",
  };
  rejectTokens(root / "src/replay/ReplayCapabilities.h", forbidden);
  rejectTokens(root / "src/replay/ReplayCapabilities.cpp", forbidden);
}

void testCodecAndFileBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  constexpr std::array<std::string_view, 12> codecForbidden{
      "ScoreProvenance", "ResultPersistence", "IrSubmission",
      "IrOutbox",        "ReplayRepository",  "sqlite3",
      "attemptToken",    "resultFingerprint", "finalScore",
      "maxCombo",        "finalGauge",        "clearType",
  };
  rejectTokens(root / "src/replay/BeatorajaReplayCodec.h", codecForbidden);
  rejectTokens(root / "src/replay/BeatorajaReplayCodec.cpp", codecForbidden);

  constexpr std::array<std::string_view, 12> storeForbidden{
      "ScoreProvenance", "ResultPersistence", "IrSubmission",
      "IrOutbox",        "ReplayRepository",  "sqlite3",
      "resultId",        "resultFingerprint", "finalScore",
      "maxCombo",        "finalGauge",        "clearType",
  };
  rejectTokens(root / "src/replay/ReplayFileLifecycle.h", storeForbidden);
  rejectTokens(root / "src/replay/ReplayFileLifecycle.cpp", storeForbidden);
  rejectTokens(root / "src/replay/ReplayFileStore.h", storeForbidden);
  rejectTokens(root / "src/replay/ReplayFileStore.cpp", storeForbidden);
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
  requireToken(root / "src/PlayOptionUtils.h",
               "kPlayOptions = replay::kBeatorajaReplayOptions",
               "application and replay option table");
  requireToken(root / "src/AppSettings.cpp", "beatorajaReplayOptionIndex",
               "settings and replay option validation");
}

} // namespace

int main() {
  testPlaybackSetupBoundary();
  testCapabilityPolicyBoundary();
  testCodecAndFileBoundary();
  testSharedFormatAuthorities();
  testModernResultAndSnapshotBoundary();
  testSharedModernResultAuthorities();
  testActivatedChartConsumersUseTheSharedPipeline();
  if (failures != 0) {
    std::cerr << failures << " replay contract boundary test(s) failed\n";
    return 1;
  }
  std::cout << "replay contract boundary tests passed\n";
  return 0;
}
