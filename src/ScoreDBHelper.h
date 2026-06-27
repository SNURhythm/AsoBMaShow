#pragma once

#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"
#include "sqlite3.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

struct TransparentStringHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
};

using ScoreRankMap =
    std::unordered_map<std::string, int, TransparentStringHash, std::equal_to<>>;

struct ScoreClearRankCache {
  ScoreRankMap rankBySha256;
  ScoreRankMap rankByMd5;
  ScoreRankMap rankByPath;
  ScoreRankMap rankByCourseId;

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta) const;
  [[nodiscard]] int bestRankForHashes(const std::string &sha256,
                                      const std::string &md5,
                                      const std::string &path = "") const;
  [[nodiscard]] int bestRankForStoredKeys(std::string_view sha256,
                                          std::string_view md5,
                                          std::string_view path = "") const;
  [[nodiscard]] int bestCourseRankForId(int courseId) const;
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

struct CoursePlaySession;

class ScoreDBHelper {
public:
  ScoreDBHelper() = default;
  ScoreDBHelper(const ScoreDBHelper &) = delete;
  ScoreDBHelper &operator=(const ScoreDBHelper &) = delete;

  static ScoreDBHelper &GetInstance();

  sqlite3 *Connect();
  void Close(sqlite3 *db);
  bool CreateScoreTable(sqlite3 *db);
  bool InsertScore(sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
                   const RhythmState &state);
  bool SaveScore(const bms_parser::ChartMeta &chartMeta,
                 const RhythmState &state);
  bool CreateCourseScoreTable(sqlite3 *db);
  bool InsertCourseScore(sqlite3 *db, const CoursePlaySession &session,
                         const RhythmState &state, int completedCharts,
                         int totalCharts);
  bool SaveCourseScore(const CoursePlaySession &session,
                       const RhythmState &state, int completedCharts,
                       int totalCharts);
  std::optional<ScoreBestSnapshot>
  LoadBestScore(const bms_parser::ChartMeta &chartMeta,
                const std::optional<std::string> &beforeCreatedAt =
                    std::nullopt);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScore(const CoursePlaySession &session);
  ScoreClearRankCache LoadBestClearRanks();
  [[nodiscard]] std::uint64_t GetRevision() const;
};
