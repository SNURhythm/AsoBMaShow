#pragma once

#include "AssistOptionUtils.h"
#include "bms_parser.hpp"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <optional>
#include <string>
#include <vector>

enum class ReplayEventAction {
  Press = 0,
  Release = 1,
  Miss = 2,
  Mine = 3,
};

enum class ReplayTouchAction {
  Down = 0,
  Move = 1,
  Up = 2,
  Cancel = 3,
};

struct ReplayEvent {
  ReplayEventAction action = ReplayEventAction::Press;
  int lane = -1;
  long long noteTimeMicros = -1;
  long long songTimeMicros = 0;
  long long judgeTimeMicros = 0;
  Judgement judgement = None;
  long long diffMicros = 0;
  float gauge = 0.0f;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;
};

struct ReplayTouchSample {
  ReplayTouchAction action = ReplayTouchAction::Move;
  long long fingerId = 0;
  long long songTimeMicros = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct ReplayLaneCoverEvent {
  long long songTimeMicros = 0;
  int noteStartPositionPercent = 0;
  bool resetVisibleTimeReference = false;
};

struct ReplayData {
  int id = 0;
  bool autoPlay = false;
  bms_parser::ChartMeta chartMeta;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::vector<ReplayEvent> events;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
};

struct CourseReplayStageData {
  ReplayData replay;
  long long restMicrosAfterStage = 0;
};

struct CourseReplayData {
  int id = 0;
  int courseId = 0;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::string requestedPlayOption = "NORMAL";
  std::string assistOption = assist_options::kOff;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  bool gaugeAutoShift = false;
  int longNoteMode = 0;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string createdAt;
  std::vector<CourseReplayStageData> stages;
};
