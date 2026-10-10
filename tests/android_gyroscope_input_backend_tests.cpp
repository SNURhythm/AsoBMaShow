// Exercise the Android backend itself; only JNI/Activity calls are substituted.
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

// SensorEvent timestamps include suspend time. Keep the native Android clock
// boundary controllable while the rest of the backend uses its real clock.
inline double androidGyroscopeBootSeconds = 1.0;
inline int androidGyroscopeClockGettime(int, timespec *value) {
  value->tv_sec = static_cast<time_t>(androidGyroscopeBootSeconds);
  value->tv_nsec = static_cast<long>((androidGyroscopeBootSeconds - value->tv_sec) * 1e9);
  return 0;
}
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#define clock_gettime androidGyroscopeClockGettime
#define __ANDROID__ 1
#include "input/AndroidGyroscopeInputBackend.cpp"
#undef __ANDROID__
#undef clock_gettime

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct Harness {
  std::vector<input::PhysicalInputEvent> inputs;
  std::vector<input::InputDeviceSnapshot> devices;
  std::vector<std::thread::id> inputThreads;
  std::uint64_t generation = 0;
  bool registrationSucceeds = true;
  bool sampleDuringStop = false;
  std::function<void()> onStop;
  AndroidGyroscopeInputBackend backend{
      {.enqueueInput = [this](auto event) {
         inputs.push_back(std::move(event));
         inputThreads.push_back(std::this_thread::get_id());
       },
       .enqueueDevice = [this](auto device) { devices.push_back(std::move(device)); }}};

  explicit Harness(bool successfulRegistration = true)
      : registrationSucceeds(successfulRegistration) {
    androidGyroscopeJavaCall = [this](std::string_view method) {
      if (method == "startGyroscopeTurntableSensors") {
        backend.acceptRegistrationResult(++generation, registrationSucceeds);
      } else if (method == "stopGyroscopeTurntableSensors" && sampleDuringStop) {
        // Android unregister joins the sensor HandlerThread. A callback may
        // already be in flight; it must finish without waiting for a core lock
        // held across this native-to-Java call.
        std::thread pending([this] { sample(100, 30, 1.4); });
        pending.join();
      }
      if (method == "stopGyroscopeTurntableSensors" && onStop) onStop();
    };
    std::string error;
    require(backend.start(error), "backend starts against a supported sensor");
  }

  ~Harness() { backend.stop(); androidGyroscopeJavaCall = {}; }

  void sample(double heading, double rate, double time,
              std::uint64_t registration = 0, double deliveryAge = 0.0) {
    androidGyroscopeBootSeconds = time + deliveryAge;
    backend.acceptSample(registration == 0 ? generation : registration,
        {.headingDegrees = heading, .clockwiseRateDegreesPerSecond = rate,
         .sensorTimestampSeconds = time, .accuracyGeneration = 1,
         .usableAccuracy = true});
  }

  void clockwise(double time = 1.0) {
    sample(0, 30, time);
    sample(3, 30, time + 0.1);
  }
};

void testEveryDirectionReachesTheSinkBeforePump() {
  Harness h;
  std::thread::id sensorThread;
  std::thread producer([&] {
    sensorThread = std::this_thread::get_id();
    h.clockwise();
    h.sample(3, -30, 1.2);
    h.sample(0, -30, 1.3);
  });
  producer.join();
  require(h.inputs.size() == 2 && h.inputs[0].normalizedValue == 1.0F &&
              h.inputs[1].normalizedValue == -1.0F,
          "clockwise and reverse samples publish immediately without a render pump");
  require(h.inputThreads == std::vector<std::thread::id>{sensorThread, sensorThread},
          "native input publication stays on the sensor producer thread");
  h.backend.pump();
  require(h.inputs.size() == 2, "a render pump cannot replay sensor samples");
}

