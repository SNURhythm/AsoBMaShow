#include "InputBindingResolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

bool controlsShareEventSource(const input::PhysicalControl &binding,
                              const input::PhysicalControl &event) {
  if (binding.deviceId != event.deviceId ||
      binding.deviceClass != event.deviceClass || binding.kind != event.kind ||
      binding.index != event.index) {
    return false;
  }
  if (binding.kind == input::ControlKind::Axis) {
    return true;
  }
  if (binding.kind == input::ControlKind::Hat &&
      event.direction == input::ControlDirection::Any) {
    return true;
  }
  return binding.direction == input::ControlDirection::Any ||
         binding.direction == event.direction;
}

float removeDeadZone(float value, float deadZone) {
  const float magnitude = std::fabs(value);
  if (magnitude <= deadZone) {
    return 0.0f;
  }
  if (deadZone >= 1.0f) {
    return 0.0f;
  }
  return std::copysign((magnitude - deadZone) / (1.0f - deadZone), value);
}

float bindingValue(const input::InputBinding &binding,
                   const input::PhysicalInputEvent &event) {
  float value = std::clamp(event.normalizedValue, -1.0f, 1.0f);
  if (binding.inverted) {
    value = -value;
  }
  value = removeDeadZone(value, binding.deadZone);

  if (binding.control.kind == input::ControlKind::Hat) {
    if (binding.control.direction != input::ControlDirection::Any &&
        binding.control.direction != event.control.direction) {
      return 0.0f;
    }
    return std::fabs(value);
  }

  switch (binding.control.direction) {
  case input::ControlDirection::Negative:
    return std::max(0.0f, -value);
  case input::ControlDirection::Positive:
    return std::max(0.0f, value);
  case input::ControlDirection::Any:
  case input::ControlDirection::Up:
  case input::ControlDirection::Right:
  case input::ControlDirection::Down:
  case input::ControlDirection::Left:
    return std::fabs(value);
  }
  return 0.0f;
}

template <typename Value>
bool contains(const std::vector<Value> &values, const Value &candidate) {
  return std::ranges::find(values, candidate) != values.end();
}

} // namespace

InputBindingResolver::InputBindingResolver(
    const InputProfile &profile, std::vector<input::InputScope> activeScopes,
    Callbacks callbacks)
    : profile_(profile), activeScopes_(std::move(activeScopes)),
      callbacks_(std::move(callbacks)),
      physicalStates_(profile_.bindings.size()) {}

void InputBindingResolver::setMode(Mode mode) {
  if (mode_ == mode) {
    return;
  }
  reset();
  mode_ = mode;
}

void InputBindingResolver::consume(const input::PhysicalInputEvent &event) {
  if (mode_ == Mode::Capture) {
    if (callbacks_.onMonitorSample) {
      callbacks_.onMonitorSample(event);
    }
    if (callbacks_.onCaptureCandidate) {
      callbacks_.onCaptureCandidate(event);
    }
    return;
  }

  std::vector<BindingEvaluation> evaluations;
  for (std::size_t index = 0; index < profile_.bindings.size(); ++index) {
    const auto &binding = profile_.bindings[index];
    if (!scopeIsActive(binding.scope) ||
        !controlsShareEventSource(binding.control, event.control)) {
      continue;
    }

    const float value = bindingValue(binding, event);
    auto &physicalState = physicalStates_[index];
    bool active = physicalState.active ? value > binding.releaseThreshold
                                       : value >= binding.activationThreshold;
    if (binding.control.kind == input::ControlKind::Hat &&
        binding.control.direction == input::ControlDirection::Any) {
      if (event.control.direction == input::ControlDirection::Any) {
        if (active) {
          physicalState.activeHatDirections.insert(
              input::ControlDirection::Any);
        } else {
          physicalState.activeHatDirections.clear();
        }
      } else {
        const bool directionWasActive =
            physicalState.activeHatDirections.contains(event.control.direction);
        const bool directionIsActive =
            directionWasActive ? value > binding.releaseThreshold
                               : value >= binding.activationThreshold;
        if (directionIsActive) {
          physicalState.activeHatDirections.insert(event.control.direction);
        } else {
          physicalState.activeHatDirections.erase(event.control.direction);
        }
      }
      active = !physicalState.activeHatDirections.empty();
    }
    evaluations.push_back(
        {.bindingIndex = index, .active = active, .value = value});
  }
  const auto timestamp =
      event.timestampMicros >
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
          ? std::numeric_limits<std::int64_t>::max()
          : static_cast<std::int64_t>(event.timestampMicros);
  applyEvaluations(evaluations, timestamp);
}

