#pragma once

#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"
#include "sqlite3.h"

#include <cstdint>
#include <string>
#include <unordered_map>

struct ScoreClearRankCache {
  std::unordered_map<std::string, int> rankBySha256;
  std::unordered_map<std::string, int> rankByMd5;
  std::unordered_map<std::string, int> rankByPath;

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta) const;
  [[nodiscard]] int bestRankForHashes(const std::string &sha256,
                                      const std::string &md5,
                                      const std::string &path = "") const;
};

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
  ScoreClearRankCache LoadBestClearRanks();
  [[nodiscard]] std::uint64_t GetRevision() const;
};
