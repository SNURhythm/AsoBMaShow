#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct TransparentStringHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
};

struct ScoreRankByLongNoteMode {
  std::array<int, 4> ranks{kNoClearTypeRank, kNoClearTypeRank,
                           kNoClearTypeRank, kNoClearTypeRank};

  [[nodiscard]] int bestRankForMode(int lnMode) const;
};

enum class ScoreBestSource {
  Local,
  ImportedIr,
};

enum class ScoreStorageSource : int {
  LocalGameplay = 0,
  ImportedIr = 1,
};

enum class ImportedIrScoreProjectionStatus {
  Applied,
  AlreadyCurrent,
  Invalid,
  StorageFailure,
};

struct ImportedIrScoreProjectionOutcome {
  ImportedIrScoreProjectionStatus status =
      ImportedIrScoreProjectionStatus::StorageFailure;
  int projectedScores = 0;
  std::string diagnostic;
};

struct ScoreBestSnapshot {
  int score = 0;
  int maxScore = 0;
  std::array<int, 5> judgementCounts{};
  int playCount = 0;
  int clearCount = 0;
  std::optional<std::int64_t> lastPlayedUnixSeconds;
  std::optional<int> maxCombo;
  std::optional<int> comboBreak;
  std::optional<int> badPoints;
  std::optional<std::int64_t> averageJudgeMicros;
  std::optional<float> finalGauge;
  int clearType = kClearTypeFailedRank;
  std::optional<std::string> createdAt;
  std::optional<std::string> attemptId;
  std::optional<std::string> bestOrderTime;
  ScoreBestSource source = ScoreBestSource::Local;
};

// The local equivalent of Beatoraja's one-row ScoreData record.  Aso stores
// each authenticated play independently, so this projection retains the
// particular historical fields SkinPropertyFactory reads: the first attempt
// that established the high EX score and the timestamp of the latest play.
struct ChartScoreHistorySnapshot {
  int score = 0;
  int maxScore = 0;
  int totalNotes = 0;
  std::array<int, 5> judgementCounts{};
  std::optional<std::int64_t> lastPlayedUnixSeconds;
};

// The local equivalent of Beatoraja PlayerData. PlayDataAccessor increments
// these fields for each locally recorded play, independently of the selected
// chart's score record.
struct PlayerScoreHistorySnapshot {
  int playCount = 0;
  int clearCount = 0;
  std::array<int, 5> judgementCounts{};
  std::int64_t playDurationSeconds = 0;
};

[[nodiscard]] inline bool scoreBestSnapshotIsBetter(
    const ScoreBestSnapshot &candidate,
    const std::optional<ScoreBestSnapshot> &current) {
  if (!current.has_value()) {
    return true;
  }
  if (candidate.score != current->score) {
    return candidate.score > current->score;
  }
  if (candidate.clearType != current->clearType) {
    return candidate.clearType > current->clearType;
  }
  const auto &candidateTime =
      candidate.bestOrderTime.has_value() ? candidate.bestOrderTime
                                          : candidate.createdAt;
  const auto &currentTime =
      current->bestOrderTime.has_value() ? current->bestOrderTime
                                         : current->createdAt;
  return candidateTime > currentTime;
}

struct ScoreBestByLongNoteMode {
  std::array<std::optional<ScoreBestSnapshot>, 4> snapshots{};

  [[nodiscard]] std::optional<ScoreBestSnapshot> bestForMode(int lnMode) const;
};

using ScoreRankMap = std::unordered_map<std::string, ScoreRankByLongNoteMode,
                                        TransparentStringHash,
                                        std::equal_to<>>;

struct CourseScoreRankByLongNoteMode {
  std::array<int, 4> ranks{kNoClearTypeRank, kNoClearTypeRank,
                           kNoClearTypeRank, kNoClearTypeRank};
  int wildcardRank = kNoClearTypeRank;

  [[nodiscard]] int bestRankForMode(int lnMode) const;
};

using CourseScoreRankMap =
    std::unordered_map<std::string, CourseScoreRankByLongNoteMode,
                       TransparentStringHash, std::equal_to<>>;
using LegacyCourseScoreRankMap =
    std::unordered_map<int, CourseScoreRankByLongNoteMode>;
using ScoreBestMap = std::unordered_map<std::string, ScoreBestByLongNoteMode,
                                        TransparentStringHash,
                                        std::equal_to<>>;

struct ScoreClearRankCache {
  ScoreRankMap rankBySha256;
  CourseScoreRankMap rankByCourseKey;
  LegacyCourseScoreRankMap rankByLegacyCourseId;

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta,
                                int selectedLongNoteMode = 0) const;
  [[nodiscard]] int bestRankForHash(const std::string &sha256,
                                    int longNoteMode = 0) const;
  [[nodiscard]] int bestRankForStoredKey(std::string_view sha256,
                                         int longNoteMode = 0) const;
  [[nodiscard]] int bestCourseRankFor(std::string_view courseKey,
                                      int legacyCourseId, int lnMode) const;
};

struct CourseScoreEvidence {
  int legacyCourseId = 0;
  int totalCharts = 0;
  std::string courseName;
  std::string courseGroupName;
  std::string canonicalConstraintPayload;
  std::string courseKey;

  bool operator==(const CourseScoreEvidence &) const = default;
};

struct CourseScoreRecoveryResult {
  std::string errorMessage;
  std::vector<CourseScoreEvidence> evidence;

  [[nodiscard]] bool ok() const noexcept { return errorMessage.empty(); }
};

struct ScoreBestCache {
  ScoreBestMap scoreBySha256;

  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestFor(const bms_parser::ChartMeta &chartMeta,
          int selectedLongNoteMode = 0) const;
  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestForHash(const std::string &sha256, int longNoteMode = 0) const;
  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestForStoredKey(std::string_view sha256, int longNoteMode = 0) const;
};

[[nodiscard]] int
scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &chartMeta,
                              int selectedLongNoteMode = 0);
[[nodiscard]] int scoreLongNoteModeForClearLamp(int chartLongNoteMode,
                                                int totalLongNotes,
                                                int totalBackSpinNotes,
                                                int selectedLongNoteMode = 0);
