#include "GyroscopePlatformBackends.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "GyroscopeInputBackendCore.h"
#include "IOSGyroscopeMotionAdapter.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_log.h>
#include <SDL2/SDL_sensor.h>
#include <mach/mach_time.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

constexpr std::uint64_t kFirstSampleTimeoutMicros = 10000000;
constexpr char kCorrectedMotionSensorName[] = "Corrected Device Motion";

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

int findCorrectedMotionSensor() {
  const int sensorCount = SDL_NumSensors();
  for (int index = 0; index < sensorCount; ++index) {
    const char *name = SDL_SensorGetDeviceName(index);
    if (SDL_SensorGetDeviceType(index) == SDL_SENSOR_UNKNOWN &&
        name != nullptr && std::strcmp(name, kCorrectedMotionSensorName) == 0) {
      return index;
    }
  }
  return -1;
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

    const bool sensorSubsystemReady =
        (SDL_WasInit(SDL_INIT_SENSOR) & SDL_INIT_SENSOR) != 0;
    if (sensorSubsystemReady) {
      sensorIndex_ = findCorrectedMotionSensor();
    }
    const bool supported = sensorSubsystemReady && sensorIndex_ >= 0;
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                "iOS SDL corrected motion sensor: subsystem=%d index=%d "
                "supported=%d",
                sensorSubsystemReady, sensorIndex_, supported);

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
      return;
    }
    const std::uint64_t now = nowMicros();
    core_.stop(now);
    drainCommands(now);
    stopNativeSensor();
    sensorIndex_ = -1;
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
    if (sensorIndex_ < 0) {
      core_.sensorStartFailed(now);
      return;
    }

    correctedMotionSensor_ = SDL_SensorOpen(sensorIndex_);
    if (correctedMotionSensor_ == nullptr) {
      SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                  "iOS SDL corrected motion sensor could not open: %s",
                  SDL_GetError());
      core_.sensorStartFailed(now);
      return;
    }

    publishStartupDiagnostic("waiting for SDL corrected motion sample");
    awaitingFirstSample_ = true;
    nativeRetryAtMicros_ = now + kFirstSampleTimeoutMicros;
  }

  void stopNativeSensor() {
    if (correctedMotionSensor_ != nullptr) {
      SDL_SensorClose(correctedMotionSensor_);
      correctedMotionSensor_ = nullptr;
    }
    nativeRetryAtMicros_.reset();
    lastTimestampMicros_.reset();
    awaitingFirstSample_ = false;
  }

  void pollLatestMotion(std::uint64_t now) {
    if (correctedMotionSensor_ == nullptr) {
      return;
    }

    std::array<float, 8> data{};
    Uint64 timestampMicros = 0;
    if (SDL_SensorGetDataWithTimestamp(
            correctedMotionSensor_, &timestampMicros, data.data(),
            static_cast<int>(data.size())) != 0 ||
        timestampMicros == 0 ||
        (lastTimestampMicros_.has_value() &&
         timestampMicros <= *lastTimestampMicros_)) {
      return;
    }
    lastTimestampMicros_ = timestampMicros;

    if (awaitingFirstSample_) {
      awaitingFirstSample_ = false;
      nativeRetryAtMicros_.reset();
      core_.sensorStartSucceeded(now);
      SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                  "iOS gyroscope received its first SDL corrected motion "
                  "sample.");
    }

    core_.observe(
        {.headingDegrees =
             input::ios_gyroscope::headingDegreesFromYawRadians(data[0]),
         .clockwiseRateDegreesPerSecond =
             input::ios_gyroscope::
                 clockwiseWorldVerticalRateDegreesPerSecond(
                     {.x = data[1], .y = data[2], .z = data[3]},
                     {.x = data[4], .y = data[5], .z = data[6]}),
         .sensorTimestampSeconds =
             static_cast<double>(timestampMicros) / 1000000.0,
         .accuracyGeneration = 0,
         .usableAccuracy = true,
         .discontinuity = false},
        now);
  }

  input::GyroscopeInputBackendCore core_;
  SDL_Sensor *correctedMotionSensor_ = nullptr;
  std::optional<Uint64> lastTimestampMicros_;
  std::optional<std::uint64_t> nativeRetryAtMicros_;
  int sensorIndex_ = -1;
  bool awaitingFirstSample_ = false;
  bool started_ = false;
};

} // namespace

std::unique_ptr<IInputBackend>
makeIOSGyroscopeInputBackend(input::InputBackendSink sink) {
  return std::make_unique<IOSGyroscopeInputBackend>(std::move(sink));
}

#endif
