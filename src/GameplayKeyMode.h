#pragma once

#include "bms_parser.hpp"

#include <string>

namespace gameplay {

// Settings identities. Parser, score, replay and IR key modes stay canonical;
// negative values select independent scratchless skin and input settings.
[[nodiscard]] inline bool isScratchlessSinglePlay(const bms_parser::Chart &chart) {
  const auto &meta = chart.Meta;
  if ((meta.KeyMode != 5 && meta.KeyMode != 7) || meta.IsDP ||
      meta.TotalScratchNotes != 0 || meta.TotalBackSpinNotes != 0) return false;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) return false;
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) return false;
      const auto occupied = [](const auto &notes) {
        return notes.size() > 7 && notes[7] != nullptr;
      };
      if (occupied(timeline->Notes) || occupied(timeline->InvisibleNotes) ||
          occupied(timeline->LandmineNotes)) return false;
    }
  }
  return true;
}

[[nodiscard]] inline int presentationKeyMode(const bms_parser::Chart &chart) {
  return isScratchlessSinglePlay(chart) ? -chart.Meta.KeyMode : chart.Meta.KeyMode;
}

[[nodiscard]] inline std::string keyModeLabel(int keyMode) {
  switch (keyMode) {
  case -5: return "5K";
  case -7: return "7K";
  case 5: return "5K1S";
  case 7: return "7K1S";
  case 10: return "5KDP";
  case 14: return "7KDP";
  default: return keyMode > 0 ? std::to_string(keyMode) + "K" : std::string{};
  }
}

} // namespace gameplay
