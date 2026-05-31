#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "sqlite3.h"

#include <optional>
#include <string>
#include <vector>

struct ReplaySummary {
  int id = 0;
  GaugeType initialGaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  int finalScore = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  int eventCount = 0;
};

class ReplayDBHelper {
public:
  ReplayDBHelper() = default;
  ReplayDBHelper(const ReplayDBHelper &) = delete;
  ReplayDBHelper &operator=(const ReplayDBHelper &) = delete;

  static ReplayDBHelper &GetInstance();

  sqlite3 *Connect();
  void Close(sqlite3 *db);
  bool CreateReplayTables(sqlite3 *db);
  std::optional<int> SaveReplay(const ReplayData &replay);
  std::vector<ReplaySummary>
  ListReplays(const bms_parser::ChartMeta &chartMeta, int limit = 100);
  std::optional<ReplayData> LoadReplay(int replayId,
                                       const bms_parser::ChartMeta &chartMeta);
  std::optional<ReplayData>
  LoadLatestReplay(const bms_parser::ChartMeta &chartMeta);
};
