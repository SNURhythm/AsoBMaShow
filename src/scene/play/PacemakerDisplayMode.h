#pragma once

#include <string_view>

namespace pacemaker {
enum class DisplayMode { Pace, ScoreMinus, ScorePlus };

inline DisplayMode displayModeFromId(std::string_view id) {
  if (id == "score_minus") return DisplayMode::ScoreMinus;
  if (id == "score_plus") return DisplayMode::ScorePlus;
  return DisplayMode::Pace;
}

inline const char *displayModeId(DisplayMode mode) {
  switch (mode) {
  case DisplayMode::ScoreMinus: return "score_minus";
  case DisplayMode::ScorePlus: return "score_plus";
  default: return "pace";
  }
}
} // namespace pacemaker
