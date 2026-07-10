#include "InputCaptureController.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace {

bool sameCapture(const input::InputBinding &binding, input::InputScope scope,
                 input::LogicalAction action,
                 const input::PhysicalControl &control) {
  return binding.scope == scope && binding.action == action &&
         binding.control == control;
}

bool sameEditableValues(const input::InputBinding &left,
                        const input::InputBinding &right) {
  return left.deadZone == right.deadZone &&
         left.activationThreshold == right.activationThreshold &&
         left.releaseThreshold == right.releaseThreshold &&
         left.inverted == right.inverted;
}

bool isDirectionalAnalog(input::ControlKind kind) {
  return kind == input::ControlKind::Axis;
}

template <typename Bindings>
auto findUniqueBinding(Bindings &bindings, std::string_view bindingId)
    -> std::pair<decltype(bindings.begin()), bool> {
  auto match = bindings.end();
  for (auto candidate = bindings.begin(); candidate != bindings.end();
       ++candidate) {
    if (std::string_view(candidate->id) != bindingId) {
      continue;
    }
    if (match != bindings.end()) {
      return {bindings.end(), true};
    }
    match = candidate;
  }
  return {match, false};
}

} // namespace

InputCaptureController::InputCaptureController(InputDeviceRegistry &registry,
                                               InputProfile &profile,
                                               SaveCallback save)
    : registry_(registry), profile_(profile), save_(std::move(save)),
      resolver_(profile_, {},
                {.onMonitorSample =
                     [this](const input::PhysicalInputEvent &event) {
                       observeSample(event);
                     },
                 .onCaptureCandidate =
                     [this](const input::PhysicalInputEvent &event) {
                       considerCandidate(event);
                     }}) {
  resolver_.setMode(InputBindingResolver::Mode::Capture);
  inputSubscription_ =
      registry_.subscribeInput([this](const input::PhysicalInputEvent &event) {
        resolver_.consume(event);
      });
  deviceSubscription_ = registry_.subscribeDevices(
      [this](const input::InputDeviceSnapshot &device) {
        observeDevice(device);
      });
}

InputCaptureController::~InputCaptureController() {
  if (inputSubscription_ != 0) {
    registry_.unsubscribe(inputSubscription_);
  }
  if (deviceSubscription_ != 0) {
    registry_.unsubscribe(deviceSubscription_);
  }
}

void InputCaptureController::begin(input::InputScope scope,
                                   input::LogicalAction action) {
  targetScope_ = scope;
  targetAction_ = action;
  clearPending();
  lastError_.clear();
  state_ = State::Listening;
}

void InputCaptureController::cancel() {
  clearPending();
  lastError_.clear();
  state_ = State::Idle;
}

void InputCaptureController::confirmReplace() {
  if (state_ != State::AwaitingConflictConfirmation ||
      !pendingBinding_.has_value()) {
    return;
  }

  InputProfile next = profile_;
  const auto &candidate = *pendingBinding_;
  std::erase_if(next.bindings, [&](const input::InputBinding &binding) {
    return binding.scope == candidate.scope &&
           binding.control == candidate.control &&
           binding.action != candidate.action;
  });
  next.bindings.push_back(candidate);
  if (persist(std::move(next))) {
    clearPending();
    state_ = State::Idle;
  }
}

void InputCaptureController::rejectReplace() {
  if (state_ != State::AwaitingConflictConfirmation) {
    return;
  }
  clearPending();
  lastError_.clear();
  state_ = State::Idle;
}

void InputCaptureController::updateBinding(std::string_view bindingId,
                                           const BindingEdit &edit) {
  lastError_.clear();
  const auto [current, ambiguous] =
      findUniqueBinding(profile_.bindings, bindingId);
  if (ambiguous) {
    lastError_ = "Input binding ID is ambiguous; edit was not saved.";
    return;
  }
  if (current == profile_.bindings.end()) {
    return;
  }
  const std::size_t bindingIndex =
      static_cast<std::size_t>(current - profile_.bindings.begin());

  input::InputBinding edited = *current;
  if (edit.deadZone.has_value()) {
    edited.deadZone = *edit.deadZone;
  }
  if (edit.activationThreshold.has_value()) {
    edited.activationThreshold = *edit.activationThreshold;
  }
  if (edit.releaseThreshold.has_value()) {
    edited.releaseThreshold = *edit.releaseThreshold;
  }
  if (edit.inverted.has_value()) {
    edited.inverted = *edit.inverted;
  }

  InputProfile sanitizer{.bindings = {edited}};
  std::vector<std::string> diagnostics;
  sanitizer.sanitize(diagnostics);
  edited = std::move(sanitizer.bindings.front());
  if (sameEditableValues(*current, edited)) {
    return;
  }

  InputProfile next = profile_;
  next.bindings[bindingIndex] = std::move(edited);
  persist(std::move(next));
}

void InputCaptureController::toggleBindingInversion(
    std::string_view bindingId) {
  lastError_.clear();
  const auto [current, ambiguous] =
      findUniqueBinding(profile_.bindings, bindingId);
  if (ambiguous) {
    lastError_ = "Input binding ID is ambiguous; edit was not saved.";
    return;
  }
  if (current == profile_.bindings.end()) {
    return;
  }
  updateBinding(bindingId, {.inverted = !current->inverted});
}

