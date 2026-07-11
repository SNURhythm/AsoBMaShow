#include "GyroscopePlatformBackends.h"

#if defined(__ANDROID__)

#include "GyroscopeInputBackendCore.h"

#include <SDL2/SDL_log.h>
#include <SDL2/SDL_system.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace {

class AndroidGyroscopeInputBackend;

struct AndroidGyroscopeCallbackGate {
  AndroidGyroscopeInputBackend *acquire() {
    const std::lock_guard lock(mutex);
    if (closed || backend == nullptr) {
      return nullptr;
    }
    ++activeCallbacks;
    return backend;
  }

  void release() {
    const std::lock_guard lock(mutex);
    if (activeCallbacks > 0) {
      --activeCallbacks;
    }
    if (activeCallbacks == 0) {
      condition.notify_all();
    }
  }

  void close() {
    const std::lock_guard lock(mutex);
    closed = true;
    backend = nullptr;
  }

  void waitForCallbacks() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return activeCallbacks == 0; });
  }

  std::mutex mutex;
  std::condition_variable condition;
  AndroidGyroscopeInputBackend *backend = nullptr;
  std::size_t activeCallbacks = 0;
  bool closed = false;
};

struct PendingRegistrationResult {
  std::uint64_t generation = 0;
  bool success = false;
};

struct PendingMotionSample {
  std::uint64_t registrationGeneration = 0;
  input::GyroscopeMotionSample sample;
};

std::mutex gBackendMutex;
std::shared_ptr<AndroidGyroscopeCallbackGate> gBackendGate;

std::uint64_t monotonicMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool clearJavaException(JNIEnv *env, std::string &errorMessage,
                        const char *context) {
  if (env == nullptr || !env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  errorMessage = context;
  return true;
}

bool callActivityBoolean(const char *methodName, bool &result,
                         std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is unavailable for gyroscope input.";
    return false;
  }
  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is unavailable for gyroscope input.";
    env->DeleteLocalRef(activity);
    return false;
  }
  jmethodID method = env->GetMethodID(activityClass, methodName, "()Z");
  if (method == nullptr) {
    clearJavaException(env, errorMessage,
                       "Android gyroscope capability method is unavailable.");
    if (errorMessage.empty()) {
      errorMessage = "Android gyroscope capability method is unavailable.";
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }
  result = env->CallBooleanMethod(activity, method) == JNI_TRUE;
  const bool failed = clearJavaException(
      env, errorMessage, "Android gyroscope capability query failed.");
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return !failed;
}

bool callActivityVoid(const char *methodName, std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is unavailable for gyroscope input.";
    return false;
  }
  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is unavailable for gyroscope input.";
    env->DeleteLocalRef(activity);
    return false;
  }
  jmethodID method = env->GetMethodID(activityClass, methodName, "()V");
  if (method == nullptr) {
    clearJavaException(env, errorMessage,
                       "Android gyroscope lifecycle method is unavailable.");
    if (errorMessage.empty()) {
      errorMessage = "Android gyroscope lifecycle method is unavailable.";
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }
  env->CallVoidMethod(activity, method);
  const bool failed = clearJavaException(
      env, errorMessage, "Android gyroscope lifecycle call failed.");
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return !failed;
}

class AndroidGyroscopeInputBackend final : public IInputBackend {
public:
  explicit AndroidGyroscopeInputBackend(input::InputBackendSink sink)
      : IInputBackend(sink), core_(std::move(sink)) {}

  ~AndroidGyroscopeInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    {
      const std::lock_guard lock(coreMutex_);
      if (started_) {
        return true;
      }
    }

    callbackGate_ = std::make_shared<AndroidGyroscopeCallbackGate>();
    callbackGate_->backend = this;
    {
      const std::lock_guard lock(gBackendMutex);
      if (gBackendGate) {
        errorMessage = "Another Android gyroscope backend is active.";
        callbackGate_->close();
        callbackGate_.reset();
        return false;
      }
      gBackendGate = callbackGate_;
    }

    bool supported = false;
    if (!callActivityBoolean("isGyroscopeTurntableSupported", supported,
                             errorMessage)) {
      const auto gate = callbackGate_;
      revokeCallbacks();
      if (gate) {
        gate->waitForCallbacks();
      }
      callbackGate_.reset();
      return false;
    }

