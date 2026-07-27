#include "repositories/ReplayRepository.h"

#include "sqlite3.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef ASOBMASHOW_SOURCE_ROOT
#error "ASOBMASHOW_SOURCE_ROOT must identify the repository root"
#endif

template <typename T>
concept HasDuplicatedJudgedSetupField =
    requires(T value) { value.randomSeed; } ||
    requires(T value) { value.randomPrng; } ||
    requires(T value) { value.randomValues; } ||
    requires(T value) { value.playOption; } ||
    requires(T value) { value.playOptionSeed; } ||
    requires(T value) { value.playOption2; } ||
    requires(T value) { value.playOption2Seed; } ||
    requires(T value) { value.assistOption; } ||
    requires(T value) { value.initialGaugeType; } ||
    requires(T value) { value.gaugeProfile; } ||
    requires(T value) { value.gaugeAutoShift; } ||
    requires(T value) { value.gaugeAutoShiftLowerBound; } ||
    requires(T value) { value.initialLaneCoverPercent; } ||
    requires(T value) { value.laneCoverEnabled; };

static_assert(!HasDuplicatedJudgedSetupField<JudgedPlaybackData>,
              "Judged playback setup must remain a single value object");

template <typename T>
concept HasDuplicatedAnalysisSetupField =
    requires(T value) { value.playback; } ||
    requires(T value) { value.candidateSelection; } ||
    requires(T value) { value.judgeWindowScalePercent; } ||
    requires(T value) { value.clubMode; };

static_assert(
    !HasDuplicatedAnalysisSetupField<analysis::PlaybackAnalysisContext>,
    "Playback analysis context must contain proof-only facts, not setup");

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-replay-decoupling-" + std::to_string(tick) + "-" +
             std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool containsAny(std::string_view text,
                 const std::vector<std::string_view> &needles) {
  for (const auto needle : needles) {
    if (text.contains(needle)) {
      return true;
    }
  }
  return false;
}

void reportForbidden(const std::filesystem::path &relative,
                     std::string_view category) {
  std::cerr << "FAIL: " << category << " in " << relative.generic_string()
            << '\n';
  ++failures;
}

void testProductionSourceBoundaries() {
  const std::filesystem::path sourceRoot =
      std::filesystem::path(ASOBMASHOW_SOURCE_ROOT) / "src";
  const std::string migration =
      "repositories/ReplayRepositoryReplayFileMigration.cpp";
  const std::vector<std::string_view> removedRuntimeSymbols{
      "struct ReplayData", "ChartResultAttempt", "SaveReplay(",
      "SaveCourseReplay(", "LoadReplayResult",
      "applyScoreProvenanceToStartOptions",
      "applyJudgedPlaybackContextToStartOptions", "insertReplayEvent(",
      "insertReplayTouchSample(", "insertReplayLaneCoverEvent(",
      "readReplaySummary("};
  const std::vector<std::string_view> rowPayloadTables{
      "replay_events", "replay_touch_samples", "replay_lane_cover_events"};
  const std::vector<std::string_view> persistenceFields{
      "ScoreProvenance", "IrSubmission", "attemptId", "resultFingerprint"};
  const std::vector<std::string_view> replayInterpretationSources{
      "analysis/JudgedPlaybackAnalysis.h", "ReplayVideoExporter.cpp",
      "scene/ChartViewerScene.cpp", "scene/MainMenuScene.cpp"};

  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(sourceRoot)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto extension = entry.path().extension();
    if (extension != ".h" && extension != ".hpp" && extension != ".cpp" &&
        extension != ".c") {
      continue;
    }
    const auto relative =
        std::filesystem::relative(entry.path(), sourceRoot).generic_string();
    const std::string contents = readText(entry.path());

    if (containsAny(contents, removedRuntimeSymbols)) {
      reportForbidden(relative, "removed replay runtime symbol");
    }
    if (relative != migration && containsAny(contents, rowPayloadTables)) {
      reportForbidden(relative, "row-per-event replay table reference");
    }

    const bool persistenceBoundary =
        relative.starts_with("replay/") ||
        relative == "analysis/JudgedPlaybackData.h";
    if (persistenceBoundary && containsAny(contents, persistenceFields)) {
      reportForbidden(relative, "persistence field in playback projection");
    }
    if (std::ranges::find(replayInterpretationSources, relative) !=
            replayInterpretationSources.end() &&
        contents.contains("applyScoreProvenanceToStartOptions")) {
      reportForbidden(relative, "provenance-based replay interpretation");
    }

    const bool legacyPlaybackAllowed =
        relative == "replay/BeatorajaReplayCodec.h" ||
        relative == "replay/BeatorajaReplayCodec.cpp" ||
        relative == migration ||
        relative == "replay/ReplayPlaybackMaterializer.cpp";
    if (!legacyPlaybackAllowed && contents.contains("legacyPlaybackEvents")) {
      reportForbidden(relative, "legacy playback reconstruction dependency");
    }
  }
}

void testFreshSchemaContainsNoRowPayloadDdl() {
  TemporaryDirectory temporary;
  const auto databasePath = temporary.path() / "replays.db";
  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(), "fresh schema initializes");

  sqlite3 *database = nullptr;
  expect(sqlite3_open_v2(databasePath.string().c_str(), &database,
                         SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
         "fresh schema reopens read-only");
  if (database == nullptr) {
    return;
  }

  sqlite3_stmt *statement = nullptr;
  const char *sql =
      "SELECT type,name,coalesce(sql,'') FROM sqlite_master "
      "WHERE type IN ('table','view','trigger') ORDER BY type,name";
  expect(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) ==
             SQLITE_OK,
         "fresh schema DDL query prepares");
  if (statement != nullptr) {
    const std::vector<std::string_view> forbidden{
        "replay_events", "replay_touch_samples", "replay_lane_cover_events"};
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const auto *ddl = sqlite3_column_text(statement, 2);
      if (ddl != nullptr &&
          containsAny(reinterpret_cast<const char *>(ddl), forbidden)) {
        const auto *name = sqlite3_column_text(statement, 1);
        std::cerr << "FAIL: fresh v11 DDL references row-per-event storage: "
                  << (name == nullptr ? "<unnamed>"
                                      : reinterpret_cast<const char *>(name))
                  << '\n';
        ++failures;
      }
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(database);
}

} // namespace

int main() {
  testProductionSourceBoundaries();
  testFreshSchemaContainsNoRowPayloadDdl();
  if (failures != 0) {
    std::cerr << failures << " replay decoupling audit(s) failed\n";
    return 1;
  }
  std::cout << "Replay decoupling audits passed\n";
  return 0;
}