void InputCaptureController::resetScopeToDefaults(input::InputScope scope) {
  InputProfile next = profile_;
  std::erase_if(next.bindings, [scope](const input::InputBinding &binding) {
    return binding.scope == scope;
  });

  const InputProfile defaults = makeDefaultInputProfile();
  for (const auto &binding : defaults.bindings) {
    if (binding.scope == scope) {
      next.bindings.push_back(binding);
    }
  }
  persist(std::move(next));
}

InputCaptureController::State InputCaptureController::state() const {
  return state_;
}

std::optional<input::PhysicalInputEvent>
InputCaptureController::monitorSample() const {
  return monitorSample_;
}

std::span<const input::InputBinding>
InputCaptureController::pendingConflicts() const {
  return pendingConflicts_;
}

bool InputCaptureController::isBindingDeviceMissing(
    std::string_view bindingId) const {
  const auto [binding, ambiguous] =
      findUniqueBinding(profile_.bindings, bindingId);
  if (ambiguous || binding == profile_.bindings.end()) {
    return false;
  }
  return binding->control.deviceId.empty() ||
         !registry_.isConnected(binding->control.deviceId);
}

std::string_view InputCaptureController::lastError() const {
  return lastError_;
}

void InputCaptureController::observeDevice(
    const input::InputDeviceSnapshot &device) {
  if (device.connected) {
    return;
  }
  std::erase_if(activationStates_, [&](const auto &entry) {
    return entry.first.deviceId == device.stableId;
  });
}

void InputCaptureController::observeSample(
    const input::PhysicalInputEvent &event) {
  monitorSample_ = event;
}

void InputCaptureController::considerCandidate(
    const input::PhysicalInputEvent &event) {
  if (!std::isfinite(event.normalizedValue)) {
    return;
  }

  if (isDirectionalAnalog(event.control.kind)) {
    input::PhysicalControl negative = event.control;
    negative.direction = input::ControlDirection::Negative;
    considerControlActivation(event, std::move(negative),
                              std::max(0.0F, -event.normalizedValue));
    input::PhysicalControl positive = event.control;
    positive.direction = input::ControlDirection::Positive;
    considerControlActivation(event, std::move(positive),
                              std::max(0.0F, event.normalizedValue));
    return;
  }

  considerControlActivation(event, event.control,
                            std::fabs(event.normalizedValue));
}

void InputCaptureController::considerControlActivation(
    const input::PhysicalInputEvent &, input::PhysicalControl control,
    float value) {
  const bool wasActive = activationStates_[control];
  const bool active =
      control.kind == input::ControlKind::Axis
          ? (wasActive ? value > kAxisCaptureReleaseThreshold
                       : value >= kAxisCaptureActivationThreshold)
          : value >= kNonAxisCaptureActivationThreshold;
  activationStates_[control] = active;
  if (wasActive || !active || state_ != State::Listening) {
    return;
  }
  stageCandidate(std::move(control));
}

void InputCaptureController::stageCandidate(input::PhysicalControl control) {
  const bool duplicate = std::ranges::any_of(
      profile_.bindings, [&](const input::InputBinding &binding) {
        return sameCapture(binding, targetScope_, targetAction_, control);
      });
  if (duplicate) {
    return;
  }

  input::InputBinding candidate{
      .id = makeBindingId(),
      .scope = targetScope_,
      .action = targetAction_,
      .control = std::move(control),
  };
  pendingConflicts_ = profile_.conflictsWith(candidate);
  if (!pendingConflicts_.empty()) {
    pendingBinding_ = std::move(candidate);
    state_ = State::AwaitingConflictConfirmation;
    return;
  }

  InputProfile next = profile_;
  next.bindings.push_back(std::move(candidate));
  if (persist(std::move(next))) {
    state_ = State::Idle;
  }
}

bool InputCaptureController::persist(InputProfile nextProfile) {
  lastError_.clear();
  if (!save_) {
    lastError_ = "Input profile persistence is not configured.";
    return false;
  }

  std::string errorMessage;
  bool saved = false;
  try {
    saved = save_(nextProfile, errorMessage);
  } catch (const std::exception &error) {
    errorMessage = error.what();
  } catch (...) {
    errorMessage = "Unknown input profile save failure.";
  }
  if (!saved) {
    lastError_ = errorMessage.empty() ? "Failed to save input profile."
                                      : std::move(errorMessage);
    return false;
  }

  profile_ = std::move(nextProfile);
  return true;
}

void InputCaptureController::clearPending() {
  pendingBinding_.reset();
  pendingConflicts_.clear();
}

std::string InputCaptureController::makeBindingId() {
  for (;;) {
    std::string candidate =
        "capture-binding-" + std::to_string(nextBindingId_++);
    const bool exists = std::ranges::any_of(
        profile_.bindings, [&](const input::InputBinding &binding) {
          return binding.id == candidate;
        });
    if (!exists) {
      return candidate;
    }
  }
}