    const std::uint64_t nowMicros = monotonicMicros();
    {
      const std::lock_guard lock(coreMutex_);
      started_ = true;
      core_.start(supported, nowMicros);
      processCommandsLocked(nowMicros);
      consumeInboundLocked(nowMicros);
    }
    return true;
  }

  void stop() override {
    std::shared_ptr<AndroidGyroscopeCallbackGate> gate;
    {
      const std::lock_guard lock(coreMutex_);
      if (!started_ && !callbackGate_) {
        return;
      }
      started_ = false;
      gate = callbackGate_;
    }

    revokeCallbacks();
    std::string ignoredError;
    (void)callActivityVoid("stopGyroscopeTurntableSensors", ignoredError);
    if (gate) {
      gate->waitForCallbacks();
    }
    invalidateInbound();

    const std::lock_guard lock(coreMutex_);
    core_.stop(monotonicMicros());
    while (core_.takeCommand() != input::GyroscopeSensorCommand::None) {
    }
    callbackGate_.reset();
  }

  void handleSdlEvent(const SDL_Event &event) override {
    switch (event.type) {
    case SDL_APP_WILLENTERBACKGROUND:
    case SDL_APP_DIDENTERBACKGROUND:
      setForeground(false);
      break;
    case SDL_APP_WILLENTERFOREGROUND:
    case SDL_APP_DIDENTERFOREGROUND:
      setForeground(true);
      break;
    default:
      break;
    }
  }

  void pump() override {
    const std::uint64_t nowMicros = monotonicMicros();
    const std::lock_guard lock(coreMutex_);
    if (!started_) {
      return;
    }
    consumeInboundLocked(nowMicros);
    core_.pump(nowMicros);
    processCommandsLocked(nowMicros);
    consumeInboundLocked(nowMicros);
  }

  void configureGyroscopeTurntable(
      input::GyroscopeTurntableConfig config) override {
    const std::lock_guard lock(coreMutex_);
    if (started_) {
      core_.configure(config, monotonicMicros());
    }
  }

  void resetGyroscopeTurntableSession() override {
    const std::lock_guard lock(coreMutex_);
    if (started_) {
      core_.resetSession(monotonicMicros());
    }
  }

  void acceptRegistrationResult(std::uint64_t generation, bool success) {
    if (generation == 0) {
      return;
    }
    const std::lock_guard lock(inboundMutex_);
    if (generation <= invalidatedGeneration_) {
      return;
    }
    lastSeenGeneration_ = std::max(lastSeenGeneration_, generation);
    if (!pendingRegistration_.has_value() ||
        generation >= pendingRegistration_->generation) {
      pendingRegistration_ = PendingRegistrationResult{
          .generation = generation, .success = success};
    }
    if (success) {
      acceptingGeneration_ = generation;
    } else if (acceptingGeneration_ == generation) {
      acceptingGeneration_ = 0;
      latestSample_.reset();
    }
  }

  void acceptSample(std::uint64_t registrationGeneration,
                    input::GyroscopeMotionSample sample) {
    const std::lock_guard lock(inboundMutex_);
    if (registrationGeneration == 0 ||
        registrationGeneration != acceptingGeneration_ ||
        registrationGeneration <= invalidatedGeneration_) {
      return;
    }
    latestSample_ = PendingMotionSample{
        .registrationGeneration = registrationGeneration,
        .sample = std::move(sample)};
  }

  void activityPaused() { setForeground(false); }
  void activityResumed() { setForeground(true); }

  void activityDestroyed() {
    invalidateInbound();
    const std::uint64_t nowMicros = monotonicMicros();
    const std::lock_guard lock(coreMutex_);
    if (!started_) {
      return;
    }
    core_.setForeground(false, nowMicros);
    processCommandsLocked(nowMicros);
    core_.stop(nowMicros);
    while (core_.takeCommand() != input::GyroscopeSensorCommand::None) {
    }
  }

