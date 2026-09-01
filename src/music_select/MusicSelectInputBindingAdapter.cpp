#include "MusicSelectInputBindingAdapter.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

bool sameEventSource(const input::PhysicalControl &binding,
                     const input::PhysicalControl &event) {
  return binding.deviceId == event.deviceId &&
         binding.deviceClass == event.deviceClass &&
         binding.kind == event.kind && binding.index == event.index;
}

float sourceAnalogValue(const input::InputBinding &binding,
                        const input::PhysicalInputEvent &event) {
  float value = std::clamp(event.normalizedValue, -1.0F, 1.0F);
  if (binding.inverted) value = -value;
  if (binding.control.direction == input::ControlDirection::Negative) {
    value = -value;
  }
  return value;
}

int sourceAnalogDifference(float previous, float current) {
  constexpr float kTickMaximumSize = 0.009F;
  float difference = current - previous;
  if (difference > 1.0F) {
    difference -= 2.0F + kTickMaximumSize / 2.0F;
  } else if (difference < -1.0F) {
    difference += 2.0F + kTickMaximumSize / 2.0F;
  }
  difference /= kTickMaximumSize;
  return static_cast<int>(difference > 0.0F ? std::ceil(difference)
                                           : std::floor(difference));
}

} // namespace

MusicSelectKeyLayout
musicSelectKeyLayoutForConfig(int musicSelectInput) noexcept {
  return static_cast<MusicSelectKeyLayout>(musicSelectInput);
}

std::vector<input::InputScope>
musicSelectInputScopes(MusicSelectKeyLayout layout) {
  switch (layout) {
  case MusicSelectKeyLayout::Popn9K: return {{.player = 1, .keyMode = 9}};
  case MusicSelectKeyLayout::Beat14K:
    return {{.player = 1, .keyMode = 14},
            {.player = 2, .keyMode = 14}};
  case MusicSelectKeyLayout::Beat7K:
    return {{.player = 1, .keyMode = 7}};
  }
  return {};
}

std::optional<std::size_t>
musicSelectKeyIndex(MusicSelectKeyLayout layout, input::InputScope scope,
                    input::LogicalAction action) noexcept {
  const int keyMode = layout == MusicSelectKeyLayout::Popn9K
                          ? 9
                          : layout == MusicSelectKeyLayout::Beat14K ? 14 : 7;
  if (scope.keyMode != keyMode) return std::nullopt;
  if (action.kind == input::LogicalActionKind::Lane) {
    if (layout == MusicSelectKeyLayout::Beat14K) {
      if (scope.player == 1 && action.lane >= 0 && action.lane <= 6) {
        return static_cast<std::size_t>(action.lane);
      }
      if (scope.player == 2 && action.lane >= 8 && action.lane <= 14) {
        return static_cast<std::size_t>(action.lane + 1);
      }
      return std::nullopt;
    }
    if (scope.player == 1 && action.lane >= 0 && action.lane < keyMode) {
      return static_cast<std::size_t>(action.lane);
    }
    return std::nullopt;
  }
  if (layout == MusicSelectKeyLayout::Popn9K ||
      (action.kind != input::LogicalActionKind::ScratchClockwise &&
       action.kind != input::LogicalActionKind::ScratchCounterClockwise)) {
    return std::nullopt;
  }
  const bool clockwise =
      action.kind == input::LogicalActionKind::ScratchClockwise;
  if (layout == MusicSelectKeyLayout::Beat14K) {
    if (scope.player == 1) {
      return static_cast<std::size_t>(clockwise ? 8 : 7);
    }
    if (scope.player == 2) {
      return static_cast<std::size_t>(clockwise ? 17 : 16);
    }
    return std::nullopt;
  }
  return scope.player == 1
             ? std::optional<std::size_t>(clockwise ? 8 : 7)
             : std::nullopt;
}

