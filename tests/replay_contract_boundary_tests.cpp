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
concept HasRawReplayCollection = requires(T value) { value.events; } ||
                                 requires(T value) { value.touchSamples; } ||
                                 requires(T value) { value.laneCoverEvents; };

static_assert(!HasResultFact<replay::ReplaySetup>);
static_assert(!HasRawReplayCollection<replay::ReplaySetup>);
static_assert(!HasResultFact<replay::ReplayPlaybackData>);
static_assert(!HasResultFact<replay::CourseReplayPlaybackData>);
static_assert(!HasResultFact<replay::ReplayCapabilities>);
static_assert(!HasResultFact<replay::ReplayChartDocument>);
static_assert(!HasResultFact<replay::ReplayCourseDocument>);
static_assert(!HasResultFact<replay::ReplayFileMetadata>);

namespace {

int failures = 0;

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void rejectTokens(const std::filesystem::path &path,
                  const std::array<std::string_view, 12> &tokens) {
  const std::string text = readText(path);
  for (std::string_view token : tokens) {
    if (text.contains(token)) {
      std::cerr << "FAIL: " << path.filename().string()
                << " contains forbidden boundary token " << token << '\n';
      ++failures;
    }
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

} // namespace

int main() {
  testPlaybackSetupBoundary();
  testCapabilityPolicyBoundary();
  testCodecAndFileBoundary();
  if (failures != 0) {
    std::cerr << failures << " replay contract boundary test(s) failed\n";
    return 1;
  }
  std::cout << "replay contract boundary tests passed\n";
  return 0;
}
