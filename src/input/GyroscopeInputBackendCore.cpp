#include "GyroscopeInputBackendCore.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace input {
namespace {

constexpr std::uint64_t kSensorWatchdogMicros = 1000000;
constexpr std::uint64_t kSensorRetryDelayMicros = 2000000;

bool sameSnapshot(const InputDeviceSnapshot &left,
                  const InputDeviceSnapshot &right) {
  return left.stableId == right.stableId &&
         left.displayName == right.displayName &&
         left.deviceClass == right.deviceClass &&
         left.connected == right.connected && left.status == right.status &&
         left.buttons == right.buttons && left.axes == right.axes &&
         left.hats == right.hats;
}

} // namespace

GyroscopeInputBackendCore::GyroscopeInputBackendCore(InputBackendSink sink)
    : sink_(std::move(sink)) {}

void GyroscopeInputBackendCore::start(bool supported, std::uint64_t nowMicros) {
  (void)nowMicros;
  if (backendStarted_) {
    return;
  }
  backendStarted_ = true;
  supported_ = supported;
  if (supported_ && foreground_) {
    requestStart();
  }
}

void GyroscopeInputBackendCore::sensorAvailable() {
  if (backendStarted_ && supported_) {
    publishSnapshot(true, InputDeviceStatus::Calibrating);
  }
}

void GyroscopeInputBackendCore::stop(std::uint64_t nowMicros) {
  if (!backendStarted_) {
    return;
  }
  publishTransition(turntable_.reset(), nowMicros);
  commands_.erase(std::remove(commands_.begin(), commands_.end(),
                              GyroscopeSensorCommand::Start),
                  commands_.end());
  if (phase_ == SensorPhase::Running || nativeStartIssued_) {
    requestStop();
  }
  backendStarted_ = false;
  supported_ = false;
  phase_ = SensorPhase::Stopped;
  retryAtMicros_.reset();
  lastFreshAtMicros_.reset();
  lastSensorTimestampSeconds_.reset();
}

void GyroscopeInputBackendCore::setForeground(bool foreground,
                                              std::uint64_t nowMicros) {
  if (foreground_ == foreground) {
    return;
  }
  foreground_ = foreground;
  if (!foreground_) {
    const bool wasRunning = phase_ == SensorPhase::Running;
    publishTransition(turntable_.reset(), nowMicros);
    commands_.erase(std::remove(commands_.begin(), commands_.end(),
                                GyroscopeSensorCommand::Start),
                    commands_.end());
    if (phase_ == SensorPhase::Running || nativeStartIssued_) {
      requestStop();
    }
    phase_ = SensorPhase::Stopped;
    retryAtMicros_.reset();
    lastFreshAtMicros_.reset();
    lastSensorTimestampSeconds_.reset();
    if (wasRunning && deviceEverPublished_) {
      publishSnapshot(true, InputDeviceStatus::Calibrating);
    }
    return;
  }

  if (backendStarted_ && supported_) {
    requestStart();
  }
}

void GyroscopeInputBackendCore::sensorStartSucceeded(std::uint64_t nowMicros) {
  if (!backendStarted_ || !supported_ || !foreground_ ||
      phase_ != SensorPhase::StartPending) {
    return;
  }
  phase_ = SensorPhase::Running;
  nativeStartIssued_ = true;
  sensorStartedAtMicros_ = nowMicros;
  retryAtMicros_.reset();
  lastFreshAtMicros_.reset();
  lastSensorTimestampSeconds_.reset();
  (void)turntable_.reset();
  publishSnapshot(true, InputDeviceStatus::Calibrating);
}

void GyroscopeInputBackendCore::sensorStartFailed(std::uint64_t nowMicros) {
  if (!backendStarted_ || !supported_ || phase_ != SensorPhase::StartPending) {
    return;
  }
  nativeStartIssued_ = false;
  enterCooldown(nowMicros);
}

void GyroscopeInputBackendCore::sensorRuntimeFailed(
    std::uint64_t nowMicros) {
  if (!backendStarted_ || !supported_ ||
      (phase_ != SensorPhase::Running &&
       phase_ != SensorPhase::StartPending)) {
    return;
  }
  enterCooldown(nowMicros);
}

void GyroscopeInputBackendCore::observe(const GyroscopeMotionSample &sample,
                                        std::uint64_t nowMicros) {
  if (!backendStarted_ || !foreground_ || phase_ != SensorPhase::Running) {
    return;
  }

  const bool validTimestamp = std::isfinite(sample.sensorTimestampSeconds) &&
                              sample.sensorTimestampSeconds >= 0.0;
  if (validTimestamp && lastSensorTimestampSeconds_.has_value()) {
    if (sample.sensorTimestampSeconds == *lastSensorTimestampSeconds_) {
      return;
    }
    if (sample.sensorTimestampSeconds < *lastSensorTimestampSeconds_) {
      publishTransition(turntable_.reset(), nowMicros);
      return;
    }
  }

  const bool freshTimestamp = validTimestamp;
  const auto transition = turntable_.observe(sample, nowMicros);
  if (transition.has_value() && *transition == 0.0F) {
    publishTransition(transition, nowMicros);
  }

  if (freshTimestamp) {
    lastSensorTimestampSeconds_ = sample.sensorTimestampSeconds;
    lastFreshAtMicros_ = nowMicros;
    const bool usable =
        sample.usableAccuracy && !sample.discontinuity &&
        std::isfinite(sample.headingDegrees) &&
        std::isfinite(sample.clockwiseRateDegreesPerSecond) &&
        std::abs(sample.clockwiseRateDegreesPerSecond) <= 1080.0;
    publishSnapshot(true, usable ? InputDeviceStatus::Ready
                                 : InputDeviceStatus::Calibrating);
  }

  if (!transition.has_value() || *transition != 0.0F) {
    publishTransition(transition, nowMicros);
  }
}

