#pragma once
#include "../i18n/Localization.h"

#include "../input/ChartLaneBinding.h"
#include "../input/InputTypes.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace settings_scene {

inline constexpr std::array<int, 12> kInputKeyModes = {4, -5, 5, 6, -7, 7, 8, 9, 10, 14, 24, 48};

constexpr bool isScratchlessInputSelection(int keyMode) {
  return keyMode == -5 || keyMode == -7;
}

constexpr input::InputScope inputScopeForSelection(int player, int keyMode) {
  return {player, keyMode};
}

struct InputActionDefinition {
  input::LogicalAction action;
  i18n::Text label;
  bool bindable = true;
};

inline bool isLegacyDigitalScratchBinding(
    const input::InputBinding &binding, input::InputScope scope) {
  if (binding.scope != scope ||
      binding.action.kind != input::LogicalActionKind::Lane ||
      (scope.keyMode != 5 && scope.keyMode != 7 && scope.keyMode != 10 &&
       scope.keyMode != 14)) {
    return false;
  }
  const int legacyScratchLane = scope.player == 1 ? 7 : 15;
  return binding.action.lane == legacyScratchLane;
}

inline std::vector<InputActionDefinition> inputActionsForScope(
    input::InputScope scope, std::span<const input::InputBinding> bindings) {
  const bool scratchless = isScratchlessInputSelection(scope.keyMode);
  std::vector<InputActionDefinition> result;
  int firstLane = 0;
  int noteLanes = scratchless ? -scope.keyMode : scope.keyMode;
  if (scope.keyMode == 10 || scope.keyMode == 14 || scope.keyMode == 48) {
    noteLanes = scope.keyMode / 2;
    firstLane = scope.player == 1 ? 0 : scope.keyMode == 48 ? 24 : 8;
  }
  for (int localLane = 0; localLane < noteLanes; ++localLane) {
    int physicalLane = firstLane + localLane;
    if (scope.player == 1) {
      physicalLane = input_profile::chartLaneForKeyPosition(
                         scope.keyMode, localLane)
                         .value_or(physicalLane);
    }
    result.push_back(
        {.action = {input::LogicalActionKind::Lane, physicalLane},
         .label = i18n::message("settings.input.actions.lane.label",
                                {{"number", std::to_string(localLane + 1)}})});
  }

  if (!scratchless && std::ranges::any_of(bindings, [scope](const auto &binding) {
        return isLegacyDigitalScratchBinding(binding, scope);
      })) {
    result.push_back(
        {.action = {input::LogicalActionKind::Lane,
                    scope.player == 1 ? 7 : 15},
         .label = i18n::message("settings.input.actions.scratch_legacy_digital.label"),
         .bindable = false});
  }
  result.push_back({.action = {input::LogicalActionKind::ScratchClockwise, 0},
                   .label = i18n::message("settings.input.actions.scratch_clockwise.label")});
  result.push_back(
      {.action = {input::LogicalActionKind::ScratchCounterClockwise, 0},
       .label = i18n::message("settings.input.actions.scratch_counter_clockwise.label")});
  result.push_back(
      {.action = {input::LogicalActionKind::Start, 0}, .label = i18n::message("settings.input.actions.start.label")});
  result.push_back(
      {.action = {input::LogicalActionKind::Select, 0}, .label = i18n::message("settings.input.actions.select.label")});
  result.push_back(
      {.action = {input::LogicalActionKind::Pause, 0}, .label = i18n::message("settings.input.actions.pause.label")});
  result.push_back(
      {.action = {input::LogicalActionKind::Retry, 0}, .label = i18n::message("settings.input.actions.retry.label")});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverIncrease, 0},
                    .label = i18n::message("settings.input.actions.lane_cover_increase.label")});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverDecrease, 0},
                    .label = i18n::message("settings.input.actions.lane_cover_decrease.label")});
  return result;
}

} // namespace settings_scene
