#include "GyroscopePlatformBackends.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "GyroscopeInputBackendCore.h"
#include "IOSGyroscopeMotionAdapter.h"

#import <CoreMotion/CoreMotion.h>
#include <SDL2/SDL_log.h>
#include <mach/mach_time.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr std::uint64_t kNativeRetryDelayMicros = 500000;
constexpr std::uint64_t kFirstSampleTimeoutMicros = 2000000;

mach_timebase_info_data_t machTimebase() {
  mach_timebase_info_data_t value{};
  if (mach_timebase_info(&value) != KERN_SUCCESS || value.denom == 0) {
    value.numer = 1;
    value.denom = 1;
  }
  return value;
}

std::uint64_t nowMicros() {
  static const mach_timebase_info_data_t timebase = machTimebase();
  const auto nanoseconds = static_cast<unsigned __int128>(mach_absolute_time()) *
                           timebase.numer / timebase.denom;
  return static_cast<std::uint64_t>(nanoseconds / 1000U);
}

struct AvailableReferenceFrame {
  CMAttitudeReferenceFrame mask = static_cast<CMAttitudeReferenceFrame>(0);
  input::ios_gyroscope::ReferenceFrameChoice choice =
      input::ios_gyroscope::ReferenceFrameChoice::Unsupported;
};

AvailableReferenceFrame probeAvailableReferenceFrame(CMMotionManager *manager) {
#if TARGET_OS_SIMULATOR
  constexpr bool simulator = true;
#else
  constexpr bool simulator = false;
#endif
  AvailableReferenceFrame result;
  result.choice = input::ios_gyroscope::probeReferenceFrameForAttempt(
      simulator, [&]() {
        result.mask = [CMMotionManager availableAttitudeReferenceFrames];
        return input::ios_gyroscope::ReferenceFrameAvailability{
            .deviceMotionAvailable =
                manager != nil && manager.deviceMotionAvailable,
            .arbitraryCorrectedZVerticalAvailable =
                (result.mask &
                 CMAttitudeReferenceFrameXArbitraryCorrectedZVertical) != 0,
            .magneticNorthZVerticalAvailable =
                (result.mask &
                 CMAttitudeReferenceFrameXMagneticNorthZVertical) != 0};
      });
  return result;
}

CMAttitudeReferenceFrame nativeReferenceFrame(
    input::ios_gyroscope::ReferenceFrameChoice choice) {
  switch (choice) {
  case input::ios_gyroscope::ReferenceFrameChoice::
      ArbitraryCorrectedZVertical:
    return CMAttitudeReferenceFrameXArbitraryCorrectedZVertical;
  case input::ios_gyroscope::ReferenceFrameChoice::MagneticNorthZVertical:
    return CMAttitudeReferenceFrameXMagneticNorthZVertical;
  case input::ios_gyroscope::ReferenceFrameChoice::Unsupported:
    break;
  }
  return static_cast<CMAttitudeReferenceFrame>(0);
}

input::ios_gyroscope::MagneticAccuracy nativeAccuracy(
    CMMagneticFieldCalibrationAccuracy accuracy) {
  switch (accuracy) {
  case CMMagneticFieldCalibrationAccuracyLow:
    return input::ios_gyroscope::MagneticAccuracy::Low;
  case CMMagneticFieldCalibrationAccuracyMedium:
    return input::ios_gyroscope::MagneticAccuracy::Medium;
  case CMMagneticFieldCalibrationAccuracyHigh:
    return input::ios_gyroscope::MagneticAccuracy::High;
  case CMMagneticFieldCalibrationAccuracyUncalibrated:
  default:
    return input::ios_gyroscope::MagneticAccuracy::Uncalibrated;
  }
}

bool isBackgroundEvent(const SDL_Event &event) {
  return event.type == SDL_APP_WILLENTERBACKGROUND ||
         event.type == SDL_APP_DIDENTERBACKGROUND ||
         (event.type == SDL_WINDOWEVENT &&
          (event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
           event.window.event == SDL_WINDOWEVENT_HIDDEN ||
           event.window.event == SDL_WINDOWEVENT_FOCUS_LOST));
}

bool isForegroundEvent(const SDL_Event &event) {
  return event.type == SDL_APP_WILLENTERFOREGROUND ||
         event.type == SDL_APP_DIDENTERFOREGROUND ||
         (event.type == SDL_WINDOWEVENT &&
          (event.window.event == SDL_WINDOWEVENT_RESTORED ||
           event.window.event == SDL_WINDOWEVENT_SHOWN ||
           event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED));
}