void GyroscopeInputBackendCore::pump(std::uint64_t nowMicros) {
  if (!backendStarted_) {
    return;
  }
  publishTransition(turntable_.advance(nowMicros), nowMicros);
  if (phase_ == SensorPhase::Running && watchdogExpired(nowMicros)) {
    enterCooldown(nowMicros);
    return;
  }
  if (phase_ == SensorPhase::Cooldown && foreground_ && supported_ &&
      retryAtMicros_.has_value() && nowMicros >= *retryAtMicros_) {
    retryAtMicros_.reset();
    if (deviceEverPublished_) {
      publishSnapshot(false, InputDeviceStatus::Retrying);
    }
    requestStart();
  }
}

void GyroscopeInputBackendCore::configure(GyroscopeTurntableConfig config,
                                          std::uint64_t nowMicros) {
  publishTransition(turntable_.configure(config), nowMicros);
}

void GyroscopeInputBackendCore::resetSession(std::uint64_t nowMicros) {
  publishTransition(turntable_.reset(), nowMicros);
}

GyroscopeSensorCommand GyroscopeInputBackendCore::takeCommand() {
  if (commands_.empty()) {
    return GyroscopeSensorCommand::None;
  }
  const GyroscopeSensorCommand command = commands_.front();
  commands_.pop_front();
  if (command == GyroscopeSensorCommand::Start) {
    nativeStartIssued_ = true;
  } else if (command == GyroscopeSensorCommand::Stop) {
    nativeStartIssued_ = false;
  }
  return command;
}

void GyroscopeInputBackendCore::requestStart() {
  if (!backendStarted_ || !supported_ || !foreground_ ||
      phase_ == SensorPhase::Running || phase_ == SensorPhase::StartPending) {
    return;
  }
  phase_ = SensorPhase::StartPending;
  if (std::ranges::find(commands_, GyroscopeSensorCommand::Start) ==
      commands_.end()) {
    commands_.push_back(GyroscopeSensorCommand::Start);
  }
}

void GyroscopeInputBackendCore::requestStop() {
  if (std::ranges::find(commands_, GyroscopeSensorCommand::Stop) ==
      commands_.end()) {
    commands_.push_back(GyroscopeSensorCommand::Stop);
  }
}

void GyroscopeInputBackendCore::enterCooldown(std::uint64_t nowMicros) {
  publishTransition(turntable_.reset(), nowMicros);
  if (phase_ == SensorPhase::Running) {
    requestStop();
  }
  phase_ = SensorPhase::Cooldown;
  retryAtMicros_ = nowMicros + kSensorRetryDelayMicros;
  lastFreshAtMicros_.reset();
  lastSensorTimestampSeconds_.reset();
  if (deviceEverPublished_) {
    publishSnapshot(false, InputDeviceStatus::Disconnected);
  }
}

void GyroscopeInputBackendCore::publishTransition(
    const std::optional<float> &transition, std::uint64_t nowMicros) {
  if (!transition.has_value() || !sink_.enqueueInput) {
    return;
  }
  sink_.enqueueInput(
      {.control = {.deviceId = std::string(kGyroscopeTurntableStableId),
                   .deviceClass = DeviceClass::Gyroscope,
                   .kind = ControlKind::Axis,
                   .index = kGyroscopeTurntableAxis,
                   .direction = ControlDirection::Any},
       .rawValue = static_cast<double>(*transition),
       .normalizedValue = *transition,
       .timestampMicros = nowMicros});
}

void GyroscopeInputBackendCore::publishSnapshot(bool connected,
                                                InputDeviceStatus status) {
  InputDeviceSnapshot snapshot{
      .stableId = std::string(kGyroscopeTurntableStableId),
      .displayName = std::string(kGyroscopeTurntableDisplayName),
      .deviceClass = DeviceClass::Gyroscope,
      .connected = connected,
      .status = status,
      .buttons = 0,
      .axes = 1,
      .hats = 0,
  };
  if (lastSnapshot_.has_value() && sameSnapshot(*lastSnapshot_, snapshot)) {
    return;
  }
  lastSnapshot_ = snapshot;
  deviceEverPublished_ = true;
  if (sink_.enqueueDevice) {
    sink_.enqueueDevice(std::move(snapshot));
  }
}

bool GyroscopeInputBackendCore::watchdogExpired(std::uint64_t nowMicros) const {
  const std::uint64_t reference =
      lastFreshAtMicros_.value_or(sensorStartedAtMicros_);
  return nowMicros >= reference &&
         nowMicros - reference >= kSensorWatchdogMicros;
}

} // namespace input
