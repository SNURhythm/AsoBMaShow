#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "sqlite3.h"

#include <optional>

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
  std::optional<ReplayData>
  LoadLatestReplay(const bms_parser::ChartMeta &chartMeta);
};