class IOSGyroscopeInputBackend final : public IInputBackend {
public:
  explicit IOSGyroscopeInputBackend(input::InputBackendSink sink)
      : IInputBackend(sink), core_(std::move(sink)) {}

  ~IOSGyroscopeInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    if (started_) {
      return true;
    }

    motionManager_ = [[CMMotionManager alloc] init];
    if (motionManager_ != nil) {
      motionManager_.deviceMotionUpdateInterval = 1.0 / 120.0;
      motionManager_.showsDeviceMovementDisplay = YES;
    }
    const bool deviceMotionAvailable =
        motionManager_ != nil && motionManager_.deviceMotionAvailable;
    const bool gyroscopeAvailable =
        motionManager_ != nil && motionManager_.gyroAvailable;
    const bool magnetometerAvailable =
        motionManager_ != nil && motionManager_.magnetometerAvailable;
#if TARGET_OS_SIMULATOR
    constexpr bool simulator = true;
#else
    constexpr bool simulator = false;
#endif
    const bool supported = input::ios_gyroscope::hasRequiredMotionHardware(
        simulator, deviceMotionAvailable, gyroscopeAvailable,
        magnetometerAvailable);
    const auto initialFrame = probeAvailableReferenceFrame(motionManager_);
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "iOS gyroscope capabilities: simulator=%d deviceMotion=%d "
                "gyro=%d magnetometer=%d frames=0x%lx supported=%d",
                simulator, deviceMotionAvailable, gyroscopeAvailable,
                magnetometerAvailable,
                static_cast<unsigned long>(initialFrame.mask), supported);
    started_ = true;
    const std::uint64_t now = nowMicros();
    core_.start(supported, now);
    if (supported) {
      core_.sensorAvailable();
    }
    drainCommands(now);
    return true;
  }

  void stop() override {
    if (!started_) {
      stopNativeSensor();
      motionManager_ = nil;
      return;
    }
    const std::uint64_t now = nowMicros();
    core_.stop(now);
    drainCommands(now);
    stopNativeSensor();
    motionManager_ = nil;
    referenceFrameChoice_ =
        input::ios_gyroscope::ReferenceFrameChoice::Unsupported;
    lastLoggedReferenceFrameMask_.reset();
    lastLoggedActiveReferenceFrame_.reset();
    loggedInactiveStart_ = false;
    started_ = false;
  }

  void handleSdlEvent(const SDL_Event &event) override {
    if (!started_) {
      return;
    }
    const std::uint64_t now = nowMicros();
    if (isBackgroundEvent(event)) {
      core_.setForeground(false, now);
      drainCommands(now);
    } else if (isForegroundEvent(event)) {
      core_.setForeground(true, now);
      drainCommands(now);
    }
  }

  void pump() override {
    if (!started_) {
      return;
    }
    const std::uint64_t now = nowMicros();
    pollLatestMotion(now);
    if (nativeRetryAtMicros_.has_value() &&
        now >= *nativeRetryAtMicros_) {
      startNativeSensor(now);
    }
    core_.pump(now);
    drainCommands(now);
  }

  void configureGyroscopeTurntable(
      input::GyroscopeTurntableConfig config) override {
    core_.configure(config, nowMicros());
  }

  void resetGyroscopeTurntableSession() override {
    core_.resetSession(nowMicros());
  }

