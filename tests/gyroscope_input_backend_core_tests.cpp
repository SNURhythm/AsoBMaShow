#include "input/GyroscopeInputBackendCore.h"
#include "input/IOSGyroscopeMotionAdapter.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

input::GyroscopeMotionSample motion(double heading, double rate,
                                    double timestamp, bool usable = true,
                                    std::uint64_t accuracyGeneration = 1,
                                    bool discontinuity = false) {
  return {.headingDegrees = heading,
          .clockwiseRateDegreesPerSecond = rate,
          .sensorTimestampSeconds = timestamp,
          .accuracyGeneration = accuracyGeneration,
          .usableAccuracy = usable,
          .discontinuity = discontinuity};
}

struct Harness {
  Harness()
      : core({.enqueueInput =
                  [this](input::PhysicalInputEvent event) {
                    order.push_back("input:" +
                                    std::to_string(event.normalizedValue));
                    inputs.push_back(std::move(event));
                  },
              .enqueueDevice =
                  [this](input::InputDeviceSnapshot device) {
                    order.push_back("device:" + std::to_string(static_cast<int>(
                                                    device.status)));
                    devices.push_back(std::move(device));
                  }}) {}

  void clearEvents() {
    inputs.clear();
    devices.clear();
    order.clear();
  }

  input::GyroscopeInputBackendCore core;
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  std::vector<std::string> order;
};

void startRunning(Harness &harness, std::uint64_t nowMicros = 0) {
  harness.core.start(true, nowMicros);
  require(harness.core.takeCommand() == input::GyroscopeSensorCommand::Start,
          "supported backend requests sensor start");
  harness.core.sensorStartSucceeded(nowMicros);
}

void activateClockwise(Harness &harness, std::uint64_t firstNowMicros = 1000) {
  harness.core.observe(motion(0.0, 30.0, 1.0), firstNowMicros);
  harness.core.observe(motion(3.0, 30.0, 1.1), firstNowMicros + 100000);
  require(!harness.inputs.empty() &&
              harness.inputs.back().normalizedValue == 1.0F,
          "fixture activates clockwise output");
}

void testUnsupportedAndConnectedBeforeInput() {
  Harness unsupported;
  unsupported.core.start(false, 0);
  unsupported.core.sensorAvailable();
  unsupported.core.start(false, 1);
  unsupported.core.pump(5000000);
  require(unsupported.core.takeCommand() ==
                  input::GyroscopeSensorCommand::None &&
              unsupported.devices.empty() && unsupported.inputs.empty(),
          "unsupported hardware publishes no phantom device or command");

  Harness supported;
  supported.core.start(true, 0);
  supported.core.start(true, 1);
  require(supported.devices.empty(),
          "platform backend controls when supported hardware is advertised");
  supported.core.sensorAvailable();
  require(supported.devices.size() == 1 &&
              supported.devices.front().connected &&
              supported.devices.front().status ==
                  input::InputDeviceStatus::Calibrating,
          "supported hardware is listed while its native sensor starts");
  require(
      supported.core.takeCommand() == input::GyroscopeSensorCommand::Start &&
          supported.core.takeCommand() == input::GyroscopeSensorCommand::None,
      "start is idempotent and produces one command");
  supported.core.sensorStartSucceeded(10);
  require(supported.devices.size() == 1,
          "successful native start publishes one device");
  const auto &device = supported.devices.front();
  require(device.stableId == input::kGyroscopeTurntableStableId &&
              device.displayName == input::kGyroscopeTurntableDisplayName &&
              device.deviceClass == input::DeviceClass::Gyroscope &&
              device.connected &&
              device.status == input::InputDeviceStatus::Calibrating &&
              device.buttons == 0 && device.axes == 1 && device.hats == 0,
          "device snapshot has the stable one-axis semantic shape");

  supported.core.observe(motion(0.0, 30.0, 1.0), 1000);
  require(supported.inputs.empty() && supported.devices.size() == 2 &&
              supported.devices.back().status ==
                  input::InputDeviceStatus::Ready,
          "first usable sample marks ready and only establishes baseline");
  supported.core.observe(motion(3.0, 30.0, 1.1), 101000);
  require(supported.inputs.size() == 1 && supported.order.size() == 3 &&
              supported.order.front().starts_with("device:") &&
              supported.order[1].starts_with("device:") &&
              supported.order[2] == "input:1.000000",
          "connected and ready snapshots precede the first input");
  const auto &event = supported.inputs.front();
  require(event.control.deviceId == input::kGyroscopeTurntableStableId &&
              event.control.deviceClass == input::DeviceClass::Gyroscope &&
              event.control.kind == input::ControlKind::Axis &&
              event.control.index == input::kGyroscopeTurntableAxis &&
              event.control.direction == input::ControlDirection::Any &&
              event.rawValue == 1.0 && event.normalizedValue == 1.0F &&
              event.timestampMicros == 101000,
          "axis event carries fixed semantic identity and monotonic time");

  Harness firstStartFailure;
  firstStartFailure.core.start(true, 0);
  firstStartFailure.core.sensorAvailable();
  require(firstStartFailure.core.takeCommand() ==
              input::GyroscopeSensorCommand::Start,
          "discoverable hardware still attempts its native sensor");
  firstStartFailure.core.sensorStartFailed(0);
  require(firstStartFailure.devices.size() == 2 &&
              firstStartFailure.devices.front().connected &&
              firstStartFailure.devices.front().status ==
                  input::InputDeviceStatus::Calibrating &&
              !firstStartFailure.devices.back().connected &&
              firstStartFailure.devices.back().status ==
                  input::InputDeviceStatus::Disconnected,
          "first native-start failure remains visible in the device list");
  firstStartFailure.core.pump(2000000);
  require(firstStartFailure.devices.back().status ==
                  input::InputDeviceStatus::Retrying &&
              firstStartFailure.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Start,
          "visible first-start failure retries at the normal deadline");
}

