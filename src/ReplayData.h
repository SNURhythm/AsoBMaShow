#pragma once

#include "bms_parser.hpp"
#include "scene/play/Judge.h"
#include "scene/play/RhythmState.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace assist_options {
inline constexpr const char *kOff = "OFF";
inline constexpr const char *kDrag = "DRAG";

inline std::string normalize(std::string option) {
  option.erase(option.begin(),
               std::find_if_not(option.begin(), option.end(),
                                [](unsigned char ch) {
                                  return std::isspace(ch) != 0;
                                }));
  option.erase(std::find_if_not(option.rbegin(), option.rend(),
                                [](unsigned char ch) {
                                  return std::isspace(ch) != 0;
                                }).base(),
               option.end());
  std::transform(option.begin(), option.end(), option.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  if (option == "DRAG" || option == "DRAG-MODE") {
    return kDrag;
  }
  return kOff;
}

inline bool isEnabled(const std::string &option) {
  return normalize(option) != kOff;
}

inline bool isDragMode(const std::string &option) {
  return normalize(option) == kDrag;
}
} // namespace assist_options

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
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  std::vector<ReplayEvent> events;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
};
