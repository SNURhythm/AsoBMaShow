#include "ResultGaugeHistory.h"

#include <algorithm>

namespace result_gauge_history {
namespace {

bool hasTypedHistory(const GameplayScoreState &state, GaugeType type) {
  return !state.gaugeHistoryFor(type).empty();
}

void appendUnique(std::vector<GaugeType> &types, GaugeType type) {
  if (std::ranges::find(types, type) == types.end()) {
    types.push_back(type);
  }
}

} // namespace

std::vector<GaugeType> availableTypes(const GameplayScoreState &state) {
  std::vector<GaugeType> types;
  types.reserve(kGaugeTypeCount);
  appendUnique(types, state.gaugeType);

  switch (state.gaugeAutoShift) {
  case GaugeAutoShiftMode::BestClear:
  case GaugeAutoShiftMode::SelectToUnder: {
    const int upper =
        state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
            ? gaugeTypeIndex(state.selectedGaugeType)
            : (gaugeProfileIsCourse(state.gaugeProfile)
                   ? gaugeTypeIndex(GaugeType::ExHard)
                   : gaugeTypeIndex(GaugeType::Hazard));
    const int lower = gaugeProfileIsCourse(state.gaugeProfile)
                          ? gaugeTypeIndex(GaugeType::Normal)
                          : std::min(gaugeTypeIndex(
                                         state.gaugeAutoShiftLowerBound),
                                     upper);
    for (int index = upper; index >= lower; --index) {
      const GaugeType type = gaugeTypeAtIndex(index);
      if (hasTypedHistory(state, type)) {
        appendUnique(types, type);
      }
    }
    break;
  }
  case GaugeAutoShiftMode::SurvivalToGroove:
    if (hasTypedHistory(state, state.selectedGaugeType)) {
      appendUnique(types, state.selectedGaugeType);
    }
    if (hasTypedHistory(state, GaugeType::Normal)) {
      appendUnique(types, GaugeType::Normal);
    }
    break;
  case GaugeAutoShiftMode::None:
  case GaugeAutoShiftMode::Continue:
    break;
  }
  return types;
}

GaugeType initialType(const GameplayScoreState &state) {
  return availableTypes(state).front();
}

GaugeType nextType(const GameplayScoreState &state, GaugeType current) {
  const auto types = availableTypes(state);
  const auto found = std::ranges::find(types, current);
  if (found == types.end() || std::next(found) == types.end()) {
    return types.front();
  }
  return *std::next(found);
}

const std::vector<float> &historyFor(const GameplayScoreState &state,
                                     GaugeType type) {
  const auto &typed = state.gaugeHistoryFor(type);
  if (!typed.empty()) {
    return typed;
  }
  return state.gaugeHistory;
}

} // namespace result_gauge_history