void InputBindingResolver::disconnectDevice(
    std::string_view stableId, std::int64_t steadyTimestampMicros) {
  std::vector<BindingEvaluation> evaluations;
  for (std::size_t index = 0; index < profile_.bindings.size(); ++index) {
    if (physicalStates_[index].active &&
        std::string_view(profile_.bindings[index].control.deviceId) ==
            stableId) {
      evaluations.push_back(
          {.bindingIndex = index, .active = false, .value = 0.0f});
    }
  }
  applyEvaluations(evaluations, steadyTimestampMicros);
}

void InputBindingResolver::reset(std::int64_t steadyTimestampMicros) {
  std::vector<BindingEvaluation> evaluations;
  for (std::size_t index = 0; index < physicalStates_.size(); ++index) {
    if (physicalStates_[index].active) {
      evaluations.push_back(
          {.bindingIndex = index, .active = false, .value = 0.0f});
    }
  }
  applyEvaluations(evaluations, steadyTimestampMicros);
}

std::set<input::DeviceClass> InputBindingResolver::activeDeviceClasses() const {
  std::set<input::DeviceClass> deviceClasses;
  for (const auto &binding : profile_.bindings) {
    if (scopeIsActive(binding.scope)) {
      deviceClasses.insert(binding.control.deviceClass);
    }
  }
  return deviceClasses;
}

bool InputBindingResolver::scopeIsActive(input::InputScope scope) const {
  return std::ranges::find(activeScopes_, scope) != activeScopes_.end();
}

void InputBindingResolver::applyEvaluations(
    std::span<const BindingEvaluation> evaluations,
    std::int64_t steadyTimestampMicros) {
  std::vector<LogicalStateKey> affectedKeys;
  std::map<LogicalStateKey, std::size_t> previousCounts;
  std::map<LogicalStateKey, float> pressValues;

  for (const auto &evaluation : evaluations) {
    auto &physicalState = physicalStates_[evaluation.bindingIndex];
    if (!evaluation.active) {
      physicalState.activeHatDirections.clear();
    }
    if (physicalState.active == evaluation.active) {
      continue;
    }

    const auto &binding = profile_.bindings[evaluation.bindingIndex];
    const LogicalStateKey key{binding.scope, binding.action};
    if (!contains(affectedKeys, key)) {
      affectedKeys.push_back(key);
      const auto count = logicalReferenceCounts_.find(key);
      previousCounts.emplace(
          key, count == logicalReferenceCounts_.end() ? 0 : count->second);
    }

    physicalState.active = evaluation.active;
    if (evaluation.active) {
      ++logicalReferenceCounts_[key];
      pressValues[key] = std::max(pressValues[key], evaluation.value);
      continue;
    }

    const auto count = logicalReferenceCounts_.find(key);
    if (count != logicalReferenceCounts_.end()) {
      if (count->second > 1) {
        --count->second;
      } else {
        logicalReferenceCounts_.erase(count);
      }
    }
  }

  std::vector<input::LogicalInputTransition> transitions;
  for (const auto &key : affectedKeys) {
    const std::size_t previous = previousCounts.at(key);
    const auto current = logicalReferenceCounts_.find(key);
    const std::size_t currentCount =
        current == logicalReferenceCounts_.end() ? 0 : current->second;
    if (previous > 0 && currentCount == 0) {
      transitions.push_back({.scope = key.scope,
                             .action = key.action,
                             .pressed = false,
                             .value = 0.0f,
                             .steadyTimestampMicros = steadyTimestampMicros});
    }
  }
  for (const auto &key : affectedKeys) {
    const std::size_t previous = previousCounts.at(key);
    const auto current = logicalReferenceCounts_.find(key);
    const std::size_t currentCount =
        current == logicalReferenceCounts_.end() ? 0 : current->second;
    if (previous == 0 && currentCount > 0) {
      transitions.push_back({.scope = key.scope,
                             .action = key.action,
                             .pressed = true,
                             .value = pressValues.at(key),
                             .steadyTimestampMicros = steadyTimestampMicros});
    }
  }

  if (!transitions.empty() && callbacks_.onTransitions) {
    callbacks_.onTransitions(transitions);
  }
}
