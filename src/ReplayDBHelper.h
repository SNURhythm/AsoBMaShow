#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "sqlite3.h"

#include <optional>
#include <string>
#include <vector>

struct ReplaySummary {
  int id = 0;
  bool courseReplay = false;
  bool autoPlay = false;
  GaugeType initialGaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  int finalScore = 0;
  int maxScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  int eventCount = 0;
  int touchSampleCount = 0;
  std::optional<bms_parser::ChartMeta> chartMeta;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  int completedCharts = 0;
  int totalCharts = 0;
  int stageCount = 0;
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
  std::optional<int> SaveCourseReplay(const CourseReplayData &replay);
  std::vector<ReplaySummary> ListReplays(const bms_parser::ChartMeta &chartMeta,
                                         int limit = 100);
  std::vector<ReplaySummary> ListCourseReplays(int courseId,
                                               int limit = 100);
  std::optional<ReplayData> LoadReplay(int replayId,
                                       const bms_parser::ChartMeta &chartMeta);
  std::optional<CourseReplayData> LoadCourseReplay(int replayId);
  std::optional<ReplayData>
  LoadLatestReplay(const bms_parser::ChartMeta &chartMeta);
};
