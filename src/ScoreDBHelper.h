#pragma once

#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"
#include "sqlite3.h"

#include <cstdint>
#include <functional>
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

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta) const;
  [[nodiscard]] int bestRankForHashes(const std::string &sha256,
                                      const std::string &md5,
                                      const std::string &path = "") const;
  [[nodiscard]] int bestRankForStoredKeys(std::string_view sha256,
                                          std::string_view md5,
                                          std::string_view path = "") const;
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
