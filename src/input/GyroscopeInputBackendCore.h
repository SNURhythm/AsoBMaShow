#pragma once

#include "GyroscopeTurntable.h"
#include "IInputBackend.h"

#include <cstdint>
#include <deque>
#include <optional>

namespace input {

enum class GyroscopeSensorCommand { None, Start, Stop };

class GyroscopeInputBackendCore {
public:
  explicit GyroscopeInputBackendCore(InputBackendSink sink);

  void start(bool supported, std::uint64_t nowMicros);
  void stop(std::uint64_t nowMicros);
  void setForeground(bool foreground, std::uint64_t nowMicros);
  void sensorStartSucceeded(std::uint64_t nowMicros);
  void sensorStartFailed(std::uint64_t nowMicros);
  void observe(const GyroscopeMotionSample &sample, std::uint64_t nowMicros);
  void pump(std::uint64_t nowMicros);
  void configure(GyroscopeTurntableConfig config, std::uint64_t nowMicros);
  void resetSession(std::uint64_t nowMicros);

  [[nodiscard]] GyroscopeSensorCommand takeCommand();

private:
  enum class SensorPhase { Stopped, StartPending, Running, Cooldown };

  void requestStart();
  void requestStop();
  void enterCooldown(std::uint64_t nowMicros);
  void publishTransition(const std::optional<float> &transition,
                         std::uint64_t nowMicros);
  void publishSnapshot(bool connected, InputDeviceStatus status);
  [[nodiscard]] bool watchdogExpired(std::uint64_t nowMicros) const;

  InputBackendSink sink_;
  GyroscopeTurntable turntable_;
  std::deque<GyroscopeSensorCommand> commands_;
  std::optional<InputDeviceSnapshot> lastSnapshot_;
  std::optional<std::uint64_t> retryAtMicros_;
  std::optional<std::uint64_t> lastFreshAtMicros_;
  std::optional<double> lastSensorTimestampSeconds_;
  std::uint64_t sensorStartedAtMicros_ = 0;
  SensorPhase phase_ = SensorPhase::Stopped;
  bool backendStarted_ = false;
  bool supported_ = false;
  bool foreground_ = true;
  bool nativeStartIssued_ = false;
  bool deviceEverPublished_ = false;
};

} // namespace input
