#pragma once

#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"

#include <array>
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

struct ScoreBestSnapshot {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
};

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
