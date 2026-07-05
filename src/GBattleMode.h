#pragma once

#include "ReplayData.h"
#include "scene/play/Pacemaker.h"

#include <algorithm>
#include <vector>

namespace gbattle {

inline constexpr const char *kTargetLabel = "G-BATTLE";

inline pacemaker::Target targetFromRecord(bms_parser::Chart &chart,
                                          const ReplayData &record) {
  if (record.autoPlay || chart.Meta.TotalNotes <= 0) {
    return {};
  }

  std::vector<int> progression =
      pacemaker::buildReplayScoreProgression(chart, record);
  if (progression.empty() ||
      progression.back() != std::max(0, record.finalScore)) {
    return {};
  }

  pacemaker::Target target;
  target.enabled = true;
  target.label = kTargetLabel;
  target.finalScore = progression.back();
  target.maxScore = std::max(0, chart.Meta.TotalNotes) * 2;
  target.totalNotes = std::max(0, chart.Meta.TotalNotes);
  target.usesReplayProgression = true;
  target.scoreAfterNotes = std::move(progression);
  return target;
}

} // namespace gbattle