private:
  void setForeground(bool foreground) {
    if (!foreground) {
      invalidateInbound();
    }
    const std::uint64_t nowMicros = monotonicMicros();
    const std::lock_guard lock(coreMutex_);
    if (!started_) {
      return;
    }
    core_.setForeground(foreground, nowMicros);
    processCommandsLocked(nowMicros);
    consumeInboundLocked(nowMicros);
  }

  void processCommandsLocked(std::uint64_t nowMicros) {
    while (true) {
      const input::GyroscopeSensorCommand command = core_.takeCommand();
      if (command == input::GyroscopeSensorCommand::None) {
        return;
      }
      if (command == input::GyroscopeSensorCommand::Stop) {
        invalidateInbound();
        std::string errorMessage;
        if (!callActivityVoid("stopGyroscopeTurntableSensors", errorMessage)) {
          SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "%s", errorMessage.c_str());
        }
        invalidateInbound();
        continue;
      }

      std::string errorMessage;
      if (!callActivityVoid("startGyroscopeTurntableSensors", errorMessage)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "%s", errorMessage.c_str());
        core_.sensorStartFailed(nowMicros);
        continue;
      }
      consumeRegistrationLocked(nowMicros);
    }
  }

  void consumeInboundLocked(std::uint64_t nowMicros) {
    std::optional<PendingRegistrationResult> registration;
    std::optional<PendingMotionSample> sample;
    {
      const std::lock_guard lock(inboundMutex_);
      registration = std::exchange(pendingRegistration_, std::nullopt);
      sample = std::exchange(latestSample_, std::nullopt);
    }
    if (registration.has_value()) {
      if (registration->success) {
        core_.sensorStartSucceeded(nowMicros);
      } else {
        core_.sensorStartFailed(nowMicros);
      }
    }
    if (sample.has_value()) {
      core_.observe(sample->sample, nowMicros);
    }
  }

  void consumeRegistrationLocked(std::uint64_t nowMicros) {
    std::optional<PendingRegistrationResult> registration;
    {
      const std::lock_guard lock(inboundMutex_);
      registration = std::exchange(pendingRegistration_, std::nullopt);
    }
    if (!registration.has_value()) {
      return;
    }
    if (registration->success) {
      core_.sensorStartSucceeded(nowMicros);
    } else {
      core_.sensorStartFailed(nowMicros);
    }
  }

  void invalidateInbound() {
    const std::lock_guard lock(inboundMutex_);
    invalidatedGeneration_ =
        std::max(invalidatedGeneration_, lastSeenGeneration_);
    acceptingGeneration_ = 0;
    pendingRegistration_.reset();
    latestSample_.reset();
  }

  void revokeCallbacks() {
    if (callbackGate_) {
      callbackGate_->close();
    }
    const std::lock_guard lock(gBackendMutex);
    if (gBackendGate == callbackGate_) {
      gBackendGate.reset();
    }
  }

  input::GyroscopeInputBackendCore core_;
  std::mutex coreMutex_;
  std::mutex inboundMutex_;
  std::optional<PendingRegistrationResult> pendingRegistration_;
  std::optional<PendingMotionSample> latestSample_;
  std::shared_ptr<AndroidGyroscopeCallbackGate> callbackGate_;
  std::uint64_t acceptingGeneration_ = 0;
  std::uint64_t invalidatedGeneration_ = 0;
  std::uint64_t lastSeenGeneration_ = 0;
  bool started_ = false;
};

class AcquiredAndroidGyroscopeBackend {
public:
  AcquiredAndroidGyroscopeBackend() {
    {
      const std::lock_guard lock(gBackendMutex);
      gate_ = gBackendGate;
    }
    if (gate_) {
      backend_ = gate_->acquire();
    }
  }

  ~AcquiredAndroidGyroscopeBackend() {
    if (backend_ != nullptr) {
      gate_->release();
    }
  }

  [[nodiscard]] AndroidGyroscopeInputBackend *get() const { return backend_; }

private:
  std::shared_ptr<AndroidGyroscopeCallbackGate> gate_;
  AndroidGyroscopeInputBackend *backend_ = nullptr;
};

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowGyroscopeTurntableManager_nativeGyroscopeRegistrationResult(
    JNIEnv *, jclass, jlong generation, jboolean success) {
  if (generation <= 0) {
    return;
  }
  AcquiredAndroidGyroscopeBackend acquired;
  if (acquired.get() == nullptr) {
    return;
  }
  acquired.get()->acceptRegistrationResult(
      static_cast<std::uint64_t>(generation), success == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowGyroscopeTurntableManager_nativeGyroscopeSample(
    JNIEnv *, jclass, jlong registrationGeneration, jdouble headingDegrees,
    jdouble clockwiseRateDegreesPerSecond, jdouble sensorTimestampSeconds,
    jlong accuracyGeneration, jboolean usableAccuracy,
    jboolean discontinuity) {
  if (registrationGeneration <= 0 || accuracyGeneration <= 0) {
    return;
  }
  AcquiredAndroidGyroscopeBackend acquired;
  if (acquired.get() == nullptr) {
    return;
  }
  acquired.get()->acceptSample(
      static_cast<std::uint64_t>(registrationGeneration),
      {.headingDegrees = static_cast<double>(headingDegrees),
       .clockwiseRateDegreesPerSecond =
           static_cast<double>(clockwiseRateDegreesPerSecond),
       .sensorTimestampSeconds = static_cast<double>(sensorTimestampSeconds),
       .accuracyGeneration = static_cast<std::uint64_t>(accuracyGeneration),
       .usableAccuracy = usableAccuracy == JNI_TRUE,
       .discontinuity = discontinuity == JNI_TRUE});
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeGyroscopeActivityPaused(
    JNIEnv *, jclass) {
  AcquiredAndroidGyroscopeBackend acquired;
  if (acquired.get() != nullptr) {
    acquired.get()->activityPaused();
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeGyroscopeActivityResumed(
    JNIEnv *, jclass) {
  AcquiredAndroidGyroscopeBackend acquired;
  if (acquired.get() != nullptr) {
    acquired.get()->activityResumed();
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeGyroscopeActivityDestroyed(
    JNIEnv *, jclass) {
  AcquiredAndroidGyroscopeBackend acquired;
  if (acquired.get() != nullptr) {
    acquired.get()->activityDestroyed();
  }
}

std::unique_ptr<IInputBackend>
makeAndroidGyroscopeInputBackend(input::InputBackendSink sink) {
  return std::make_unique<AndroidGyroscopeInputBackend>(std::move(sink));
}

#endif
