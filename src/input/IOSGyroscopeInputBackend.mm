#include "GyroscopePlatformBackends.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "GyroscopeInputBackendCore.h"
#include "IOSGyroscopeMotionAdapter.h"
#include "NativeCallbackLifetime.h"

#include <CoreMotion/CoreMotion.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_log.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kFirstSampleTimeoutMicros = 10000000;
constexpr NSTimeInterval kMotionUpdateIntervalSeconds = 1.0 / 120.0;

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

std::uint64_t sampleMicros(NSTimeInterval timestamp,
                           std::uint64_t fallback) {
  if (!std::isfinite(timestamp) || timestamp <= 0.0) {
    return fallback;
  }
  const long double micros = static_cast<long double>(timestamp) * 1000000.0L;
  if (micros >=
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(micros);
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

input::ios_gyroscope::MagneticAccuracy
magneticAccuracy(CMMagneticFieldCalibrationAccuracy accuracy) {
  using input::ios_gyroscope::MagneticAccuracy;
  switch (accuracy) {
  case CMMagneticFieldCalibrationAccuracyLow:
    return MagneticAccuracy::Low;
  case CMMagneticFieldCalibrationAccuracyMedium:
    return MagneticAccuracy::Medium;
  case CMMagneticFieldCalibrationAccuracyHigh:
    return MagneticAccuracy::High;
  case CMMagneticFieldCalibrationAccuracyUncalibrated:
  default:
    return MagneticAccuracy::Uncalibrated;
  }
}

input::ios_gyroscope::ReferenceFrameChoice
probeReferenceFrame(CMMotionManager *manager) {
  return input::ios_gyroscope::probeReferenceFrameForAttempt(
      TARGET_OS_SIMULATOR != 0, [manager] {
        const CMAttitudeReferenceFrame frames =
            [CMMotionManager availableAttitudeReferenceFrames];
        return input::ios_gyroscope::ReferenceFrameAvailability{
            .deviceMotionAvailable = manager.deviceMotionAvailable,
            .arbitraryCorrectedZVerticalAvailable =
                (frames &
                 CMAttitudeReferenceFrameXArbitraryCorrectedZVertical) != 0,
            .magneticNorthZVerticalAvailable =
                (frames & CMAttitudeReferenceFrameXMagneticNorthZVertical) !=
                0,
        };
      });
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
    return static_cast<CMAttitudeReferenceFrame>(0);
  }
  return static_cast<CMAttitudeReferenceFrame>(0);
}

class IOSGyroscopeInputBackend final : public IInputBackend {
public:
  explicit IOSGyroscopeInputBackend(input::InputBackendSink sink)
      : IInputBackend(sink), core_(std::move(sink)) {}

  ~IOSGyroscopeInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    {
      const std::lock_guard lock(coreMutex_);
      if (started_) {
        return true;
      }
    }

    motionManager_ = [[CMMotionManager alloc] init];
    motionQueue_ = [[NSOperationQueue alloc] init];
    motionQueue_.maxConcurrentOperationCount = 1;
    motionQueue_.qualityOfService = NSQualityOfServiceUserInteractive;
    motionQueue_.name = @"AsoBMaShow realtime gyroscope";
    callbackLifetime_ = std::make_unique<NativeCallbackLifetime>(this);

    const bool supported = input::ios_gyroscope::hasRequiredMotionHardware(
        TARGET_OS_SIMULATOR != 0, motionManager_.deviceMotionAvailable,
        motionManager_.gyroAvailable, motionManager_.magnetometerAvailable);
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "iOS asynchronous corrected motion: hardware=%d frame=%d",
                supported, static_cast<int>(probeReferenceFrame(motionManager_)));

    const std::uint64_t now = nowMicros();
    {
      const std::lock_guard lock(coreMutex_);
      started_ = true;
      core_.start(supported, now);
      if (supported) {
        core_.sensorAvailable();
      }
    }
    drainCommands(now);
    return true;
  }

  void stop() override {
    const std::uint64_t now = nowMicros();
    {
      const std::lock_guard lock(coreMutex_);
      if (started_) {
        core_.stop(now);
        started_ = false;
      }
    }
    drainCommands(now);
    stopNativeSensor();
    if (callbackLifetime_) {
      callbackLifetime_->closeAndWait();
      callbackLifetime_.reset();
    }
    [motionQueue_ cancelAllOperations];
    motionQueue_ = nil;
    motionManager_ = nil;
  }

  void handleSdlEvent(const SDL_Event &event) override {
    const std::uint64_t now = nowMicros();
    bool changed = false;
    {
      const std::lock_guard lock(coreMutex_);
      if (!started_) {
        return;
      }
      if (isBackgroundEvent(event)) {
        core_.setForeground(false, now);
        changed = true;
      } else if (isForegroundEvent(event)) {
        core_.setForeground(true, now);
        changed = true;
      }
    }
    if (changed) {
      drainCommands(now);
    }
  }

  void pump() override {
    const std::uint64_t now = nowMicros();
    bool restartForMissingFirstSample = false;
    {
      const std::lock_guard lock(coreMutex_);
      if (!started_) {
        return;
      }
      if (nativeRetryAtMicros_.has_value() &&
          now >= *nativeRetryAtMicros_) {
        nativeRetryAtMicros_.reset();
        restartForMissingFirstSample = true;
      }
      core_.pump(now);
    }
    if (restartForMissingFirstSample) {
      startNativeSensor(now);
    }
    drainCommands(now);
  }

  void configureGyroscopeTurntable(
      input::GyroscopeTurntableConfig config) override {
    const std::lock_guard lock(coreMutex_);
    core_.configure(config, nowMicros());
  }

  void resetGyroscopeTurntableSession() override {
    const std::lock_guard lock(coreMutex_);
    core_.resetSession(nowMicros());
  }

