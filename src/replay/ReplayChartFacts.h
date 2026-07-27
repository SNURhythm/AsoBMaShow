#pragma once

#include "ReplayPlaybackData.h"
#include "../bms_parser.hpp"

namespace replay {

// Facts that must be captured from the authored RANDOM branch before gameplay
// constraints or user-selected modes materialize the chart metadata.
struct AuthoredReplayChartFacts {
  bool hasUndefinedLongNotes = false;

  bool operator==(const AuthoredReplayChartFacts &) const = default;
};

[[nodiscard]] inline AuthoredReplayChartFacts captureAuthoredReplayChartFacts(
    const bms_parser::ChartMeta &authoredMeta) noexcept {
  return {.hasUndefinedLongNotes = hasUndefinedLongNotesForReplay(
              authoredMeta.LnMode, authoredMeta.TotalLongNotes,
              authoredMeta.TotalBackSpinNotes)};
}

[[nodiscard]] inline AuthoredReplayChartFacts captureAuthoredReplayChartFacts(
    const bms_parser::Chart &authoredChart) noexcept {
  return captureAuthoredReplayChartFacts(authoredChart.Meta);
}

} // namespace replay
