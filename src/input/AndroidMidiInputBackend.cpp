#include "MidiPlatformBackends.h"

#if defined(__ANDROID__)

#include "QueuedMidiInputBackend.h"

#include <SDL2/SDL_system.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

class AndroidMidiInputBackend;

struct AndroidMidiCallbackGate {
  AndroidMidiInputBackend *acquire() {
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
  AndroidMidiInputBackend *backend = nullptr;
  std::size_t activeCallbacks = 0;
  bool closed = false;
};

std::mutex gBackendMutex;
std::shared_ptr<AndroidMidiCallbackGate> gBackendGate;

std::uint64_t monotonicMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string jstringToUtf8(JNIEnv *env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const char *characters = env->GetStringUTFChars(value, nullptr);
  if (characters == nullptr) {
    return {};
  }
  std::string result(characters);
  env->ReleaseStringUTFChars(value, characters);
  return result;
}

bool clearJavaException(JNIEnv *env, std::string &errorMessage) {
  if (env == nullptr || !env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  errorMessage = "Android MIDI Java bridge failed.";
  return true;
}

std::string callStartMidiInput(std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is unavailable for MIDI input.";
    return {};
  }
  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is unavailable for MIDI input.";
    env->DeleteLocalRef(activity);
    return {};
  }
  jmethodID method =
      env->GetMethodID(activityClass, "startMidiInput", "()Ljava/lang/String;");
  if (method == nullptr) {
    clearJavaException(env, errorMessage);
    if (errorMessage.empty()) {
      errorMessage = "Android MIDI start method is unavailable.";
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }
  auto result = static_cast<jstring>(env->CallObjectMethod(activity, method));
  if (clearJavaException(env, errorMessage)) {
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }
  std::string status = jstringToUtf8(env, result);
  if (result != nullptr) {
    env->DeleteLocalRef(result);
  }
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return status;
}

void callStopMidiInput() {
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    return;
  }
  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    env->DeleteLocalRef(activity);
    return;
  }
  jmethodID method = env->GetMethodID(activityClass, "stopMidiInput", "()V");
  if (method != nullptr) {
    env->CallVoidMethod(activity, method);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
}

class AndroidMidiInputBackend final : public QueuedMidiInputBackend {
public:
  explicit AndroidMidiInputBackend(input::InputBackendSink sink)
      : QueuedMidiInputBackend(std::move(sink)) {}

  ~AndroidMidiInputBackend() override { stop(); }

  bool start(std::string &errorMessage) override {
    errorMessage.clear();
    if (started_) {
      return true;
    }
    openQueue();
    callbackGate_ = std::make_shared<AndroidMidiCallbackGate>();
    callbackGate_->backend = this;
    {
      const std::lock_guard lock(gBackendMutex);
      if (gBackendGate) {
        errorMessage = "Another Android MIDI input backend is active.";
        callbackGate_->close();
        callbackGate_.reset();
        closeQueue();
        return false;
      }
      gBackendGate = callbackGate_;
    }

    const std::string status = callStartMidiInput(errorMessage);
    if (!errorMessage.empty() || status != "ok") {
      if (errorMessage.empty()) {
        errorMessage =
            status.empty() ? "Android MIDI service is unavailable." : status;
      }
      callStopMidiInput();
      revokeCallbacks();
      closeQueue();
      return false;
    }
    started_ = true;
    return true;
  }

  void stop() override {
    if (!started_ && !callbackGate_) {
      closeQueue();
      return;
    }
    started_ = false;
    revokeCallbacks();
    callStopMidiInput();
    if (callbackGate_) {
      callbackGate_->waitForCallbacks();
    }
    callbackGate_.reset();
    closeQueue();
  }

  void acceptDevice(std::string stableId, std::string displayName,
                    bool connected) {
    if (stableId.empty()) {
      return;
    }
    enqueueDevice({.stableId = std::move(stableId),
                   .displayName = std::move(displayName),
                   .deviceClass = input::DeviceClass::Midi,
                   .connected = connected});
  }

  void acceptPacket(std::string stableId, std::vector<std::uint8_t> bytes,
                    std::uint64_t timestampMicros) {
    enqueuePacket(std::move(stableId), std::move(bytes), timestampMicros);
  }

private:
  void revokeCallbacks() {
    if (callbackGate_) {
      callbackGate_->close();
    }
    const std::lock_guard lock(gBackendMutex);
    if (gBackendGate == callbackGate_) {
      gBackendGate.reset();
    }
  }

  std::shared_ptr<AndroidMidiCallbackGate> callbackGate_;
  bool started_ = false;
};

class AcquiredAndroidBackend {
public:
  AcquiredAndroidBackend() {
    {
      const std::lock_guard lock(gBackendMutex);
      gate_ = gBackendGate;
    }
    if (gate_) {
      backend_ = gate_->acquire();
    }
  }

  ~AcquiredAndroidBackend() {
    if (backend_ != nullptr) {
      gate_->release();
    }
  }

  [[nodiscard]] AndroidMidiInputBackend *get() const { return backend_; }

private:
  std::shared_ptr<AndroidMidiCallbackGate> gate_;
  AndroidMidiInputBackend *backend_ = nullptr;
};

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowMidiManager_nativeMidiDevice(
    JNIEnv *env, jclass, jstring stableId, jstring displayName,
    jboolean connected) {
  AcquiredAndroidBackend acquired;
  if (acquired.get() == nullptr) {
    return;
  }
  acquired.get()->acceptDevice(jstringToUtf8(env, stableId),
                               jstringToUtf8(env, displayName),
                               connected == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowMidiManager_nativeMidiPacket(
    JNIEnv *env, jclass, jstring stableId, jbyteArray data, jint offset,
    jint count, jlong timestampNanos) {
  constexpr jint kMaximumJavaPacketBytes = 64 * 1024;
  if (env == nullptr || data == nullptr || offset < 0 || count <= 0 ||
      count > kMaximumJavaPacketBytes) {
    return;
  }
  const jsize dataLength = env->GetArrayLength(data);
  if (offset > dataLength || count > dataLength - offset) {
    return;
  }
  AcquiredAndroidBackend acquired;
  if (acquired.get() == nullptr) {
    return;
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
  env->GetByteArrayRegion(data, offset, count,
                          reinterpret_cast<jbyte *>(bytes.data()));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return;
  }
  const std::uint64_t timestampMicros =
      timestampNanos > 0 ? static_cast<std::uint64_t>(timestampNanos) / 1000ULL
                         : monotonicMicros();
  acquired.get()->acceptPacket(jstringToUtf8(env, stableId), std::move(bytes),
                               timestampMicros);
}

std::unique_ptr<IInputBackend>
makeAndroidMidiInputBackend(input::InputBackendSink sink) {
  return std::make_unique<AndroidMidiInputBackend>(std::move(sink));
}

#endif