void testCalibrationConfigAndSessionRelease() {
  Harness harness;
  startRunning(harness);
  activateClockwise(harness);
  harness.clearEvents();

  harness.core.observe(motion(6.0, 30.0, 1.2, false), 201000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.front().normalizedValue == 0.0F &&
              harness.devices.size() == 1 &&
              harness.devices.front().connected &&
              harness.devices.front().status ==
                  input::InputDeviceStatus::Calibrating &&
              harness.order.front() == "input:0.000000",
          "calibration loss releases before publishing calibrating status");

  harness.clearEvents();
  harness.core.observe(motion(9.0, 30.0, 1.3, true), 301000);
  require(harness.inputs.empty() && harness.devices.size() == 1 &&
              harness.devices.front().status == input::InputDeviceStatus::Ready,
          "calibration recovery re-baselines without input");
  harness.core.observe(motion(12.0, 30.0, 1.4, true), 401000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.back().normalizedValue == 1.0F,
          "motion resumes after recovery baseline");

  harness.clearEvents();
  harness.core.configure({.stepAngleDegrees = 6, .releaseDelayMs = 400},
                         450000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.front().normalizedValue == 0.0F,
          "configuration replacement publishes active zero");
  harness.clearEvents();
  harness.core.observe(motion(20.0, 60.0, 2.0), 500000);
  harness.core.observe(motion(23.0, 30.0, 2.1), 600000);
  require(harness.inputs.empty(),
          "configuration replacement discards baseline and old remainder");
  harness.core.observe(motion(26.0, 30.0, 2.2), 700000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.back().normalizedValue == 1.0F,
          "new six-degree step applies after re-baseline");

  harness.clearEvents();
  harness.core.resetSession(750000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.front().normalizedValue == 0.0F,
          "session reset synchronously releases active output");
  harness.clearEvents();
  harness.core.resetSession(760000);
  require(harness.inputs.empty(),
          "inactive duplicate session reset emits no redundant zero");
}