void testPauseInvalidatesGenerationAndDoesNotJoinUnderCoreLock() {
  Harness h;
  h.clockwise();
  require(h.inputs.size() == 1, "clockwise sample is active before pause");
  const auto oldGeneration = h.generation;
  h.sampleDuringStop = true;
  h.backend.activityPaused();
  require(h.inputs.size() == 2 && h.inputs.back().normalizedValue == 0.0F,
          "pause releases active gyro ownership before rendering");
  h.backend.activityResumed();
  require(h.generation > oldGeneration, "resume starts a fresh registration");
  h.sample(90, 100, 2.0, oldGeneration);
  h.clockwise(2.0);
  require(h.inputs.size() == 3 && h.inputs.back().normalizedValue == 1.0F,
          "late old-generation samples cannot corrupt the new baseline");
  h.backend.stop();
  const auto stoppedCount = h.inputs.size();
  h.clockwise(3.0);
  h.backend.pump();
  require(h.inputs.size() == stoppedCount, "stop rejects late sensor callbacks");
}

void testSensorDeliveryAgeSurvivesDifferentBootAndSteadyEpochs() {
  Harness h;
  h.sample(0, 30, 50000.0);
  const auto before = monotonicMicros();
  h.sample(3, 30, 50000.1, 0, 0.025);
  const auto after = monotonicMicros();
  require(h.inputs.size() == 1 &&
              h.inputs[0].timestampMicros >= before - 25001 &&
              h.inputs[0].timestampMicros <= after - 24999,
          "gyro event retains 25ms delivery age across boottime and steady epochs");
}

void testStationarySamplesReleaseWithoutPumpAndOldSamplesCannotFeedWatchdog() {
  Harness h;
  h.backend.configureGyroscopeTurntable({.releaseDelayMs = 50});
  h.clockwise();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  h.sample(3, 0, 1.101);
  require(h.inputs.size() == 2 && h.inputs.back().normalizedValue == 0.0F,
          "sensor cadence advances the release timer without a render pump");
  h.backend.resetGyroscopeTurntableSession();
  h.sample(0, 30, 2.0);
  h.sample(3, 30, 2.1, 0, 1.5);
  require(!h.devices.empty() && !h.devices.back().connected &&
              h.devices.back().status == input::InputDeviceStatus::Disconnected,
          "a stale sensor sample cannot refresh the watchdog using delivery time");
}

void testFailedRegistrationRetriesAndRejectsPriorGeneration() {
  Harness h(false);
  h.clockwise();
  require(h.inputs.empty(), "failed registration never admits samples");
  h.registrationSucceeds = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(2010));
  h.backend.pump();
  require(h.generation == 2, "retry supervisor issues the next registration");
  h.backend.acceptRegistrationResult(1, true);
  h.clockwise();
  require(h.inputs.size() == 1 && h.inputs.back().normalizedValue == 1.0F,
          "late older registration results cannot replace a successful retry");
}

void testStopWaitsForAcquiredLifecycleCallbackWithoutHoldingCommandLock() {
  Harness h;
  std::mutex mutex;
  std::condition_variable condition;
  bool acquired = false;
  bool continueCallback = false;
  std::thread lifecycle([&] {
    AcquiredAndroidGyroscopeBackend lease;
    require(lease.get() != nullptr, "lifecycle callback acquires backend lease");
    {
      std::unique_lock lock(mutex);
      acquired = true;
      condition.notify_all();
      condition.wait(lock, [&] { return continueCallback; });
    }
    lease.get()->activityPaused();
  });
  {
    std::unique_lock lock(mutex);
    condition.wait(lock, [&] { return acquired; });
  }
  h.onStop = [&] {
    const std::lock_guard lock(mutex);
    continueCallback = true;
    condition.notify_all();
  };
  h.backend.stop();
  lifecycle.join();
  require(!AcquiredAndroidGyroscopeBackend{}.get(), "stop revokes future callback leases");
}
} // namespace

int main() {
  try {
    testEveryDirectionReachesTheSinkBeforePump();
    testPauseInvalidatesGenerationAndDoesNotJoinUnderCoreLock();
    testSensorDeliveryAgeSurvivesDifferentBootAndSteadyEpochs();
    testStationarySamplesReleaseWithoutPumpAndOldSamplesCannotFeedWatchdog();
    testFailedRegistrationRetriesAndRejectsPriorGeneration();
    testStopWaitsForAcquiredLifecycleCallbackWithoutHoldingCommandLock();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
