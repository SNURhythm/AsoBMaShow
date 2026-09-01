#pragma once

#include "../input/ChartLaneBinding.h"
#include "../input/InputTypes.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace settings_scene {

struct InputActionDefinition {
  input::LogicalAction action;
  std::string label;
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
  std::vector<InputActionDefinition> result;
  int firstLane = 0;
  int noteLanes = scope.keyMode;
  if (scope.keyMode == 10 || scope.keyMode == 14) {
    noteLanes = scope.keyMode / 2;
    firstLane = scope.player == 1 ? 0 : 8;
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
         .label = "Lane " + std::to_string(localLane + 1)});
  }

  if (std::ranges::any_of(bindings, [scope](const auto &binding) {
        return isLegacyDigitalScratchBinding(binding, scope);
      })) {
    result.push_back(
        {.action = {input::LogicalActionKind::Lane,
                    scope.player == 1 ? 7 : 15},
         .label = "Scratch (legacy digital)",
         .bindable = false});
  }
  result.push_back({.action = {input::LogicalActionKind::ScratchClockwise, 0},
                    .label = "Scratch clockwise"});
  result.push_back(
      {.action = {input::LogicalActionKind::ScratchCounterClockwise, 0},
       .label = "Scratch counter-clockwise"});
  result.push_back(
      {.action = {input::LogicalActionKind::Start, 0}, .label = "Start"});
  result.push_back(
      {.action = {input::LogicalActionKind::Select, 0}, .label = "Select"});
  result.push_back(
      {.action = {input::LogicalActionKind::Pause, 0}, .label = "Pause"});
  result.push_back(
      {.action = {input::LogicalActionKind::Retry, 0}, .label = "Retry"});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverIncrease, 0},
                    .label = "Lane cover increase"});
  result.push_back({.action = {input::LogicalActionKind::LaneCoverDecrease, 0},
                    .label = "Lane cover decrease"});
  return result;
}

} // namespace settings_scene