void testWatchdogDisconnectRetryAndReconnect() {
  Harness noFirstSample;
  startRunning(noFirstSample, 0);
  noFirstSample.clearEvents();
  noFirstSample.core.pump(999999);
  require(noFirstSample.core.takeCommand() ==
                  input::GyroscopeSensorCommand::None &&
              noFirstSample.devices.empty(),
          "first-sample watchdog remains live before one second");
  noFirstSample.core.pump(1000000);
  require(noFirstSample.devices.size() == 1 &&
              !noFirstSample.devices.front().connected &&
              noFirstSample.devices.front().status ==
                  input::InputDeviceStatus::Disconnected &&
              noFirstSample.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Stop,
          "missing first sample disconnects and requests stop at one second");
  noFirstSample.core.pump(2999999);
  require(noFirstSample.core.takeCommand() ==
              input::GyroscopeSensorCommand::None,
          "retry waits for the full two-second cooldown");
  noFirstSample.core.pump(3000000);
  require(noFirstSample.devices.back().status ==
                  input::InputDeviceStatus::Retrying &&
              noFirstSample.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Start,
          "retry publishes status and requests start at the deadline");
  noFirstSample.core.sensorStartFailed(3000000);
  require(noFirstSample.devices.back().status ==
              input::InputDeviceStatus::Disconnected,
          "failed retry returns to disconnected cooldown");
  noFirstSample.core.pump(5000000);
  require(noFirstSample.core.takeCommand() ==
              input::GyroscopeSensorCommand::Start,
          "failed start schedules the next retry");
  noFirstSample.core.sensorStartSucceeded(5000000);
  noFirstSample.clearEvents();
  noFirstSample.core.observe(motion(90.0, 30.0, 3.0), 5000100);
  require(noFirstSample.inputs.empty(),
          "reconnect first sample establishes a fresh baseline");
  noFirstSample.core.observe(motion(93.0, 30.0, 3.1), 5100100);
  require(noFirstSample.inputs.size() == 1,
          "input resumes after reconnect baseline");

  Harness staleTimestamp;
  startRunning(staleTimestamp, 0);
  staleTimestamp.core.observe(motion(0.0, 30.0, 1.0), 100000);
  staleTimestamp.clearEvents();
  staleTimestamp.core.observe(motion(20.0, 30.0, 1.0), 900000);
  staleTimestamp.core.observe(motion(20.0, 30.0, 0.9), 950000);
  staleTimestamp.core.pump(1100000);
  require(staleTimestamp.devices.size() == 1 &&
              staleTimestamp.devices.front().status ==
                  input::InputDeviceStatus::Disconnected &&
              staleTimestamp.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Stop,
          "equal and older timestamps do not postpone the exact watchdog");

  Harness outOfOrderBacklog;
  startRunning(outOfOrderBacklog, 0);
  outOfOrderBacklog.core.observe(motion(0.0, 30.0, 10.0), 100000);
  outOfOrderBacklog.core.observe(motion(3.0, 30.0, 10.1), 200000);
  outOfOrderBacklog.clearEvents();
  outOfOrderBacklog.core.observe(motion(100.0, 30.0, 9.0), 300000);
  outOfOrderBacklog.core.observe(motion(103.0, 30.0, 9.1), 400000);
  outOfOrderBacklog.core.observe(motion(106.0, 30.0, 9.2), 500000);
  require(outOfOrderBacklog.inputs.size() == 1 &&
              outOfOrderBacklog.inputs.front().normalizedValue == 0.0F,
          "an older timestamp backlog only releases active output");
  outOfOrderBacklog.core.observe(motion(6.0, 30.0, 10.2), 600000);
  require(outOfOrderBacklog.inputs.size() == 1,
          "the first sample beyond the high-water timestamp re-baselines");
  outOfOrderBacklog.core.observe(motion(9.0, 30.0, 10.3), 700000);
  require(outOfOrderBacklog.inputs.size() == 2 &&
              outOfOrderBacklog.inputs.back().normalizedValue == 1.0F,
          "input resumes only after the post-backlog baseline");
}

void testTransientCorrectedFrameRecoveryReprobesAndRebaselines() {
  using input::ios_gyroscope::ReferenceFrameAvailability;
  using input::ios_gyroscope::ReferenceFrameChoice;
  using input::ios_gyroscope::probeReferenceFrameForAttempt;

  Harness harness;
  harness.core.start(true, 0);
  harness.core.sensorAvailable();
  int probeCount = 0;
  const auto startAttempt = [&](std::uint64_t nowMicros) {
    const ReferenceFrameChoice choice = probeReferenceFrameForAttempt(
        false, [&]() {
          ++probeCount;
          return probeCount == 1
                     ? ReferenceFrameAvailability{
                           .deviceMotionAvailable = true}
                     : ReferenceFrameAvailability{
                           .deviceMotionAvailable = true,
                           .arbitraryCorrectedZVerticalAvailable = true};
        });
    if (choice == ReferenceFrameChoice::Unsupported) {
      harness.core.sensorStartFailed(nowMicros);
    } else {
      harness.core.sensorStartSucceeded(nowMicros);
    }
  };

  require(harness.core.takeCommand() == input::GyroscopeSensorCommand::Start,
          "supported iOS hardware requests its first corrected-frame probe");
  startAttempt(0);
  require(probeCount == 1 && harness.inputs.empty() &&
              harness.devices.back().status ==
                  input::InputDeviceStatus::Disconnected,
          "a temporarily unavailable corrected frame emits no input and "
          "enters visible retry");

  harness.clearEvents();
  harness.core.pump(2000000);
  require(harness.devices.size() == 1 &&
              harness.devices.front().status ==
                  input::InputDeviceStatus::Retrying &&
              harness.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Start,
          "corrected-frame availability is retried at the normal deadline");
  startAttempt(2000000);
  require(probeCount == 2 && harness.devices.back().connected &&
              harness.devices.back().status ==
                  input::InputDeviceStatus::Calibrating,
          "retry observes the newly available corrected frame");

  harness.clearEvents();
  harness.core.observe(motion(90.0, 30.0, 3.0), 2000100);
  require(harness.inputs.empty(),
          "corrected-frame recovery establishes a fresh heading baseline");
  harness.core.observe(motion(93.0, 30.0, 3.1), 2100100);
  require(harness.inputs.size() == 1 &&
              harness.inputs.front().normalizedValue == 1.0F,
          "input begins only after the recovered corrected-frame baseline");
}