MusicSelectInputBindingAdapter::MusicSelectInputBindingAdapter(
    const InputProfile &profile, MusicSelectKeyLayout layout)
    : profile_(profile), layout_(layout),
      resolver_(profile_, musicSelectInputScopes(layout_),
                {.onTransitions =
                     [this](std::span<const input::LogicalInputTransition>
                                transitions) { apply(transitions); }}) {
  state_.keys.resize(layout_ == MusicSelectKeyLayout::Beat14K ? 18 : 9);
  state_.changed.resize(state_.keys.size());
  state_.analog.resize(state_.keys.size());
  state_.analogDelta.resize(state_.keys.size());
  for (const auto &binding : profile_.bindings) {
    const auto index = musicSelectKeyIndex(layout_, binding.scope,
                                           binding.action);
    if (index && binding.control.kind == input::ControlKind::Axis) {
      state_.analog[*index] = true;
    }
  }
}

void MusicSelectInputBindingAdapter::consume(
    const input::PhysicalInputEvent &event) {
  consumeAnalog(event);
  resolver_.consume(event);
}

void MusicSelectInputBindingAdapter::disconnectDevice(
    std::string_view stableId) {
  resolver_.disconnectDevice(stableId);
}

void MusicSelectInputBindingAdapter::reset() {
  resolver_.reset();
  heldKeys_.clear();
  heldStart_.clear();
  heldSelect_.clear();
  std::ranges::fill(state_.keys, false);
  std::ranges::fill(state_.changed, false);
  std::ranges::fill(state_.analogDelta, 0);
  state_.start = false;
  state_.select = false;
  analogValues_.clear();
}

void MusicSelectInputBindingAdapter::clearFrameEdges() {
  std::ranges::fill(state_.changed, false);
  std::ranges::fill(state_.analogDelta, 0);
  state_.controlPressed.clear();
  state_.commands.clear();
  state_.wheel = 0;
  state_.selectedBarChanged = false;
}

void MusicSelectInputBindingAdapter::apply(
    std::span<const input::LogicalInputTransition> transitions) {
  for (const auto &transition : transitions) {
    const LogicalKey key{transition.scope, transition.action};
    if (transition.action.kind == input::LogicalActionKind::Start) {
      if (transition.pressed)
        heldStart_.insert(key);
      else
        heldStart_.erase(key);
      state_.start = !heldStart_.empty();
      continue;
    }
    if (transition.action.kind == input::LogicalActionKind::Select) {
      if (transition.pressed)
        heldSelect_.insert(key);
      else
        heldSelect_.erase(key);
      state_.select = !heldSelect_.empty();
      continue;
    }
    const auto index = musicSelectKeyIndex(layout_, transition.scope,
                                           transition.action);
    if (!index) continue;
    const bool wasPressed = !heldKeys_[*index].empty();
    if (transition.pressed)
      heldKeys_[*index].insert(key);
    else
      heldKeys_[*index].erase(key);
    const bool pressed = !heldKeys_[*index].empty();
    state_.keys[*index] = pressed;
    if (!wasPressed && pressed) state_.changed[*index] = true;
  }
}

void MusicSelectInputBindingAdapter::consumeAnalog(
    const input::PhysicalInputEvent &event) {
  if (event.control.kind != input::ControlKind::Axis) return;
  for (std::size_t bindingIndex = 0;
       bindingIndex < profile_.bindings.size(); ++bindingIndex) {
    const auto &binding = profile_.bindings[bindingIndex];
    if (binding.control.kind != input::ControlKind::Axis ||
        !sameEventSource(binding.control, event.control)) {
      continue;
    }
    const auto index = musicSelectKeyIndex(layout_, binding.scope,
                                           binding.action);
    if (!index) continue;
    const float current = sourceAnalogValue(binding, event);
    const float previous = analogValues_[bindingIndex];
    state_.analogDelta[*index] +=
        sourceAnalogDifference(previous, current);
    analogValues_[bindingIndex] = current;
  }
}