private:
  void drainCommands(std::uint64_t now) {
    while (true) {
      switch (core_.takeCommand()) {
      case input::GyroscopeSensorCommand::Start:
        startNativeSensor(now);
        break;
      case input::GyroscopeSensorCommand::Stop:
        stopNativeSensor();
        break;
      case input::GyroscopeSensorCommand::None:
        return;
      }
    }
  }

  void startNativeSensor(std::uint64_t now) {
    stopNativeSensor();
    const auto frame = probeAvailableReferenceFrame(motionManager_);
    referenceFrameChoice_ = frame.choice;
    const unsigned long frameMask = static_cast<unsigned long>(frame.mask);
    if (!lastLoggedReferenceFrameMask_.has_value() ||
        *lastLoggedReferenceFrameMask_ != frameMask) {
      lastLoggedReferenceFrameMask_ = frameMask;
      if (referenceFrameChoice_ ==
          input::ios_gyroscope::ReferenceFrameChoice::Unsupported) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "iOS gyroscope is waiting for a corrected attitude "
                    "reference frame (available=0x%lx).",
                    frameMask);
      } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                    "iOS gyroscope selected corrected attitude reference "
                    "frame with available mask 0x%lx.",
                    frameMask);
      }
    }
    if (motionManager_ == nil ||
        referenceFrameChoice_ ==
            input::ios_gyroscope::ReferenceFrameChoice::Unsupported) {
      nativeRetryAtMicros_ = now + kNativeRetryDelayMicros;
      return;
    }

    [motionManager_ startDeviceMotionUpdatesUsingReferenceFrame:
                        nativeReferenceFrame(referenceFrameChoice_)];
    if (!motionManager_.deviceMotionActive) {
      if (!loggedInactiveStart_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "iOS gyroscope device-motion updates did not become "
                    "active after start.");
        loggedInactiveStart_ = true;
      }
      nativeRetryAtMicros_ = now + kNativeRetryDelayMicros;
      return;
    }
    loggedInactiveStart_ = false;
    const unsigned long activeReferenceFrame =
        static_cast<unsigned long>(motionManager_.attitudeReferenceFrame);
    if (!lastLoggedActiveReferenceFrame_.has_value() ||
        *lastLoggedActiveReferenceFrame_ != activeReferenceFrame) {
      lastLoggedActiveReferenceFrame_ = activeReferenceFrame;
      SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                  "iOS gyroscope device-motion updates are active "
                  "(referenceFrame=0x%lx).",
                  activeReferenceFrame);
    }
    awaitingFirstSample_ = true;
    nativeRetryAtMicros_ = now + kFirstSampleTimeoutMicros;
  }

  void stopNativeSensor() {
    if (motionManager_ != nil && motionManager_.deviceMotionActive) {
      [motionManager_ stopDeviceMotionUpdates];
    }
    nativeRetryAtMicros_.reset();
    awaitingFirstSample_ = false;
    lastPolledTimestampSeconds_.reset();
    accuracyTracker_.reset();
  }

  void pollLatestMotion(std::uint64_t now) {
    if (motionManager_ == nil || !motionManager_.deviceMotionActive) {
      return;
    }
    CMDeviceMotion *motion = motionManager_.deviceMotion;
    if (motion == nil || !std::isfinite(motion.timestamp) ||
        (lastPolledTimestampSeconds_.has_value() &&
         motion.timestamp <= *lastPolledTimestampSeconds_)) {
      return;
    }
    lastPolledTimestampSeconds_ = motion.timestamp;
    if (awaitingFirstSample_) {
      awaitingFirstSample_ = false;
      nativeRetryAtMicros_.reset();
      core_.sensorStartSucceeded(now);
      SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                  "iOS gyroscope received its first device-motion sample.");
    }

    const auto accuracy = accuracyTracker_.observe(
        nativeAccuracy(motion.magneticField.accuracy));
    const CMRotationRate rotationRate = motion.rotationRate;
    const CMAcceleration gravity = motion.gravity;
    core_.observe(
        {.headingDegrees =
             input::ios_gyroscope::headingDegreesFromYawRadians(
                 motion.attitude.yaw),
         .clockwiseRateDegreesPerSecond =
             input::ios_gyroscope::
                 clockwiseWorldVerticalRateDegreesPerSecond(
                     {.x = rotationRate.x,
                      .y = rotationRate.y,
                      .z = rotationRate.z},
                     {.x = gravity.x, .y = gravity.y, .z = gravity.z}),
         .sensorTimestampSeconds = motion.timestamp,
         .accuracyGeneration = accuracy.generation,
         .usableAccuracy = true,
         .discontinuity = false},
        now);
  }

  input::GyroscopeInputBackendCore core_;
  CMMotionManager *__strong motionManager_ = nil;
  input::ios_gyroscope::AccuracyGenerationTracker accuracyTracker_;
  std::optional<double> lastPolledTimestampSeconds_;
  std::optional<std::uint64_t> nativeRetryAtMicros_;
  std::optional<unsigned long> lastLoggedReferenceFrameMask_;
  std::optional<unsigned long> lastLoggedActiveReferenceFrame_;
  input::ios_gyroscope::ReferenceFrameChoice referenceFrameChoice_ =
      input::ios_gyroscope::ReferenceFrameChoice::Unsupported;
  bool loggedInactiveStart_ = false;
  bool awaitingFirstSample_ = false;
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend>
makeIOSGyroscopeInputBackend(input::InputBackendSink sink) {
  return std::make_unique<IOSGyroscopeInputBackend>(std::move(sink));
}

#endif