void testActiveStallAndLifecycleOrdering() {
  Harness stalled;
  startRunning(stalled, 0);
  activateClockwise(stalled, 0);
  stalled.clearEvents();
  stalled.core.pump(1100000);
  require(stalled.inputs.size() == 1 &&
              stalled.inputs.front().normalizedValue == 0.0F &&
              stalled.devices.size() == 1 &&
              stalled.devices.front().status ==
                  input::InputDeviceStatus::Disconnected &&
              stalled.order.front() == "input:0.000000" &&
              stalled.core.takeCommand() == input::GyroscopeSensorCommand::Stop,
          "active stall releases before disconnecting and stopping");
  stalled.core.pump(3099999);
  require(stalled.core.takeCommand() == input::GyroscopeSensorCommand::None,
          "active-stall retry waits until its deadline");
  stalled.core.pump(3100000);
  require(stalled.devices.back().status == input::InputDeviceStatus::Retrying &&
              stalled.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Start,
          "active-stall retry starts exactly two seconds later");

  Harness disconnectedBackground;
  startRunning(disconnectedBackground, 0);
  disconnectedBackground.clearEvents();
  disconnectedBackground.core.pump(1000000);
  require(disconnectedBackground.devices.size() == 1 &&
              !disconnectedBackground.devices.front().connected &&
              disconnectedBackground.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Stop,
          "fixture reaches disconnected cooldown");
  disconnectedBackground.clearEvents();
  disconnectedBackground.core.setForeground(false, 1100000);
  require(disconnectedBackground.devices.empty() &&
              disconnectedBackground.core.takeCommand() ==
                  input::GyroscopeSensorCommand::None,
          "backgrounding preserves an already disconnected snapshot");
  disconnectedBackground.core.setForeground(true, 1200000);
  require(disconnectedBackground.devices.empty() &&
              disconnectedBackground.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Start,
          "foregrounding resumes retry without falsely reconnecting first");

  Harness lifecycle;
  startRunning(lifecycle, 0);
  activateClockwise(lifecycle, 0);
  lifecycle.clearEvents();
  lifecycle.core.setForeground(false, 150000);
  require(lifecycle.inputs.size() == 1 &&
              lifecycle.inputs.front().normalizedValue == 0.0F &&
              lifecycle.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Stop,
          "backgrounding releases and stops the native sensor");
  for (const auto &device : lifecycle.devices) {
    require(device.connected,
            "backgrounding never claims the hardware was unplugged");
  }
  lifecycle.clearEvents();
  lifecycle.core.setForeground(false, 160000);
  require(lifecycle.inputs.empty() && lifecycle.devices.empty() &&
              lifecycle.core.takeCommand() ==
                  input::GyroscopeSensorCommand::None,
          "duplicate background transition is idempotent");
  lifecycle.core.setForeground(true, 200000);
  lifecycle.core.setForeground(true, 210000);
  require(
      lifecycle.core.takeCommand() == input::GyroscopeSensorCommand::Start &&
          lifecycle.core.takeCommand() == input::GyroscopeSensorCommand::None,
      "foreground requests exactly one fresh native start");
  lifecycle.core.sensorStartSucceeded(220000);
  lifecycle.clearEvents();
  lifecycle.core.observe(motion(180.0, 30.0, 4.0), 230000);
  require(lifecycle.inputs.empty(),
          "foreground restart never reuses the old heading baseline");
  lifecycle.core.stop(240000);
  (void)lifecycle.core.takeCommand();
  lifecycle.core.pump(10000000);
  require(lifecycle.core.takeCommand() == input::GyroscopeSensorCommand::None,
          "stopped backend never schedules another retry");
}

void testNativeRuntimeFailureReleasesWithoutWaitingForPump() {
  Harness harness;
  startRunning(harness, 0);
  activateClockwise(harness, 0);
  harness.clearEvents();

  harness.core.sensorRuntimeFailed(250000);
  require(harness.inputs.size() == 1 &&
              harness.inputs.front().normalizedValue == 0.0F &&
              harness.inputs.front().timestampMicros == 250000 &&
              harness.devices.size() == 1 &&
              !harness.devices.front().connected &&
              harness.core.takeCommand() ==
                  input::GyroscopeSensorCommand::Stop,
          "native callback failure immediately releases and disconnects");
}

} // namespace

int main() {
  try {
    testUnsupportedAndConnectedBeforeInput();
    testCalibrationConfigAndSessionRelease();
    testWatchdogDisconnectRetryAndReconnect();
    testTransientCorrectedFrameRecoveryReprobesAndRebaselines();
    testActiveStallAndLifecycleOrdering();
    testNativeRuntimeFailureReleasesWithoutWaitingForPump();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "gyroscope_input_backend_core_tests: " << error.what() << '\n';
    return 1;
  }
}
