#include "InputProfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>

namespace {

constexpr std::array<int, 7> kKnownKeyModes = {4, 5, 6, 7, 8, 10, 14};

bool isExactDuplicate(const input::InputBinding &left,
                      const input::InputBinding &right) {
  return left.id == right.id && left.scope == right.scope &&
         left.action == right.action && left.control == right.control &&
         left.deadZone == right.deadZone &&
         left.activationThreshold == right.activationThreshold &&
         left.releaseThreshold == right.releaseThreshold &&
         left.inverted == right.inverted;
}

bool hasValidThresholdOrder(const input::InputBinding &binding) {
  return binding.deadZone >= 0.0f &&
         binding.deadZone < binding.releaseThreshold &&
         binding.releaseThreshold < binding.activationThreshold &&
         binding.activationThreshold <= 1.0f;
}

} // namespace

void InputProfile::sanitize(std::vector<std::string> &diagnostics) {
  if (schemaVersion != kSchemaVersion) {
    schemaVersion = kSchemaVersion;
    diagnostics.emplace_back("Reset unsupported input schema version to 1.");
  }

  for (auto &binding : bindings) {
    if (binding.scope.player != 1 && binding.scope.player != 2) {
      binding.scope.player = binding.scope.player > 1 ? 2 : 1;
      diagnostics.emplace_back("Clamped input binding player to 1 or 2.");
    }

    if (std::ranges::find(kKnownKeyModes, binding.scope.keyMode) ==
        kKnownKeyModes.end()) {
      binding.scope.keyMode = 7;
      diagnostics.emplace_back("Reset unknown input key mode to 7.");
    }

    if (!std::isfinite(binding.deadZone) ||
        !std::isfinite(binding.releaseThreshold) ||
        !std::isfinite(binding.activationThreshold)) {
      binding.deadZone = 0.0f;
      binding.releaseThreshold = 0.35f;
      binding.activationThreshold = 0.5f;
      diagnostics.emplace_back("Reset non-finite input thresholds.");
    } else {
      binding.deadZone = std::clamp(binding.deadZone, 0.0f, 1.0f);
      binding.releaseThreshold =
          std::clamp(binding.releaseThreshold, 0.0f, 1.0f);
      binding.activationThreshold =
          std::clamp(binding.activationThreshold, 0.0f, 1.0f);
    }

    if (!hasValidThresholdOrder(binding)) {
      binding.deadZone = 0.0f;
      binding.releaseThreshold = 0.35f;
      binding.activationThreshold = 0.5f;
      diagnostics.emplace_back(
          "Reset input thresholds to preserve hysteresis ordering.");
    }
  }

  std::vector<input::InputBinding> uniqueBindings;
  uniqueBindings.reserve(bindings.size());
  for (const auto &binding : bindings) {
    const bool duplicate = std::ranges::any_of(
        uniqueBindings, [&](const input::InputBinding &existing) {
          return isExactDuplicate(existing, binding);
        });
    if (duplicate) {
      diagnostics.emplace_back("Removed an exact duplicate input binding.");
      continue;
    }
    uniqueBindings.push_back(binding);
  }
  bindings = std::move(uniqueBindings);
}

std::vector<std::reference_wrapper<const input::InputBinding>>
InputProfile::bindingsFor(input::InputScope scope) const {
  std::vector<std::reference_wrapper<const input::InputBinding>> matches;
  for (const auto &binding : bindings) {
    if (binding.scope == scope) {
      matches.emplace_back(std::cref(binding));
    }
  }
  return matches;
}

std::vector<input::InputBinding>
InputProfile::conflictsWith(const input::InputBinding &candidate) const {
  std::vector<input::InputBinding> conflicts;
  for (const auto &binding : bindings) {
    if (binding.id != candidate.id && binding.scope == candidate.scope &&
        binding.control == candidate.control) {
      conflicts.push_back(binding);
    }
  }
  return conflicts;
}

bool InputProfile::hasDigitalBinding(input::InputScope scope,
                                     input::LogicalAction action,
                                     std::string_view deviceId,
                                     int scancode) const {
  return std::ranges::any_of(bindings, [&](const input::InputBinding &binding) {
    return binding.scope == scope && binding.action == action &&
           binding.control.deviceClass == input::DeviceClass::Keyboard &&
           binding.control.kind == input::ControlKind::Key &&
           binding.control.direction == input::ControlDirection::Any &&
           std::string_view(binding.control.deviceId) == deviceId &&
           binding.control.index == scancode;
  });
}
