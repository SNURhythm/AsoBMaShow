#pragma once

#include "ReplayData.h"
#include "ResultContracts.h"
#include "scene/play/Pacemaker.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace gbattle {

inline constexpr const char *kTargetLabel = "G-BATTLE";

inline pacemaker::Target targetFromRecord(bms_parser::Chart &chart,
                                          const ReplayData &record) {
  const auto maximumScore =
      result_contract::maximumScoreForNotes(chart.Meta.TotalNotes);
  if (record.autoPlay || chart.Meta.TotalNotes <= 0 || !maximumScore) {
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
  target.maxScore = *maximumScore;
  target.totalNotes = std::max(0, chart.Meta.TotalNotes);
  target.usesReplayProgression = true;
  target.scoreAfterNotes = std::move(progression);
  return target;
}

inline std::optional<ResultPacemakerData>
resultPacemakerDataFromRecord(bms_parser::Chart &chart,
                              const RhythmState &state,
                              const ReplayData &record) {
  pacemaker::Target target = targetFromRecord(chart, record);
  if (!target.enabled) {
    return std::nullopt;
  }

  return ResultPacemakerData{
      .label = target.label,
      .targetScore = target.finalScore,
      .delta = state.getScore() - target.finalScore,
      .usesReplayProgression = target.usesReplayProgression,
  };
}

} // namespace gbattle