private:
  void publishStartupDiagnostic(std::string detail) const {
    publishDevice({
        .stableId = std::string(input::kGyroscopeTurntableStableId),
        .displayName = std::string(input::kGyroscopeTurntableDisplayName) +
                       " · " + std::move(detail),
        .deviceClass = input::DeviceClass::Gyroscope,
        .connected = true,
        .status = input::InputDeviceStatus::Calibrating,
        .buttons = 0,
        .axes = 1,
        .hats = 0,
    });
  }

  void drainCommands(std::uint64_t now) {
    while (true) {
      input::GyroscopeSensorCommand command;
      {
        const std::lock_guard lock(coreMutex_);
        command = core_.takeCommand();
      }
      switch (command) {
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
    if (motionManager_ == nil || motionQueue_ == nil ||
        callbackLifetime_ == nullptr) {
      const std::lock_guard lock(coreMutex_);
      core_.sensorStartFailed(now);
      return;
    }

    const auto choice = probeReferenceFrame(motionManager_);
    const CMAttitudeReferenceFrame referenceFrame =
        nativeReferenceFrame(choice);
    if (referenceFrame == 0) {
      const std::lock_guard lock(coreMutex_);
      core_.sensorStartFailed(now);
      return;
    }

    const std::uint64_t generation =
        nativeGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
      const std::lock_guard lock(coreMutex_);
      accuracyTracker_.reset();
      awaitingFirstSample_ = true;
      nativeRetryAtMicros_ = now + kFirstSampleTimeoutMicros;
    }
    publishStartupDiagnostic("waiting for asynchronous corrected motion");

    motionManager_.deviceMotionUpdateInterval = kMotionUpdateIntervalSeconds;
    motionManager_.showsDeviceMovementDisplay = YES;
    void *callbackToken = callbackLifetime_->token();
    [motionManager_
        startDeviceMotionUpdatesUsingReferenceFrame:referenceFrame
                                            toQueue:motionQueue_
                                         withHandler:^(CMDeviceMotion *motion,
                                                       NSError *error) {
                                           auto lease =
                                               NativeCallbackLifetime::acquire(
                                                   callbackToken);
                                           if (auto *backend = lease.ownerAs<
                                                   IOSGyroscopeInputBackend>()) {
                                             backend->acceptMotion(
                                                 motion, error, generation);
                                           }
                                         }];
  }

  void stopNativeSensor() {
    nativeGeneration_.fetch_add(1, std::memory_order_acq_rel);
    [motionManager_ stopDeviceMotionUpdates];
    const std::lock_guard lock(coreMutex_);
    accuracyTracker_.reset();
    nativeRetryAtMicros_.reset();
    awaitingFirstSample_ = false;
  }

  void acceptMotion(CMDeviceMotion *motion, NSError *error,
                    std::uint64_t generation) {
    if (generation != nativeGeneration_.load(std::memory_order_acquire)) {
      return;
    }
    if (error != nil || motion == nil) {
      if (error != nil) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "iOS asynchronous corrected motion error: %s",
                    error.localizedDescription.UTF8String);
      }
      const std::uint64_t timestampMicros = nowMicros();
      {
        const std::lock_guard lock(coreMutex_);
        if (!started_ || generation !=
                             nativeGeneration_.load(std::memory_order_acquire)) {
          return;
        }
        awaitingFirstSample_ = false;
        nativeRetryAtMicros_.reset();
        core_.sensorRuntimeFailed(timestampMicros);
      }
      stopNativeSensor();
      drainCommands(timestampMicros);
      return;
    }

    const std::uint64_t timestampMicros =
        sampleMicros(motion.timestamp, nowMicros());
    const CMRotationRate rate = motion.rotationRate;
    const CMAcceleration gravity = motion.gravity;
    bool firstSample = false;
    {
      const std::lock_guard lock(coreMutex_);
      if (!started_ || generation !=
                           nativeGeneration_.load(std::memory_order_acquire)) {
        return;
      }
      const auto accuracy = accuracyTracker_.observe(
          magneticAccuracy(motion.magneticField.accuracy));
      if (awaitingFirstSample_) {
        awaitingFirstSample_ = false;
        nativeRetryAtMicros_.reset();
        core_.sensorStartSucceeded(timestampMicros);
        firstSample = true;
      }
      core_.observe(
          {.headingDegrees =
               input::ios_gyroscope::headingDegreesFromYawRadians(
                   motion.attitude.yaw),
           .clockwiseRateDegreesPerSecond =
               input::ios_gyroscope::
                   clockwiseWorldVerticalRateDegreesPerSecond(
                       {.x = rate.x, .y = rate.y, .z = rate.z},
                       {.x = gravity.x, .y = gravity.y, .z = gravity.z}),
           .sensorTimestampSeconds = motion.timestamp,
           .accuracyGeneration = accuracy.generation,
           .usableAccuracy = accuracy.usable,
           .discontinuity = false},
          timestampMicros);
      // Advancing on every native sample makes both press and delayed release
      // independent of the render loop.
      core_.pump(timestampMicros);
    }
    if (firstSample) {
      SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                  "iOS gyroscope received its first asynchronous corrected "
                  "motion sample.");
    }
  }

  input::GyroscopeInputBackendCore core_;
  std::mutex coreMutex_;
  input::ios_gyroscope::AccuracyGenerationTracker accuracyTracker_;
  CMMotionManager *motionManager_ = nil;
  NSOperationQueue *motionQueue_ = nil;
  std::unique_ptr<NativeCallbackLifetime> callbackLifetime_;
  std::atomic_uint64_t nativeGeneration_{0};
  std::optional<std::uint64_t> nativeRetryAtMicros_;
  bool awaitingFirstSample_ = false;
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend>
makeIOSGyroscopeInputBackend(input::InputBackendSink sink) {
  return std::make_unique<IOSGyroscopeInputBackend>(std::move(sink));
}

#endif
