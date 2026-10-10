#include "GyroscopePlatformBackends.h"

#if defined(__ANDROID__)

#include "GyroscopeInputBackendCore.h"
#include "InputLifecycle.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <time.h>

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

std::mutex gBackendMutex;
std::shared_ptr<AndroidGyroscopeCallbackGate> gBackendGate;

std::uint64_t monotonicMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t sensorSampleMicros(double sensorSeconds,
                                std::uint64_t receiptMicros) {
  // SensorEvent.timestamp uses elapsed realtime (CLOCK_BOOTTIME, including
  // suspend); gameplay uses CLOCK_MONOTONIC. Rebase age on every delivery so
  // a device sleep cannot become a permanent judgement offset.
  timespec bootNow{};
  if (!std::isfinite(sensorSeconds) || sensorSeconds < 0.0 ||
      clock_gettime(CLOCK_BOOTTIME, &bootNow) != 0) return receiptMicros;
  const long double ageMicros =
      static_cast<long double>(bootNow.tv_sec) * 1'000'000.0L +
      static_cast<long double>(bootNow.tv_nsec) / 1000.0L -
      static_cast<long double>(sensorSeconds) * 1'000'000.0L;
  if (ageMicros <= 0.0L) return receiptMicros;
  if (ageMicros >= receiptMicros) return 1;
  return receiptMicros - static_cast<std::uint64_t>(ageMicros);
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
    }
    processCommands(nowMicros);
    return true;
  }

  void stop() override {
    std::unique_lock commandLock(commandMutex_);
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
    // An already-acquired activity lifecycle callback can still need the
    // command mutex. Let it observe started_=false before joining gate leases.
    commandLock.unlock();
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
    if (input::isBackgroundLifecycleEvent(event)) {
      setForeground(false);
    } else if (input::isForegroundLifecycleEvent(event)) {
      setForeground(true);
    }
  }

  void pump() override {
    const std::uint64_t nowMicros = monotonicMicros();
    {
      const std::lock_guard lock(coreMutex_);
      if (!started_) return;
      consumeRegistrationLocked(nowMicros);
      core_.pump(nowMicros);
    }
    processCommands(nowMicros);
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
    if (generation <= invalidatedGeneration_ || generation < lastSeenGeneration_) {
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
    }
  }

  void acceptSample(std::uint64_t registrationGeneration,
                    input::GyroscopeMotionSample sample) {
    const std::uint64_t nowMicros = monotonicMicros();
    const auto sampleMicros = sensorSampleMicros(sample.sensorTimestampSeconds, nowMicros);
    const std::lock_guard lock(coreMutex_);
    if (!started_) return;
    {
      const std::lock_guard inboundLock(inboundMutex_);
      if (registrationGeneration == 0 ||
          registrationGeneration != acceptingGeneration_ ||
          registrationGeneration <= invalidatedGeneration_) return;
    }
    consumeRegistrationLocked(nowMicros);
    // The dedicated Android sensor thread owns delivery cadence. Preserve
    // every accepted direction change instead of replacing it until a frame.
    core_.observe(sample, sampleMicros);
    core_.pump(monotonicMicros());
  }

  void activityPaused() { setForeground(false); }
  void activityResumed() { setForeground(true); }

  void activityDestroyed() {
    const std::uint64_t nowMicros = monotonicMicros();
    {
      const std::lock_guard lock(coreMutex_);
      invalidateInbound();
      if (!started_) return;
      core_.setForeground(false, nowMicros);
      core_.stop(nowMicros);
    }
    processCommands(nowMicros);
  }

private:
  void setForeground(bool foreground) {
    const std::uint64_t nowMicros = monotonicMicros();
    {
      const std::lock_guard lock(coreMutex_);
      if (!foreground) invalidateInbound();
      if (!started_) return;
      core_.setForeground(foreground, nowMicros);
    }
    processCommands(nowMicros);
  }

  void processCommands(std::uint64_t nowMicros) {
    // Java unregister joins its sensor HandlerThread. Never hold coreMutex_
    // across JNI: an in-flight sample needs that mutex before it can finish.
    const std::lock_guard commandLock(commandMutex_);
    while (true) {
      input::GyroscopeSensorCommand command;
      {
        const std::lock_guard lock(coreMutex_);
        if (!started_) return;
        command = core_.takeCommand();
        if (command == input::GyroscopeSensorCommand::Stop) invalidateInbound();
      }
      if (command == input::GyroscopeSensorCommand::None) return;

      std::string errorMessage;
      if (command == input::GyroscopeSensorCommand::Stop) {
        if (!callActivityVoid("stopGyroscopeTurntableSensors", errorMessage)) {
          SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "%s", errorMessage.c_str());
        }
        const std::lock_guard lock(coreMutex_);
        invalidateInbound();
        continue;
      }

      const bool started = callActivityVoid("startGyroscopeTurntableSensors", errorMessage);
      const std::lock_guard lock(coreMutex_);
      if (!started) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "%s", errorMessage.c_str());
        core_.sensorStartFailed(nowMicros);
      }
      consumeRegistrationLocked(nowMicros);
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
  std::mutex commandMutex_;
  std::mutex inboundMutex_;
  std::optional<PendingRegistrationResult> pendingRegistration_;
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
