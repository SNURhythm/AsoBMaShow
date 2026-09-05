#include "audio/AudioWrapper.h"
#include "audio/Jukebox.h"
#include "audio/JukeboxLifecycle.h"
#include "audio/JukeboxSoundResources.h"
#include "audio/decoder.h"
#include "skin/beatoraja/LuaSkinApplicationAudioBackend.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

bool decodeAudioToPCM(const path_t &, std::vector<short> &, SF_INFO &,
                      std::atomic<bool> &) {
  return false;
}

bool decodeAudioToPCMBounded(const path_t &, std::vector<short> &, SF_INFO &,
                             std::atomic<bool> &, AudioDecodeLimits,
                             std::stop_token) {
  return false;
}

bool decodeAudioBytesToPCM(const path_t &, const std::vector<unsigned char> &,
                           std::vector<short> &, SF_INFO &,
                           std::atomic<bool> &) {
  return false;
}

bool decodeAudioBytesToPCMBounded(const path_t &,
                                  const std::vector<unsigned char> &,
                                  std::vector<short> &, SF_INFO &,
                                  std::atomic<bool> &, std::size_t) {
  return false;
}

bool decodeSkinSoundBundleAware(const path_t &, std::vector<short> &, SF_INFO &,
                                std::atomic<bool> &, AudioDecodeLimits,
                                std::stop_token) {
  return false;
}

namespace {

using namespace std::chrono_literals;

using LoadChartSignature = audio::playback::BackendOperationResult (Jukebox::*)(
    bms_parser::Chart &, bool, std::atomic_bool &);
using ReloadChartSignature = audio::playback::BackendOperationResult (
    Jukebox::*)(bms_parser::Chart &, bool, std::atomic_bool &);
using PlaySignature =
    audio::playback::BackendOperationResult (Jukebox::*)(long long);
using StopSignature = audio::playback::BackendOperationResult (Jukebox::*)();
using SeekSignature =
    audio::playback::BackendOperationResult (Jukebox::*)(long long);

static_assert(
    std::same_as<decltype(static_cast<LoadChartSignature>(&Jukebox::loadChart)),
                 LoadChartSignature>);
static_assert(std::same_as<decltype(static_cast<ReloadChartSignature>(
                               &Jukebox::reloadChartResources)),
                           ReloadChartSignature>);
static_assert(std::same_as<decltype(static_cast<PlaySignature>(&Jukebox::play)),
                           PlaySignature>);
static_assert(std::same_as<decltype(static_cast<StopSignature>(&Jukebox::stop)),
                           StopSignature>);
static_assert(std::same_as<decltype(static_cast<SeekSignature>(&Jukebox::seek)),
                           SeekSignature>);

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

struct BackendControl {
  std::mutex mutex;
  std::condition_variable condition;
  audio::playback::BackendRunState state =
      audio::playback::BackendRunState::Stopped;
  bool blockStop = false;
  bool stopEntered = false;
  bool releaseStop = false;
  bool blockStart = false;
  bool startEntered = false;
  bool releaseStart = false;
  bool blockObserve = false;
  bool observeEntered = false;
  bool releaseObserve = false;
  bool failStop = false;
  int failStopCall = -1;
  int failObserveCall = -1;
  bool failStart = false;
  bool destroyed = false;
  std::atomic<int> observeCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> startCalls{0};
};

class GatedBackend final : public audio::playback::IBackendLifecycle {
public:
  explicit GatedBackend(std::shared_ptr<BackendControl> control)
      : control(std::move(control)) {}

  ~GatedBackend() override {
    {
      std::lock_guard lock(control->mutex);
      control->destroyed = true;
    }
    control->condition.notify_all();
  }

  audio::playback::BackendStateObservation observeState() const override {
    const int observeCall =
        control->observeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    std::unique_lock lock(control->mutex);
    control->observeEntered = true;
    control->condition.notify_all();
    control->condition.wait(lock, [this] {
      return !control->blockObserve || control->releaseObserve;
    });
    if (control->failObserveCall == observeCall) {
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "gated observation failure"};
    }
    return {.state = control->state};
  }

  int outputSampleRate() const override { return 44100; }

  audio::playback::BackendOperationResult stopAndDrain() override {
    const int stopCall =
        control->stopCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    std::unique_lock lock(control->mutex);
    control->stopEntered = true;
    control->condition.notify_all();
    control->condition.wait(
        lock, [this] { return !control->blockStop || control->releaseStop; });
    if (control->failStop || control->failStopCall == stopCall) {
      return {.success = false, .diagnostic = "gated stop failure"};
    }
    control->state = audio::playback::BackendRunState::Stopped;
    return {.success = true};
  }

  audio::playback::BackendOperationResult start() override {
    control->startCalls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(control->mutex);
    control->startEntered = true;
    control->condition.notify_all();
    control->condition.wait(
        lock, [this] { return !control->blockStart || control->releaseStart; });
    if (control->failStart) {
      return {.success = false, .diagnostic = "gated start failure"};
    }
    control->state = audio::playback::BackendRunState::Running;
    return {.success = true};
  }

private:
  std::shared_ptr<BackendControl> control;
};

struct FactoryControl {
  audio::Capabilities capabilities{
      .canSelectOutputDevice = true,
      .canSelectSampleRate = true,
      .canSelectBufferFrames = true,
      .outputDevices = {{.id = "default",
                         .name = "Default",
                         .isDefault = true,
                         .sampleRates = {44100, 48000},
                         .bufferFrames = {0, 128}},
                        {.id = "usb",
                         .name = "USB",
                         .sampleRates = {48000},
                         .bufferFrames = {0, 64}}},
  };
  std::vector<audio::StreamRequest> opens;
  std::vector<std::string> events;
  std::deque<bool> openResults{true};
  std::deque<bool> startResults{true};
  std::deque<bool> stopResults{true};
  bool rejectConcurrentStreams = false;
  int liveStreams = 0;
  std::optional<audio::playback::BackendRunState> authoritativeState;
  audio::RenderCallback renderCallback = nullptr;
  void *renderUserData = nullptr;
};

class FakeConfigurableStream final : public audio::IBackend {
public:
  FakeConfigurableStream(std::shared_ptr<FactoryControl> control,
                         audio::StreamRequest request)
      : control_(std::move(control)) {
    ++control_->liveStreams;
    state_.request = std::move(request);
    state_.effectiveSampleRate =
        state_.request.sampleRate == 0 ? 44100 : state_.request.sampleRate;
    state_.effectiveBufferFrames = state_.request.bufferFrames;
  }

  ~FakeConfigurableStream() override { --control_->liveStreams; }

  bool start(std::string &errorMessage) override {
    control_->events.push_back("start:" + state_.request.deviceId);
    if (!pop(control_->startResults, true)) {
      errorMessage = "fake configurable start failed";
      return false;
    }
    started_ = true;
    if (control_->authoritativeState.has_value()) {
      control_->authoritativeState = audio::playback::BackendRunState::Running;
    }
    return true;
  }

  bool stop(std::string &) override {
    control_->events.push_back("stop:" + state_.request.deviceId);
    if (!pop(control_->stopResults, true)) {
      return false;
    }
    started_ = false;
    if (control_->authoritativeState.has_value()) {
      control_->authoritativeState = audio::playback::BackendRunState::Stopped;
    }
    return true;
  }

  [[nodiscard]] bool isStarted() const override { return started_; }
  [[nodiscard]] audio::playback::BackendStateObservation
  observeState() const override {
    return {.state = control_->authoritativeState.value_or(
                started_ ? audio::playback::BackendRunState::Running
                         : audio::playback::BackendRunState::Stopped)};
  }
  [[nodiscard]] audio::RuntimeState runtimeState() const override {
    return state_;
  }

private:
  static bool pop(std::deque<bool> &values, bool fallback) {
    if (values.empty()) {
      return fallback;
    }
    const bool value = values.front();
    values.pop_front();
    return value;
  }

  std::shared_ptr<FactoryControl> control_;
  audio::RuntimeState state_;
  bool started_ = false;
};

class FakeConfigurableFactory final : public audio::IBackendFactory {
public:
  explicit FakeConfigurableFactory(std::shared_ptr<FactoryControl> control)
      : control_(std::move(control)) {}

  [[nodiscard]] audio::Capabilities capabilities() const override {
    return control_->capabilities;
  }

  std::unique_ptr<audio::IBackend> open(const audio::StreamRequest &request,
                                        audio::RenderCallback renderCallback,
                                        void *renderUserData,
                                        std::string &errorMessage) override {
    control_->renderCallback = renderCallback;
    control_->renderUserData = renderUserData;
    control_->opens.push_back(request);
    if (control_->rejectConcurrentStreams && control_->liveStreams != 0) {
      errorMessage = "fake factory permits only one live stream";
      return nullptr;
    }
    if (!pop(control_->openResults, true)) {
      errorMessage = "fake configurable open failed";
      return nullptr;
    }
    return std::make_unique<FakeConfigurableStream>(control_, request);
  }

private:
  static bool pop(std::deque<bool> &values, bool fallback) {
    if (values.empty()) {
      return fallback;
    }
    const bool value = values.front();
    values.pop_front();
    return value;
  }

  std::shared_ptr<FactoryControl> control_;
};

struct WrapperFixture {
  Stopwatch stopwatch;
  std::shared_ptr<BackendControl> control = std::make_shared<BackendControl>();
  std::unique_ptr<AudioWrapper> wrapper;

  WrapperFixture()
      : wrapper(std::make_unique<AudioWrapper>(
            &stopwatch, std::make_unique<GatedBackend>(control))) {
    require(control->state == audio::playback::BackendRunState::Running,
            "the injected backend is started by the production wrapper");
    control->startCalls.store(0, std::memory_order_relaxed);
    control->observeCalls.store(0, std::memory_order_relaxed);
    control->startEntered = false;
  }

  void load(const path_t &path) {
    require(wrapper->loadGeneratedSound(path, {100, 200, 300, 400}, 1, 44100),
            "generated test PCM is retained by AudioWrapper");
  }

  void waitForStopEntry() {
    std::unique_lock lock(control->mutex);
    require(control->condition.wait_for(
                lock, 2s, [this] { return control->stopEntered; }),
            "the gated backend receives the stop request");
  }

  void waitForStartEntry() {
    std::unique_lock lock(control->mutex);
    require(control->condition.wait_for(
                lock, 2s, [this] { return control->startEntered; }),
            "the gated backend receives the start request");
  }

  void releaseStop() {
    {
      std::lock_guard lock(control->mutex);
      control->releaseStop = true;
    }
    control->condition.notify_all();
  }

  void releaseStart() {
    {
      std::lock_guard lock(control->mutex);
      control->releaseStart = true;
    }
    control->condition.notify_all();
  }
};

void testUnloadOneSerializesDrainClearAndEraseAgainstPlay() {
  WrapperFixture fixture;
  const path_t soundPath = PATH("atomic-unload-one");
  const path_t retainedPath = PATH("atomic-unload-one-retained");
  fixture.load(soundPath);
  fixture.load(retainedPath);
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->blockStop = true;
  }

  auto unload = std::async(std::launch::async, [&] {
    return fixture.wrapper->unloadSound(soundPath);
  });
  fixture.waitForStopEntry();

  std::promise<void> playAtCallBoundary;
  auto play = std::async(std::launch::async, [&] {
    playAtCallBoundary.set_value();
    return fixture.wrapper->playSound(soundPath, audio::Bus::Bgm);
  });
  playAtCallBoundary.get_future().wait();
  require(play.wait_for(50ms) == std::future_status::timeout,
          "play is held behind the in-flight unload transaction");

  fixture.releaseStop();
  require(unload.wait_for(2s) == std::future_status::ready,
          "unload-one completes without a lock-order deadlock");
  require(play.wait_for(2s) == std::future_status::ready,
          "the blocked producer resumes after deletion");
  require(unload.get().success,
          "a confirmed stop permits the single sound to be erased");
  require(!play.get(),
          "a concurrent producer cannot republish the erased SoundData");
  require(!fixture.wrapper->getSoundDurationMicros(soundPath).has_value(),
          "the selected sound is absent after the atomic transaction");
  require(fixture.wrapper->getSoundDurationMicros(retainedPath).has_value(),
          "unload-one preserves unrelated retained SoundData ownership");
}

void testUnloadAllSerializesDrainClearAndEraseAgainstPlay() {
  WrapperFixture fixture;
  const path_t firstPath = PATH("atomic-unload-all-first");
  const path_t secondPath = PATH("atomic-unload-all-second");
  fixture.load(firstPath);
  fixture.load(secondPath);
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->blockStop = true;
  }

  auto unload = std::async(std::launch::async,
                           [&] { return fixture.wrapper->unloadSounds(); });
  fixture.waitForStopEntry();

  std::promise<void> playAtCallBoundary;
  auto play = std::async(std::launch::async, [&] {
    playAtCallBoundary.set_value();
    return fixture.wrapper->playSound(secondPath, audio::Bus::Keysound);
  });
  playAtCallBoundary.get_future().wait();
  require(play.wait_for(50ms) == std::future_status::timeout,
          "play is held behind unload-all while callback storage is owned");

  fixture.releaseStop();
  require(unload.wait_for(2s) == std::future_status::ready &&
              play.wait_for(2s) == std::future_status::ready,
          "unload-all and its blocked producer finish without deadlock");
  require(unload.get().success && !play.get(),
          "unload-all erases before a producer can reacquire storage");
  require(!fixture.wrapper->getSoundDurationMicros(firstPath).has_value() &&
              !fixture.wrapper->getSoundDurationMicros(secondPath).has_value(),
          "the entire sound store is erased only after positive drain");
}

void testBatchPruneNoOpDoesNotObserveOrDrainBackend() {
  WrapperFixture fixture;
  const path_t retainedPath = PATH("batch-no-op-retained");
  fixture.load(retainedPath);
  const int observationsBefore = fixture.control->observeCalls.load();
  const int drainsBefore = fixture.control->stopCalls.load();

  const auto empty = fixture.wrapper->pruneSounds({});
  const auto missing =
      fixture.wrapper->pruneSounds({PATH("batch-no-op-missing")});

  require(empty.success && missing.success,
          "empty and absent batch-prune requests are successful no-ops");
  require(fixture.control->observeCalls.load() == observationsBefore &&
              fixture.control->stopCalls.load() == drainsBefore,
          "no-op batch prune does not observe or drain the backend");
  require(fixture.wrapper->getSoundDurationMicros(retainedPath).has_value(),
          "no-op batch prune preserves unrelated storage");
}

void testBatchPruneUsesOneConfirmationForEveryCandidate() {
  WrapperFixture fixture;
  const path_t firstPath = PATH("batch-prune-first");
  const path_t secondPath = PATH("batch-prune-second");
  const path_t retainedPath = PATH("batch-prune-retained");
  fixture.load(firstPath);
  fixture.load(secondPath);
  fixture.load(retainedPath);
  const int drainsBefore = fixture.control->stopCalls.load();
  const int observationsBefore = fixture.control->observeCalls.load();
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->failObserveCall = observationsBefore + 3;
  }

  const auto result =
      fixture.wrapper->pruneSounds({firstPath, secondPath, firstPath});

  require(result.success &&
              fixture.control->stopCalls.load() == drainsBefore + 1 &&
              fixture.control->observeCalls.load() == observationsBefore + 2,
          "one confirmation commits every candidate without reaching a "
          "configured second-candidate observation failure");
  require(
      !fixture.wrapper->getSoundDurationMicros(firstPath).has_value() &&
          !fixture.wrapper->getSoundDurationMicros(secondPath).has_value() &&
          fixture.wrapper->getSoundDurationMicros(retainedPath).has_value(),
      "batch prune erases all candidates together and retains other owners");
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->failObserveCall = -1;
  }
}

void testBatchPruneFailureKeepsOldMapAndPartiallyLoadedNewOwner() {
  WrapperFixture fixture;
  const path_t oldFirst = PATH("batch-map-old-first");
  const path_t oldSecond = PATH("batch-map-old-second");
  const path_t newOwner = PATH("batch-map-new-owner");
  fixture.load(oldFirst);
  fixture.load(oldSecond);
  fixture.load(newOwner);
  std::unordered_map<int, path_t> currentMap{{1, oldFirst}, {2, oldSecond}};
  const std::unordered_map<int, path_t> nextMap{{3, newOwner}};
  const int drainsBefore = fixture.control->stopCalls.load();
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->failStopCall = drainsBefore + 1;
  }

  auto result = jukebox_sound_resources::PruneAndCommitSoundMap(
      *fixture.wrapper, currentMap, nextMap, {oldFirst, oldSecond});

  require(!result.success &&
              currentMap == std::unordered_map<int, path_t>{{1, oldFirst},
                                                            {2, oldSecond}},
          "failed batch confirmation leaves the advertised old map intact");
  require(
      fixture.wrapper->getSoundDurationMicros(oldFirst).has_value() &&
          fixture.wrapper->getSoundDurationMicros(oldSecond).has_value() &&
          fixture.wrapper->getSoundDurationMicros(newOwner).has_value(),
      "failure preserves old owners, callback safety, and a partial new load");

  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->failStopCall = -1;
  }
  result = jukebox_sound_resources::PruneAndCommitSoundMap(
      *fixture.wrapper, currentMap, nextMap, {oldFirst, oldSecond});
  require(result.success && currentMap == nextMap,
          "successful batch prune publishes the corresponding map once");
  require(!fixture.wrapper->getSoundDurationMicros(oldFirst).has_value() &&
              !fixture.wrapper->getSoundDurationMicros(oldSecond).has_value() &&
              fixture.wrapper->getSoundDurationMicros(newOwner).has_value(),
          "successful map commit matches the all-at-once owner set");
}

void testBatchPruneSerializesProducerAgainstWholeErase() {
  WrapperFixture fixture;
  const path_t firstPath = PATH("batch-producer-first");
  const path_t secondPath = PATH("batch-producer-second");
  fixture.load(firstPath);
  fixture.load(secondPath);
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->blockStop = true;
  }

  auto prune = std::async(std::launch::async, [&] {
    return fixture.wrapper->pruneSounds({firstPath, secondPath});
  });
  fixture.waitForStopEntry();
  auto producer = std::async(std::launch::async, [&] {
    return fixture.wrapper->playSound(secondPath, audio::Bus::Keysound);
  });
  require(producer.wait_for(50ms) == std::future_status::timeout,
          "producer remains behind the complete batch-prune transaction");

  fixture.releaseStop();
  require(prune.wait_for(2s) == std::future_status::ready &&
              producer.wait_for(2s) == std::future_status::ready,
          "batch prune and blocked producer finish without deadlock");
  require(prune.get().success && !producer.get(),
          "producer cannot reacquire any owner erased by the committed batch");
}

void testConcurrentStartThenUnloadUsesOneLockOrder() {
  WrapperFixture fixture;
  const path_t soundPath = PATH("concurrent-start-unload");
  fixture.load(soundPath);
  require(fixture.wrapper->stopSounds().success,
          "the fixture can enter a positively stopped state");
  {
    std::lock_guard lock(fixture.control->mutex);
    fixture.control->blockStart = true;
    fixture.control->releaseStart = false;
    fixture.control->startEntered = false;
    fixture.control->stopEntered = false;
  }

  auto play = std::async(std::launch::async, [&] {
    return fixture.wrapper->playSound(soundPath, audio::Bus::Bgm);
  });
  fixture.waitForStartEntry();
  auto unload = std::async(std::launch::async, [&] {
    return fixture.wrapper->unloadSound(soundPath);
  });
  require(unload.wait_for(50ms) == std::future_status::timeout,
          "unload waits behind a start transaction using the same lock order");

  fixture.releaseStart();
  require(play.wait_for(2s) == std::future_status::ready &&
              unload.wait_for(2s) == std::future_status::ready,
          "concurrent start/play and unload complete without deadlock");
  require(play.get() && unload.get().success,
          "the established producer completes before the later unload");
  require(!fixture.wrapper->getSoundDurationMicros(soundPath).has_value(),
          "the later unload drains that producer before erasing its storage");
}

void testFailedStopRetainsSingleAndAllStorage() {
  {
    WrapperFixture fixture;
    const path_t soundPath = PATH("retain-one-on-stop-failure");
    fixture.load(soundPath);
    {
      std::lock_guard lock(fixture.control->mutex);
      fixture.control->failStop = true;
    }
    const auto result = fixture.wrapper->unloadSound(soundPath);
    require(!result.success && result.diagnostic.find("gated stop failure") !=
                                   std::string::npos,
            "unload-one surfaces the backend stop diagnostic");
    require(fixture.wrapper->getSoundDurationMicros(soundPath).has_value(),
            "unload-one retains storage when callback quiescence is unknown");
  }

  {
    WrapperFixture fixture;
    const path_t firstPath = PATH("retain-all-first-on-stop-failure");
    const path_t secondPath = PATH("retain-all-second-on-stop-failure");
    fixture.load(firstPath);
    fixture.load(secondPath);
    {
      std::lock_guard lock(fixture.control->mutex);
      fixture.control->failStop = true;
    }
    const auto result = fixture.wrapper->unloadSounds();
    require(!result.success, "unload-all reports an unconfirmed backend drain");
    require(fixture.wrapper->getSoundDurationMicros(firstPath).has_value() &&
                fixture.wrapper->getSoundDurationMicros(secondPath).has_value(),
            "unload-all retains every owner after stop failure");
  }
}

void testDestructorReleasesBackendBeforeDependentStorageOnStopFailure() {
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  {
    auto wrapper = std::make_unique<AudioWrapper>(
        &stopwatch, std::make_unique<GatedBackend>(control));
    require(wrapper->loadGeneratedSound(PATH("destructor-lifetime"), {100, 200},
                                        1, 44100),
            "destructor fixture retains PCM");
    std::lock_guard lock(control->mutex);
    control->failStop = true;
  }
  std::lock_guard lock(control->mutex);
  require(control->destroyed,
          "failed shutdown destroys the callback-owning backend synchronously");
}

class FakeJukeboxLifecycle {
public:
  audio::playback::BackendOperationResult stopResult{.success = true};
  audio::playback::BackendOperationResult startResult{.success = true};
  std::vector<std::string> events;

  audio::playback::BackendOperationResult stopSounds() {
    events.emplace_back("stop");
    return stopResult;
  }

  audio::playback::BackendOperationResult startDevice() {
    events.emplace_back("start");
    return startResult;
  }
};

void testJukeboxLifecycleActionsRunOnlyAfterConfirmation() {
  FakeJukeboxLifecycle lifecycle;
  lifecycle.stopResult = {.success = false,
                          .diagnostic = "device would not drain"};
  bool stoppedMutationRan = false;
  auto stopped =
      jukebox_lifecycle::RunAfterConfirmedStop(lifecycle, "Jukebox::seek", [&] {
        stoppedMutationRan = true;
        lifecycle.events.emplace_back("mutate-after-stop");
      });
  require(!stopped.success && !stoppedMutationRan &&
              stopped.diagnostic.find("Jukebox::seek") != std::string::npos &&
              stopped.diagnostic.find("device would not drain") !=
                  std::string::npos,
          "a failed Jukebox stop preserves state and surfaces context");

  lifecycle = FakeJukeboxLifecycle{};
  bool confirmedStopMutationRan = false;
  stopped =
      jukebox_lifecycle::RunAfterConfirmedStop(lifecycle, "Jukebox::stop", [&] {
        confirmedStopMutationRan = true;
        lifecycle.events.emplace_back("mutate-after-stop");
      });
  require(stopped.success && confirmedStopMutationRan &&
              lifecycle.events ==
                  std::vector<std::string>{"stop", "mutate-after-stop"},
          "Jukebox stop-side mutations follow positive drain confirmation");

  lifecycle = FakeJukeboxLifecycle{};
  lifecycle.startResult = {.success = false,
                           .diagnostic = "device would not start"};
  bool startedMutationRan = false;
  const auto started = jukebox_lifecycle::RunAfterConfirmedStart(
      lifecycle, "Jukebox::play", [&] {
        startedMutationRan = true;
        lifecycle.events.emplace_back("claim-playing");
      });
  require(!started.success && !startedMutationRan &&
              lifecycle.events == std::vector<std::string>{"start"} &&
              started.diagnostic.find("Jukebox::play") != std::string::npos &&
              started.diagnostic.find("device would not start") !=
                  std::string::npos,
          "a failed Jukebox start cannot advance clock or claim playback");

  lifecycle = FakeJukeboxLifecycle{};
  bool confirmedStartMutationRan = false;
  const auto confirmedStart = jukebox_lifecycle::RunAfterConfirmedStart(
      lifecycle, "Jukebox::play", [&] {
        confirmedStartMutationRan = true;
        lifecycle.events.emplace_back("claim-playing");
      });
  require(
      confirmedStart.success && confirmedStartMutationRan &&
          lifecycle.events ==
              std::vector<std::string>{"start", "claim-playing"},
      "Jukebox clocks and playing state advance only after confirmed start");
}

struct JukeboxStateFixture {
  std::atomic_bool isPlaying{true};
  std::atomic_bool schedulerActive{true};
  Stopwatch stopwatch;
  std::mutex transitionMutex;
  std::mutex positionMutex;
  size_t audioCursor = 7;
  size_t bmpCursor = 8;
  size_t bmpLayerCursor = 9;
  std::atomic<int> currentBga{41};
  std::atomic<int> currentBmpLayer{42};
  int schedulerWakes = 0;

  JukeboxStateFixture() {
    stopwatch.seek(123456);
    stopwatch.pause();
  }

  jukebox_lifecycle::SessionState state() {
    return {.isPlaying = isPlaying,
            .schedulerActive = schedulerActive,
            .stopwatch = stopwatch,
            .transitionMutex = transitionMutex,
            .positionMutex = positionMutex,
            .audioCursor = audioCursor,
            .bmpCursor = bmpCursor,
            .bmpLayerCursor = bmpLayerCursor,
            .currentBga = currentBga,
            .currentBmpLayer = currentBmpLayer};
  }

  auto wakeScheduler() {
    return [this] { ++schedulerWakes; };
  }
};

class GatedJukeboxLifecycle {
public:
  audio::playback::BackendOperationResult stopResult{.success = true};
  audio::playback::BackendOperationResult startResult{.success = true};

  audio::playback::BackendOperationResult stopSounds() {
    std::unique_lock lock(mutex);
    stopEntered = true;
    condition.notify_all();
    condition.wait(lock, [this] { return !blockStop || releaseStop; });
    return stopResult;
  }

  audio::playback::BackendOperationResult startDevice() {
    std::unique_lock lock(mutex);
    startEntered = true;
    condition.notify_all();
    condition.wait(lock, [this] { return !blockStart || releaseStart; });
    return startResult;
  }

  void gateStop() {
    std::lock_guard lock(mutex);
    blockStop = true;
    releaseStop = false;
  }

  void gateStart() {
    std::lock_guard lock(mutex);
    blockStart = true;
    releaseStart = false;
  }

  void waitForStop() {
    std::unique_lock lock(mutex);
    require(condition.wait_for(lock, 2s, [this] { return stopEntered; }),
            "seek reaches the gated stop while owning its transition");
  }

  void waitForStart() {
    std::unique_lock lock(mutex);
    require(condition.wait_for(lock, 2s, [this] { return startEntered; }),
            "seek reaches the gated restart before target publication");
  }

  void allowStop() {
    {
      std::lock_guard lock(mutex);
      releaseStop = true;
    }
    condition.notify_all();
  }

  void allowStart() {
    {
      std::lock_guard lock(mutex);
      releaseStart = true;
    }
    condition.notify_all();
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  bool blockStop = false;
  bool releaseStop = false;
  bool stopEntered = false;
  bool blockStart = false;
  bool releaseStart = false;
  bool startEntered = false;
};

void testPlayingSeekAtomicallyExcludesStaleSchedulerAndReaders() {
  JukeboxStateFixture fixture;
  fixture.stopwatch.start();
  auto state = fixture.state();
  GatedJukeboxLifecycle lifecycle;
  lifecycle.gateStop();
  lifecycle.gateStart();

  std::mutex schedulerGateMutex;
  std::condition_variable schedulerGateCondition;
  bool schedulerPassedRunningCheck = false;
  bool allowSchedulerPositionLock = false;
  bool schedulerAdvanced = false;
  size_t schedulerObservedCursor = 0;
  auto scheduler = std::async(std::launch::async, [&] {
    require(fixture.isPlaying && fixture.stopwatch.isRunning(),
            "scheduler passes its outer running check before seek begins");
    {
      std::unique_lock gateLock(schedulerGateMutex);
      schedulerPassedRunningCheck = true;
      schedulerGateCondition.notify_all();
      schedulerGateCondition.wait(gateLock,
                                  [&] { return allowSchedulerPositionLock; });
    }
    std::lock_guard positionLock(fixture.positionMutex);
    if (jukebox_lifecycle::CanAdvanceSchedulerLocked(state)) {
      schedulerAdvanced = true;
      schedulerObservedCursor = fixture.audioCursor;
    }
  });
  {
    std::unique_lock gateLock(schedulerGateMutex);
    require(schedulerGateCondition.wait_for(
                gateLock, 2s, [&] { return schedulerPassedRunningCheck; }),
            "gated scheduler reaches the pre-lock race window");
  }

  const jukebox_lifecycle::CursorPosition target{
      .audio = 40, .bmp = 50, .bmpLayer = 60};
  std::atomic<long long> publishedAudioMicros{111111};
  bool commitRan = false;
  auto seek = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 654321,
        [&](bool wasPlaying) {
          require(wasPlaying,
                  "running seek identifies its audio restart commit");
          require(!fixture.isPlaying && !fixture.stopwatch.isRunning(),
                  "seek commits clock, cursors, and audio while unpublished");
          publishedAudioMicros.store(777777, std::memory_order_release);
          commitRan = true;
        },
        fixture.wakeScheduler());
  });
  lifecycle.waitForStop();
  require(!fixture.isPlaying && !fixture.stopwatch.isRunning(),
          "playing seek publishes a conservative transition before drain");

  {
    std::lock_guard gateLock(schedulerGateMutex);
    allowSchedulerPositionLock = true;
  }
  schedulerGateCondition.notify_all();

  std::atomic<int> liveTimeReads{0};
  auto readTime = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::ReadPublishedTime(state, [&] {
      liveTimeReads.fetch_add(1, std::memory_order_relaxed);
      return publishedAudioMicros.load(std::memory_order_acquire);
    });
  });
  std::atomic<int> keysoundsPlayed{0};
  auto playKey = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::PlayKeySoundIfPublished(state, [&] {
      keysoundsPlayed.fetch_add(1, std::memory_order_relaxed);
    });
  });
  require(scheduler.wait_for(50ms) == std::future_status::timeout &&
              readTime.wait_for(50ms) == std::future_status::timeout &&
              playKey.wait_for(50ms) == std::future_status::timeout,
          "stale scheduler and readers remain behind the full seek transition");

  lifecycle.allowStop();
  lifecycle.waitForStart();
  require(!commitRan && !fixture.isPlaying && fixture.audioCursor == 7 &&
              publishedAudioMicros.load() == 111111,
          "restart confirmation precedes every target-state publication");
  lifecycle.allowStart();

  require(seek.wait_for(2s) == std::future_status::ready &&
              scheduler.wait_for(2s) == std::future_status::ready &&
              readTime.wait_for(2s) == std::future_status::ready &&
              playKey.wait_for(2s) == std::future_status::ready,
          "seek and all blocked production readers finish without deadlock");
  const auto result = seek.get();
  require(result.success && commitRan && fixture.isPlaying &&
              fixture.stopwatch.isRunning() && fixture.audioCursor == 40 &&
              fixture.bmpCursor == 50 && fixture.bmpLayerCursor == 60,
          "successful seek publishes running only after target commit");
  scheduler.get();
  require(schedulerAdvanced && schedulerObservedCursor == 40,
          "stale scheduler can only advance the newly committed position");
  require(readTime.get() == 777777 && liveTimeReads.load() == 1,
          "time reader observes the committed audio clock, never the old one");
  require(playKey.get() && keysoundsPlayed.load() == 1,
          "keysound request resumes only after successful publication");
}

void testPlayingSeekFailureLeavesSchedulerAndReadersStopped() {
  JukeboxStateFixture fixture;
  fixture.stopwatch.start();
  auto state = fixture.state();
  GatedJukeboxLifecycle lifecycle;
  lifecycle.gateStop();
  lifecycle.startResult = {.success = false,
                           .diagnostic = "restart stayed unavailable"};

  const jukebox_lifecycle::CursorPosition target{
      .audio = 40, .bmp = 50, .bmpLayer = 60};
  bool commitRan = false;
  auto seek = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 654321,
        [&](bool wasPlaying) {
          require(wasPlaying,
                  "failed running seek retains its pre-transition identity");
          commitRan = true;
        },
        fixture.wakeScheduler());
  });
  lifecycle.waitForStop();

  std::atomic<int> liveTimeReads{0};
  auto readTime = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::ReadPublishedTime(state, [&] {
      liveTimeReads.fetch_add(1, std::memory_order_relaxed);
      return 999999LL;
    });
  });
  std::atomic<int> keysoundsPlayed{0};
  auto playKey = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::PlayKeySoundIfPublished(
        state, [&] { keysoundsPlayed.fetch_add(1); });
  });
  lifecycle.allowStop();

  require(seek.wait_for(2s) == std::future_status::ready &&
              readTime.wait_for(2s) == std::future_status::ready &&
              playKey.wait_for(2s) == std::future_status::ready,
          "failed restart releases blocked readers in conservative state");
  const auto result = seek.get();
  require(!result.success &&
              result.diagnostic.find("restart stayed unavailable") !=
                  std::string::npos &&
              !commitRan && !fixture.isPlaying && !fixture.schedulerActive &&
              !fixture.stopwatch.isRunning() && fixture.audioCursor == 7 &&
              fixture.bmpCursor == 8 && fixture.bmpLayerCursor == 9,
          "restart failure publishes neither old advancement nor new target");
  require(readTime.get() != 999999 && liveTimeReads.load() == 0,
          "failed seek reader uses only the frozen logical clock");
  require(!playKey.get() && keysoundsPlayed.load() == 0,
          "failed seek cannot queue a keysound after transition");
}

void testSeekTransitionSerializesConcurrentPlayAndStopEntry() {
  JukeboxStateFixture fixture;
  fixture.stopwatch.start();
  auto state = fixture.state();
  GatedJukeboxLifecycle lifecycle;
  lifecycle.gateStop();
  const jukebox_lifecycle::CursorPosition target{
      .audio = 12, .bmp = 13, .bmpLayer = 14};

  auto seek = std::async(std::launch::async, [&] {
    return jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 246810,
        [](bool wasPlaying) {
          require(wasPlaying, "serialized seek began from playing state");
        },
        fixture.wakeScheduler());
  });
  lifecycle.waitForStop();

  std::atomic<int> serializedEntries{0};
  auto playEntry = std::async(std::launch::async, [&] {
    std::lock_guard transitionLock(fixture.transitionMutex);
    serializedEntries.fetch_add(1, std::memory_order_relaxed);
  });
  auto stopEntry = std::async(std::launch::async, [&] {
    std::lock_guard transitionLock(fixture.transitionMutex);
    serializedEntries.fetch_add(1, std::memory_order_relaxed);
  });
  require(playEntry.wait_for(50ms) == std::future_status::timeout &&
              stopEntry.wait_for(50ms) == std::future_status::timeout,
          "concurrent play and stop stay behind the complete seek transaction");

  lifecycle.allowStop();
  require(seek.wait_for(2s) == std::future_status::ready &&
              playEntry.wait_for(2s) == std::future_status::ready &&
              stopEntry.wait_for(2s) == std::future_status::ready,
          "serialized seek, play, and stop entries finish without deadlock");
  require(seek.get().success && serializedEntries.load() == 2,
          "play and stop enter only after seek publishes its final state");
  playEntry.get();
  stopEntry.get();
}

void testJukeboxProductionStateTransitionsFailClosed() {
  {
    JukeboxStateFixture fixture;
    FakeJukeboxLifecycle lifecycle;
    lifecycle.stopResult = {.success = false,
                            .diagnostic = "load drain failure"};
    auto state = fixture.state();
    const auto result = jukebox_lifecycle::StopSessionForTransition(
        lifecycle, "Jukebox::loadChart", state, fixture.wakeScheduler());
    require(!result.success && fixture.isPlaying &&
                fixture.stopwatch.elapsedMicros() == 123456 &&
                fixture.audioCursor == 7 && fixture.bmpCursor == 8 &&
                fixture.bmpLayerCursor == 9 && fixture.schedulerWakes == 0,
            "load failure preserves playing, clock, cursors, and scheduler");
  }

  {
    JukeboxStateFixture fixture;
    FakeJukeboxLifecycle lifecycle;
    lifecycle.stopResult = {.success = false,
                            .diagnostic = "stop drain failure"};
    auto state = fixture.state();
    auto result = jukebox_lifecycle::StopPlayback(
        lifecycle, "Jukebox::stop", state, fixture.wakeScheduler());
    require(!result.success && fixture.isPlaying &&
                fixture.currentBga.load() == 41 &&
                fixture.currentBmpLayer.load() == 42 &&
                fixture.schedulerWakes == 0,
            "stop failure cannot clear visual or playing state");

    lifecycle.stopResult = {.success = true};
    result = jukebox_lifecycle::StopPlayback(lifecycle, "Jukebox::stop", state,
                                             fixture.wakeScheduler());
    require(
        result.success && !fixture.isPlaying &&
            !fixture.stopwatch.isRunning() && fixture.currentBga.load() == -1 &&
            fixture.currentBmpLayer.load() == -1 && fixture.schedulerWakes == 1,
        "confirmed stop commits conservative session and visual state");
  }

  {
    JukeboxStateFixture fixture;
    fixture.isPlaying = false;
    FakeJukeboxLifecycle lifecycle;
    lifecycle.startResult = {.success = false,
                             .diagnostic = "play start failure"};
    bool committedAudio = false;
    auto state = fixture.state();
    const jukebox_lifecycle::CursorPosition target{
        .audio = 1, .bmp = 2, .bmpLayer = 3};
    auto result = jukebox_lifecycle::StartPlayback(
        lifecycle, "Jukebox::play", state, target,
        [&] { committedAudio = true; }, fixture.wakeScheduler());
    require(!result.success && !committedAudio && !fixture.isPlaying &&
                fixture.stopwatch.elapsedMicros() == 123456 &&
                fixture.audioCursor == 7 && fixture.bmpCursor == 8 &&
                fixture.bmpLayerCursor == 9 && fixture.schedulerWakes == 0,
            "play start failure cannot move clocks, cursors, or playing state");

    lifecycle.startResult = {.success = true};
    result = jukebox_lifecycle::StartPlayback(
        lifecycle, "Jukebox::play", state, target,
        [&] {
          require(fixture.audioCursor == 1 && fixture.bmpCursor == 2 &&
                      fixture.bmpLayerCursor == 3 && !fixture.isPlaying &&
                      !fixture.stopwatch.isRunning(),
                  "play commits prepared audio before claiming playback");
          committedAudio = true;
        },
        fixture.wakeScheduler());
    require(result.success && committedAudio && fixture.isPlaying &&
                fixture.stopwatch.isRunning() && fixture.schedulerWakes == 1,
            "confirmed play start publishes cursors, clock, and playing state");
  }

  {
    JukeboxStateFixture fixture;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    lifecycle.stopResult = {.success = false,
                            .diagnostic = "seek drain failure"};
    bool committedSeek = false;
    const jukebox_lifecycle::CursorPosition target{
        .audio = 4, .bmp = 5, .bmpLayer = 6};
    const auto result = jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 654321,
        [&](bool wasPlaying) {
          require(wasPlaying, "drain failure began from a playing session");
          committedSeek = true;
        },
        fixture.wakeScheduler());
    require(!result.success && !committedSeek && !fixture.isPlaying &&
                !fixture.schedulerActive && !fixture.stopwatch.isRunning() &&
                fixture.stopwatch.elapsedMicros() == 123456 &&
                fixture.audioCursor == 7 && fixture.bmpCursor == 8 &&
                fixture.bmpLayerCursor == 9 && fixture.schedulerWakes == 2,
            "seek drain failure keeps old position in a conservative stopped "
            "session");
  }

  {
    JukeboxStateFixture fixture;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    const jukebox_lifecycle::CursorPosition target{
        .audio = 4, .bmp = 5, .bmpLayer = 6};
    bool committedSeek = false;
    const auto result = jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 654321,
        [&](bool wasPlaying) {
          require(wasPlaying,
                  "paused playing seek still restarts audio ownership");
          require(fixture.audioCursor == 4 && fixture.bmpCursor == 5 &&
                      fixture.bmpLayerCursor == 6 &&
                      fixture.stopwatch.elapsedMicros() == 654321 &&
                      !fixture.isPlaying && !fixture.stopwatch.isRunning(),
                  "seek target and audio commit remain unpublished");
          committedSeek = true;
        },
        fixture.wakeScheduler());
    require(
        result.success && committedSeek && fixture.isPlaying &&
            fixture.schedulerActive && !fixture.stopwatch.isRunning() &&
            fixture.schedulerWakes == 2,
        "confirmed paused seek commits target state without resuming clock");
  }

  {
    JukeboxStateFixture fixture;
    fixture.isPlaying = false;
    fixture.schedulerActive = false;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    const jukebox_lifecycle::CursorPosition target{
        .audio = 10, .bmp = 11, .bmpLayer = 12};
    bool committedSeek = false;
    const auto result = jukebox_lifecycle::ExecuteSeekTransition(
        lifecycle, "Jukebox::seek", state, target, 777777,
        [&](bool wasPlaying) {
          require(!wasPlaying, "stopped seek does not enqueue playback audio");
          committedSeek = true;
        },
        fixture.wakeScheduler());
    require(
        result.success && committedSeek && !fixture.isPlaying &&
            !fixture.schedulerActive && !fixture.stopwatch.isRunning() &&
            fixture.stopwatch.elapsedMicros() == 777777 &&
            fixture.audioCursor == 10 && fixture.bmpCursor == 11 &&
            fixture.bmpLayerCursor == 12 && lifecycle.events.empty() &&
            fixture.schedulerWakes == 1,
        "stopped seek commits position without starting or claiming playback");
  }
}

void testPlaybackSnapshotClockModesRestoreWithoutPrematureEligibility() {
  const jukebox_lifecycle::CursorPosition target{
      .audio = 21, .bmp = 22, .bmpLayer = 23};

  for (const bool paused : {true, false}) {
    JukeboxStateFixture fixture;
    fixture.isPlaying = false;
    fixture.schedulerActive = false;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    bool audioBecameEligibleDuringCommit = false;
    const auto result = jukebox_lifecycle::StartPlayback(
        lifecycle, "Jukebox::restorePlayback", state, target,
        [&] {
          audioBecameEligibleDuringCommit = fixture.stopwatch.isRunning();
        },
        fixture.wakeScheduler(),
        {.positionMicros = 654321, .running = !paused});

    require(result.success && fixture.isPlaying && fixture.schedulerActive,
            "active snapshot restoration republishes the active session");
    require(
        !audioBecameEligibleDuringCommit,
        "active snapshot commits audio before the clock can become eligible");
    require(fixture.stopwatch.isRunning() == !paused,
            "active snapshot restores its exact paused/running mode");
    if (paused) {
      require(fixture.stopwatch.elapsedMicros() == 654321,
              "active paused snapshot restores the exact frozen position");
    } else {
      require(fixture.stopwatch.elapsedMicros() >= 654321,
              "active running snapshot resumes from the requested position");
    }
  }

  for (const bool paused : {true, false}) {
    JukeboxStateFixture fixture;
    fixture.isPlaying = false;
    fixture.schedulerActive = false;
    auto state = fixture.state();
    long long restoredAudioMicros = -1;
    const auto result = jukebox_lifecycle::RestoreInactivePlayback(
        state, {.positionMicros = 777777, .running = !paused},
        [&] {
          require(!fixture.stopwatch.isRunning(),
                  "inactive audio position commits while the clock is frozen");
          restoredAudioMicros = 777777;
        },
        fixture.wakeScheduler());

    require(result.success && !fixture.isPlaying && !fixture.schedulerActive &&
                restoredAudioMicros == 777777,
            "inactive snapshot stays inactive while restoring its position");
    require(fixture.stopwatch.isRunning() == !paused,
            "inactive snapshot preserves its paused/running mode");
    if (paused) {
      require(fixture.stopwatch.elapsedMicros() == 777777,
              "inactive paused snapshot restores the exact frozen position");
    } else {
      require(fixture.stopwatch.elapsedMicros() >= 777777,
              "inactive running snapshot resumes from the requested position");
    }
  }
}

void testConfigurableWrapperRestartsAndRestoresRetainedPcm() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(
      &stopwatch, std::make_unique<FakeConfigurableFactory>(control));
  require(wrapper.capabilities() == control->capabilities,
          "wrapper exposes injected runtime capabilities");
  require(wrapper.runtimeState().effectiveSampleRate == 44100,
          "wrapper publishes the initially effective sample rate");

  const path_t sound = PATH("configurable-retained-pcm");
  require(wrapper.loadGeneratedSound(sound, std::vector<short>(44100, 123), 1,
                                     44100),
          "configurable wrapper retains one second of source PCM");
  require(wrapper.getSoundDurationMicros(sound) == 1'000'000,
          "initial PCM duration is one second");

  require(wrapper.stopSounds().success,
          "runtime is drained before a configurable restart");
  std::string error;
  require(wrapper.restart({.deviceId = "usb",
                           .sampleRate = 48000,
                           .bufferFrames = 64},
                          error),
          "wrapper opens and starts the requested stream");
  require(wrapper.runtimeState().request.deviceId == "usb" &&
              wrapper.runtimeState().effectiveSampleRate == 48000,
          "wrapper commits candidate runtime state only after start");
  require(wrapper.getSoundDurationMicros(sound) == 1'000'000,
          "retained source PCM is regenerated without duration drift");

  const audio::RuntimeState previous{
      .request = {}, .effectiveSampleRate = 44100};
  require(wrapper.stopSounds().success,
          "candidate stream drains before restoration");
  require(wrapper.restore(previous, error),
          "wrapper can reopen the exact previous request");
  require(wrapper.runtimeState().effectiveSampleRate == 44100 &&
              wrapper.getSoundDurationMicros(sound) == 1'000'000,
          "rollback regenerates PCM for the previous stream");

  require(wrapper.stopSounds().success,
          "restored stream drains before a failed candidate probe");
  control->startResults = {false};
  require(!wrapper.restart({.deviceId = "usb",
                            .sampleRate = 48000,
                            .bufferFrames = 64},
                           error),
          "candidate start failure is reported without replacing runtime");
  require(wrapper.runtimeState().effectiveSampleRate == 44100,
          "failed candidate does not replace published runtime state");
  require(wrapper.restore(previous, error) &&
              wrapper.getSoundDurationMicros(sound) == 1'000'000,
          "post-failure rollback regenerates retained PCM for old runtime");

  require(control->opens.size() == 5 && control->opens[0].deviceId.empty() &&
              control->opens[1].deviceId == "usb" &&
              control->opens[2].deviceId.empty() &&
              control->opens[3].deviceId == "usb" &&
              control->opens[4].deviceId.empty(),
          "factory receives successful and failed transaction requests in order");
}

void testPlaybackRateRequiresStoppedPitchShiftAndScalesChartClock() {
  for (const auto [percent, expectedMicros] :
       std::array<std::pair<int, long long>, 2>{std::pair{50, 500'000LL},
                                                std::pair{200, 2'000'000LL}}) {
    Stopwatch stopwatch;
    auto control = std::make_shared<FactoryControl>();
    AudioWrapper wrapper(&stopwatch,
                         std::make_unique<FakeConfigurableFactory>(control));
    std::string error;

    require(!wrapper.setPlaybackRate({.percent = percent}, error) &&
                error.find("stopped") != std::string::npos,
            "playback-rate mutation rejects a running backend");
    require(wrapper.stopSounds().success,
            "clock fixture drains before selecting 48 kHz");
    require(wrapper.restart({.sampleRate = 48000}, error),
            "clock fixture starts a 48 kHz stream");
    require(wrapper.stopSounds().success,
            "clock fixture drains before rate mutation");
    require(!wrapper.setPlaybackRate(
                {.percent = percent, .mode = audio::PlaybackMode::TimeStretch},
                error) &&
                error.find("TimeStretch") != std::string::npos,
            "time-stretch mode fails with an explicit unsupported error");
    require(wrapper.setPlaybackRate({.percent = percent}, error) &&
                wrapper.playbackRate() ==
                    audio::PlaybackRate{.percent = percent},
            "stopped pitch-shift rate mutation is retained");

    require(control->renderCallback != nullptr &&
                control->renderUserData != nullptr,
            "configurable backend exposes the production render callback");
    stopwatch.start();
    std::vector<std::int16_t> output(48'000 * 2);
    control->renderCallback(output.data(), 48'000, 2, control->renderUserData);
    std::array<std::int16_t, 1> emptyOutput{};
    control->renderCallback(emptyOutput.data(), 0, 2, control->renderUserData);
    stopwatch.pause();

    require(
        wrapper.getTimeMicros() == expectedMicros,
        percent == 50
            ? "48,000 frames advance 500,000 chart microseconds at 50 percent"
            : "48,000 frames advance 2,000,000 chart microseconds at 200 "
              "percent");
  }
}

void testPausedMidBufferRateTransitionPreservesPublishedPosition() {
  for (const int percent : {50, 200}) {
    Stopwatch stopwatch;
    auto control = std::make_shared<FactoryControl>();
    AudioWrapper wrapper(&stopwatch,
                         std::make_unique<FakeConfigurableFactory>(control));
    std::string error;
    require(wrapper.stopSounds().success,
            "transition fixture drains its initial stream");
    require(wrapper.restart({.sampleRate = 48000}, error),
            "transition fixture starts at 48 kHz");
    require(wrapper.stopSounds().success &&
                wrapper.setPlaybackRate({.percent = 100}, error),
            "transition fixture selects a stopped 100 percent rate");
    require(wrapper.startDevice().success,
            "transition fixture restarts at 100 percent");

    stopwatch.start();
    std::vector<std::int16_t> output(48'000 * 2);
    control->renderCallback(output.data(), 48'000, 2, control->renderUserData);
    auto *callbackData = static_cast<UserData *>(control->renderUserData);
    const long long wallNow =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    callbackData->audioClockAnchorWallMicros->store(wallNow - 250'000);
    const long long publishedMicros = wrapper.getTimeMicros();
    require(publishedMicros >= 240'000 && publishedMicros <= 260'000,
            "paused transition fixture captures a mid-buffer position");
    stopwatch.pause();

    require(wrapper.stopSounds().success &&
                wrapper.setPlaybackRate({.percent = percent}, error),
            "stopped transition rebases before selecting its new rate");
    require(wrapper.getTimeMicros() == publishedMicros,
            "paused rate mutation preserves the exact published chart "
            "position instead of the submitted buffer end");
    require(wrapper.startDevice().success,
            "transition fixture restarts without an explicit seek");

    stopwatch.resume();
    control->renderCallback(output.data(), 48'000, 2, control->renderUserData);
    control->renderCallback(output.data(), 1, 2, control->renderUserData);
    stopwatch.pause();
    const long long expected =
        publishedMicros + (percent == 50 ? 500'000 : 2'000'000);
    require(wrapper.getTimeMicros() == expected,
            "the restarted buffer advances from the rebased chart position");
  }
}

void testRunningMidBufferStopAndRateTransitionDoesNotJump() {
  for (const int percent : {50, 200}) {
    Stopwatch stopwatch;
    auto control = std::make_shared<FactoryControl>();
    AudioWrapper wrapper(&stopwatch,
                         std::make_unique<FakeConfigurableFactory>(control));
    std::string error;
    require(wrapper.stopSounds().success,
            "running transition fixture drains its initial stream");
    require(wrapper.restart({.sampleRate = 48000}, error),
            "running transition fixture starts at 48 kHz");
    require(wrapper.stopSounds().success &&
                wrapper.setPlaybackRate({.percent = 100}, error) &&
                wrapper.startDevice().success,
            "running transition fixture starts at 100 percent");

    stopwatch.start();
    std::vector<std::int16_t> output(48'000 * 2);
    control->renderCallback(output.data(), 48'000, 2, control->renderUserData);
    auto *callbackData = static_cast<UserData *>(control->renderUserData);
    const long long wallNow =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    callbackData->audioClockAnchorWallMicros->store(wallNow - 250'000);
    const long long beforeStopMicros = wrapper.getTimeMicros();
    require(beforeStopMicros >= 240'000 && beforeStopMicros <= 260'000,
            "running transition fixture observes a mid-buffer position");

    require(wrapper.stopSounds().success &&
                wrapper.setPlaybackRate({.percent = percent}, error),
            "running transition drains before selecting its new rate");
    const long long rebasedMicros = wrapper.getTimeMicros();
    require(rebasedMicros >= beforeStopMicros &&
                rebasedMicros - beforeStopMicros <= 20'000,
            "running-to-stopped rate mutation preserves the interpolated "
            "position instead of jumping to the submitted buffer end");
    require(wrapper.startDevice().success,
            "running transition fixture restarts after rate mutation");

    control->renderCallback(output.data(), 48'000, 2, control->renderUserData);
    control->renderCallback(output.data(), 1, 2, control->renderUserData);
    stopwatch.pause();
    const long long expected =
        rebasedMicros + (percent == 50 ? 500'000 : 2'000'000);
    require(wrapper.getTimeMicros() == expected,
            "running transition advances one new-rate second from its "
            "captured position");
  }
}

void testWallInterpolationUsesTheRatePublishedWithItsAnchor() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  auto *callbackData = static_cast<UserData *>(control->renderUserData);
  require(callbackData != nullptr &&
              callbackData->audioClockAnchorRatePercent != nullptr,
          "callback data publishes an anchor-associated playback rate");

  const long long wallNow =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  callbackData->audioClockAnchorMicros->store(1'000'000);
  callbackData->audioClockAnchorEndMicros->store(2'000'000);
  callbackData->audioClockAnchorWallMicros->store(wallNow - 100'000);
  callbackData->audioClockAnchorRatePercent->store(50);
  callbackData->playbackRatePercent->store(200);
  stopwatch.start();
  const long long interpolated = wrapper.getTimeMicros();
  stopwatch.pause();
  require(interpolated >= 1'045'000 && interpolated <= 1'065'000,
          "wall interpolation uses the callback anchor's 50 percent rate");
}

void testClockAnchorReaderWaitsForACompleteGeneration() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  auto *callbackData = static_cast<UserData *>(control->renderUserData);
  require(callbackData != nullptr &&
              callbackData->audioClockAnchorSequence != nullptr,
          "callback data exposes the clock-anchor publication generation");

  const std::uint64_t previousGeneration =
      callbackData->audioClockAnchorSequence->fetch_add(
          1, std::memory_order_acq_rel);
  require((previousGeneration & 1U) == 0,
          "clock-anchor fixture begins from a complete generation");
  auto reader =
      std::async(std::launch::async, [&] { return wrapper.getTimeMicros(); });
  require(reader.wait_for(20ms) == std::future_status::timeout,
          "clock reader retries while an anchor generation is incomplete");

  callbackData->audioClockAnchorMicros->store(777'777,
                                              std::memory_order_relaxed);
  callbackData->audioClockAnchorWallMicros->store(0, std::memory_order_relaxed);
  callbackData->audioClockAnchorEndMicros->store(888'888,
                                                 std::memory_order_relaxed);
  callbackData->audioClockAnchorRatePercent->store(50,
                                                   std::memory_order_relaxed);
  callbackData->audioClockAnchorSequence->fetch_add(1,
                                                    std::memory_order_release);

  require(reader.wait_for(1s) == std::future_status::ready &&
              reader.get() == 777'777,
          "clock reader returns the tuple only after its generation completes");
}

void testNativeTimestampClampsToPublishedAudioBuffer() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  auto *callbackData = static_cast<UserData *>(control->renderUserData);
  require(callbackData != nullptr, "native timestamp fixture has clock data");

  callbackData->audioClockAnchorMicros->store(1'000'000,
                                              std::memory_order_relaxed);
  callbackData->audioClockAnchorWallMicros->store(5'000'000,
                                                  std::memory_order_relaxed);
  callbackData->audioClockAnchorEndMicros->store(1'010'000,
                                                 std::memory_order_relaxed);
  callbackData->audioClockAnchorRatePercent->store(50,
                                                   std::memory_order_relaxed);

  const auto mapped = wrapper.songTimeMicrosAtSteadyMicros(5'200'000);
  require(mapped.has_value() && *mapped == 1'010'000,
          "native input time cannot advance beyond rendered audio");
  const auto olderInput = wrapper.songTimeMicrosAtSteadyMicros(4'800'000);
  require(olderInput.has_value() && *olderInput == 900'000,
          "an older native input retains its age before the current anchor");
}

void testConfigurableWrapperReleasesOldStreamBeforeOpenAndRollback() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  control->rejectConcurrentStreams = true;
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  require(control->liveStreams == 1,
          "initial construction owns exactly one configurable stream");

  require(wrapper.stopSounds().success,
          "single-stream fixture drains before reconfiguration");
  std::string error;
  require(
      wrapper.restart(
          {.deviceId = "usb", .sampleRate = 48000, .bufferFrames = 64}, error),
      "restart releases the stopped working stream before candidate open");
  require(control->liveStreams == 1 &&
              wrapper.runtimeState().request.deviceId == "usb",
          "successful replacement owns only the candidate stream");

  const audio::RuntimeState previous = wrapper.runtimeState();
  require(wrapper.stopSounds().success,
          "replacement drains before a candidate open failure");
  control->openResults = {false};
  require(!wrapper.restart(
              {.deviceId = "default", .sampleRate = 44100, .bufferFrames = 128},
              error),
          "candidate open failure remains visible to the transaction");
  require(control->liveStreams == 0,
          "failed open leaves the released device available for rollback");
  require(wrapper.restore(previous, error),
          "rollback reopens the previous request after an open failure");
  require(control->liveStreams == 1 &&
              wrapper.runtimeState().request == previous.request,
          "open-failure rollback restores exactly one working stream");

  require(wrapper.stopSounds().success,
          "replacement drains before a candidate start failure");
  control->startResults = {false};
  require(!wrapper.restart(
              {.deviceId = "default", .sampleRate = 44100, .bufferFrames = 128},
              error),
          "candidate start failure remains visible to the transaction");
  require(control->liveStreams == 0,
          "failed candidate closes before rollback reopens the old request");
  require(wrapper.restore(previous, error),
          "rollback reopens the previous request under one-stream ownership");
  require(control->liveStreams == 1 &&
              wrapper.runtimeState().request == previous.request,
          "rollback leaves exactly one restored working stream");
}

void testBufferCapabilitySelectionUsesCachedNativeLimits() {
  constexpr std::array<std::uint32_t, 7> candidates{0,   64,   128, 256,
                                                    512, 1024, 2048};

  require(audio::SelectPortAudioBufferFrameOptions(candidates, std::nullopt) ==
              std::vector<std::uint32_t>(candidates.begin(), candidates.end()),
          "generic PortAudio devices expose request choices without opening "
          "probe streams");
  require(audio::SelectPortAudioBufferFrameOptions(
              candidates,
              audio::NativeBufferFrameLimits{.minimum = 256,
                                             .maximum = 256,
                                             .preferred = 256,
                                             .granularity = 0}) ==
              std::vector<std::uint32_t>({0, 64, 128, 256}),
          "fixed-size ASIO metadata keeps only requests that divide the "
          "native buffer");
  require(audio::SelectPortAudioBufferFrameOptions(
              candidates,
              audio::NativeBufferFrameLimits{.minimum = 128,
                                             .maximum = 512,
                                             .preferred = 256,
                                             .granularity = -1}) ==
              std::vector<std::uint32_t>({0, 64, 128, 256, 512}),
          "power-of-two ASIO metadata filters requests without creating a "
          "device stream");
}

void testConfigurableWrapperRecoversFromAuthoritativeExternalStop() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  control->events.clear();
  control->authoritativeState = audio::playback::BackendRunState::Stopped;

  const auto restarted = wrapper.startDevice();

  require(restarted.success,
          "authoritatively stopped stream can be restarted after device loss");
  require(control->events == std::vector<std::string>{"start:"},
          "wrapper consults native run state instead of a stale started flag");
  require(control->authoritativeState ==
              audio::playback::BackendRunState::Running,
          "successful recovery republishes the authoritative running state");
}

void testSoundSubmissionsRecoverFromAuthoritativeExternalStop() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sound = PATH("authoritative-submission-recovery");
  require(wrapper.loadGeneratedSound(sound, {100, 200, 300, 400}, 1, 44100),
          "submission recovery fixture retains production PCM");

  control->events.clear();
  control->authoritativeState = audio::playback::BackendRunState::Stopped;
  require(wrapper.playSound(sound, audio::Bus::Bgm),
          "playSound recovers an authoritatively stopped stream");
  require(control->events == std::vector<std::string>{"start:"} &&
              control->authoritativeState ==
                  audio::playback::BackendRunState::Running,
          "playSound observes and restarts the native stream before submit");

  control->events.clear();
  control->authoritativeState = audio::playback::BackendRunState::Stopped;
  require(wrapper.scheduleSound(sound, audio::Bus::Keysound, 123456),
          "scheduleSound recovers an authoritatively stopped stream");
  require(
      control->events == std::vector<std::string>{"start:"} &&
          control->authoritativeState ==
              audio::playback::BackendRunState::Running,
      "scheduleSound observes and restarts the native stream before submit");
}

void testRealtimeKeysoundHandleCommitsWithoutLookupOrLifecycleWork() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sound = PATH("realtime-keysound");
  require(wrapper.loadGeneratedSound(sound, {16384, 16384, 16384, 16384}, 1,
                                     44100),
          "realtime fixture retains production PCM");

  const auto handle = wrapper.resolveRealtimeSound(sound);
  require(handle.has_value() && handle->valid(),
          "chart setup pre-resolves a stable realtime sound handle");
  require(!wrapper.resolveRealtimeSound(PATH("missing-realtime-sound"))
               .has_value(),
          "missing chart sounds fail during setup, not on the input path");

  const auto reservation = wrapper.tryReserveRealtimeSoundCommand();
  require(reservation.has_value(),
          "the gameplay transaction reserves audio capacity first");
  require(wrapper.commitRealtimeKeysound(*reservation, *handle),
          "the pre-resolved handle commits into its reserved slot");

  require(control->renderCallback != nullptr &&
              control->renderUserData != nullptr,
          "realtime fixture exposes the production callback");
  stopwatch.start();
  std::array<std::int16_t, 8> output{};
  control->renderCallback(output.data(), 4, 2, control->renderUserData);
  stopwatch.pause();
  require(std::ranges::any_of(output, [](std::int16_t sample) {
            return sample != 0;
          }),
          "the next audio callback renders the committed realtime keysound");
}

void testRealtimeReservationExcludesLifecycleResetUntilCommit() {
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<GatedBackend>(control));
  const path_t sound = PATH("realtime-lifecycle-gate");
  require(wrapper.loadGeneratedSound(sound, {100, 200, 300, 400}, 1, 44100),
          "lifecycle-gate fixture retains PCM");
  const auto handle = wrapper.resolveRealtimeSound(sound);
  const auto reservation = wrapper.tryReserveRealtimeSoundCommand();
  require(handle.has_value() && reservation.has_value(),
          "lifecycle-gate fixture reserves a realtime transaction");

  auto stopFuture = std::async(std::launch::async,
                               [&wrapper] { return wrapper.stopSounds(); });
  require(stopFuture.wait_for(20ms) == std::future_status::timeout,
          "backend lifecycle reset waits for the reserved transaction");
  require(wrapper.commitRealtimeKeysound(*reservation, *handle),
          "reserved keysound commit remains valid while lifecycle waits");
  require(stopFuture.wait_for(2s) == std::future_status::ready &&
              stopFuture.get().success,
          "lifecycle reset continues after the transaction commits");
}

void testRealtimeReservationDoesNotContendOnUnrelatedLifecycleWork() {
  Stopwatch stopwatch;
  auto control = std::make_shared<BackendControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<GatedBackend>(control));
  const path_t sound = PATH("realtime-no-lifecycle-contention");
  require(wrapper.loadGeneratedSound(sound, {100, 200, 300, 400}, 1, 44100),
          "realtime contention fixture retains PCM");
  const auto handle = wrapper.resolveRealtimeSound(sound);
  require(handle.has_value(), "realtime contention fixture resolves a handle");

  {
    std::lock_guard lock(control->mutex);
    control->blockObserve = true;
    control->observeEntered = false;
    control->releaseObserve = false;
  }
  auto ordinarySubmission = std::async(
      std::launch::async,
      [&wrapper, &sound] { return wrapper.playSound(sound, audio::Bus::Bgm); });
  {
    std::unique_lock lock(control->mutex);
    require(control->condition.wait_for(
                lock, 2s, [&] { return control->observeEntered; }),
            "ordinary lifecycle work holds its mutex for the fixture");
  }

  const auto reservation = wrapper.tryReserveRealtimeSoundCommand();
  {
    std::lock_guard lock(control->mutex);
    control->releaseObserve = true;
  }
  control->condition.notify_all();
  require(ordinarySubmission.get(), "ordinary submission completes");
  require(reservation.has_value(),
          "realtime audio capacity remains available without taking the "
          "lifecycle mutex");
  require(wrapper.commitRealtimeKeysound(*reservation, *handle),
          "lock-free reservation still commits the matching keysound");
}

void testLuaSkinProductionAdapterPreservesUnrelatedAudioOwnership() {
  WrapperFixture fixture;
  const path_t skinPath = PATH("skin-adapter-sound.ogg");
  const path_t retainedPath = PATH("skin-adapter-retained");
  fixture.load(skinPath);
  fixture.load(retainedPath);
  float systemVolume = 0.3F;
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      *fixture.wrapper, [&systemVolume] { return systemVolume; });
  auto secondSessionBackend = skin::createLuaSkinApplicationAudioBackend(
      *fixture.wrapper, [&systemVolume] { return systemVolume; });
  require(backend != nullptr && backend->systemVolume() == 0.3F,
          "production adapter reads the current application system volume");
  require(!backend->load(std::filesystem::path(PATH("missing-device-audio")), {})
               .has_value(),
          "production adapter fails closed when the decoder-backed sound is genuinely unavailable");

  const auto firstIdentity = backend->load(std::filesystem::path(skinPath), {});
  const auto secondIdentity =
      secondSessionBackend->load(std::filesystem::path(skinPath), {});
  require(firstIdentity.has_value() && secondIdentity.has_value(),
          "two skin sessions load private same-path identities");
  backend->play(*firstIdentity, 0.6F, true);
  secondSessionBackend->play(*secondIdentity, 0.4F, true);
  const int stopsBefore = fixture.control->stopCalls.load();
  const int startsBefore = fixture.control->startCalls.load();
  backend->stop(*firstIdentity);
  require(fixture.wrapper->getSoundDurationMicros(skinPath).has_value() &&
              fixture.wrapper->getSoundDurationMicros(retainedPath).has_value() &&
              fixture.control->stopCalls.load() == stopsBefore &&
              fixture.control->startCalls.load() == startsBefore,
          "selective stop retains shared chart audio without cycling the device");
  backend->dispose(*firstIdentity);
  secondSessionBackend->dispose(*secondIdentity);
  require(fixture.wrapper->getSoundDurationMicros(skinPath).has_value() &&
              fixture.wrapper->getSoundDurationMicros(retainedPath).has_value() &&
              fixture.wrapper->playSound(skinPath, audio::Bus::Keysound) &&
              fixture.wrapper->playSound(retainedPath, audio::Bus::Bgm) &&
              fixture.control->stopCalls.load() == stopsBefore &&
              fixture.control->startCalls.load() == startsBefore,
          "selective dispose releases private voices without mutating same-path "
          "keysound or unrelated BGM ownership");
}

void testReplayLuaAudioBackendIsExplicitlyNoOutputAndCancellable() {
  auto backend = skin::createLuaSkinNoOutputAudioBackend();
  require(backend != nullptr && backend->systemVolume() == 0.0F,
          "replay Lua audio backend advertises explicit no-output volume");
  const auto identity = backend->load(PATH("replay-skin-sound.ogg"), {});
  require(identity.has_value(),
          "no-output replay backend preserves skin identity semantics");
  backend->play(*identity, 1.0F, true);
  backend->stop(*identity);
  backend->dispose(*identity);
  std::stop_source cancelled;
  cancelled.request_stop();
  require(!backend->load(PATH("cancelled-replay-sound.ogg"),
                         cancelled.get_token())
               .has_value(),
          "no-output replay backend honors its export session stop token");
}

void testLuaSkinRetirementWaitsForCallbackWhileChartClockContinues() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("callback-shared-skin-and-chart.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 12000), 1, 44100),
          "callback fixture retains same-path chart PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 2,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "callback fixture starts private skin and shared chart owners");
  backend->play(*identity, 0.5F, true);
  require(control->renderCallback != nullptr,
          "callback fixture exposes the running production mixer");

  stopwatch.start();
  std::array<std::int16_t, 8> first{};
  control->renderCallback(first.data(), 4, 2, control->renderUserData);
  const long long beforeStop = wrapper.getTimeMicros();
  control->events.clear();
  backend->dispose(*identity);
  require(!backend->load(std::filesystem::path(sharedPath), {}).has_value(),
          "disposed PCM remains quota-accounted until callback acknowledgement");

  std::array<std::int16_t, 8> second{};
  control->renderCallback(second.data(), 4, 2, control->renderUserData);
  const long long afterStop = wrapper.getTimeMicros();
  const auto replacement =
      backend->load(std::filesystem::path(sharedPath), {});
  stopwatch.pause();
  require(replacement.has_value() && afterStop > beforeStop &&
              std::ranges::any_of(second, [](std::int16_t sample) {
                return sample != 0;
              }) &&
              control->events.empty(),
          "callback acknowledgement releases private PCM while same-path BGM "
          "and the chart clock continue without device stop or restart");
}

void testLuaSkinDisposeRetriesAfterCommandQueueCapacityReturns() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("queue-full-shared-skin-and-chart.ogg");
  const path_t fillerPath = PATH("queue-full-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 12000), 1, 44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 100), 1, 44100),
          "queue-full retirement fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "queue-full retirement fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  std::array<std::int16_t, 8> first{};
  control->renderCallback(first.data(), 4, 2, control->renderUserData);

  std::size_t queued = 0;
  while (queued <= kAudioCommandQueueSize &&
         wrapper.playSound(fillerPath, audio::Bus::System)) {
    ++queued;
  }
  require(queued == kAudioCommandQueueSize,
          "queue-full retirement fixture exhausts callback command capacity");
  const std::size_t eventsBefore = control->events.size();
  backend->dispose(*identity);
  require(!backend->load(std::filesystem::path(sharedPath), {}).has_value(),
          "ordinary queue pressure retains disposed quota until the owner "
          "control is acknowledged");
  std::array<std::int16_t, 8> acknowledge{};
  control->renderCallback(acknowledge.data(), 4, 2,
                          control->renderUserData);
  const auto replacement =
      backend->load(std::filesystem::path(sharedPath), {});
  require(replacement.has_value(),
          "the reserved owner-control admission releases quota after callback "
          "acknowledgement");
  require(wrapper.getSoundDurationMicros(sharedPath).has_value() &&
              control->events.size() == eventsBefore,
          "queue-full retry preserves same-path chart ownership without a "
          "device stop or restart");
}

void testLuaSkinDisposeAcknowledgesWhileCallbackIsStopped() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("stopped-callback-shared-skin-and-chart.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 9000), 1, 44100),
          "stopped-callback retirement fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "stopped-callback fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  std::array<std::int16_t, 8> first{};
  control->renderCallback(first.data(), 4, 2, control->renderUserData);

  control->authoritativeState = audio::playback::BackendRunState::Stopped;
  const std::size_t eventsBefore = control->events.size();
  backend->dispose(*identity);
  const auto replacement =
      backend->load(std::filesystem::path(sharedPath), {});
  require(replacement.has_value(),
          "authoritatively stopped callback retires and releases private PCM "
          "synchronously");
  require(wrapper.getSoundDurationMicros(sharedPath).has_value() &&
              control->events.size() == eventsBefore,
          "stopped callback cleanup preserves same-path chart ownership and "
          "does not cycle the device");
}

void testLuaSkinBackendTeardownRetriesQueueFullRetirement() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("teardown-shared-skin-and-chart.ogg");
  const path_t fillerPath = PATH("teardown-queue-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 7000), 1, 44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 0), 1, 44100),
          "teardown retirement fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "teardown fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  std::array<std::int16_t, 8> first{};
  control->renderCallback(first.data(), 4, 2, control->renderUserData);
  std::size_t queued = 0;
  while (queued <= kAudioCommandQueueSize &&
         wrapper.playSound(fillerPath, audio::Bus::System)) {
    ++queued;
  }
  require(queued == kAudioCommandQueueSize,
          "teardown fixture exhausts callback command capacity");
  const std::size_t eventsBefore = control->events.size();

  auto teardown = std::async(
      std::launch::async,
      [owned = std::move(backend)]() mutable { owned.reset(); });
  require(teardown.wait_for(2s) == std::future_status::ready,
          "backend teardown transfers owner work without waiting for ordinary "
          "callback capacity");
  teardown.get();
  auto replacementBackend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  require(!replacementBackend->load(std::filesystem::path(sharedPath), {})
               .has_value(),
          "transferred teardown retains private PCM until callback acknowledgement");
  std::array<std::int16_t, 8> drain{};
  control->renderCallback(drain.data(), 4, 2, control->renderUserData);
  require(replacementBackend->load(std::filesystem::path(sharedPath), {})
              .has_value() &&
              wrapper.getSoundDurationMicros(sharedPath).has_value() &&
              control->events.size() == eventsBefore,
          "teardown acknowledgement releases quota, preserves same-path chart "
          "ownership, and does not cycle the device");
}

void testLuaSkinStopSurvivesOrdinaryCommandQueuePressure() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("stop-queue-shared-skin-and-chart.ogg");
  const path_t fillerPath = PATH("stop-queue-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 7000), 1, 44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 0), 1, 44100),
          "stop queue fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; });
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "stop queue fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  stopwatch.start();
  std::array<std::int16_t, 8> before{};
  control->renderCallback(before.data(), 4, 2, control->renderUserData);
  std::size_t queued = 0;
  while (queued <= kAudioCommandQueueSize &&
         wrapper.playSound(fillerPath, audio::Bus::System)) {
    ++queued;
  }
  require(queued == kAudioCommandQueueSize,
          "stop queue fixture exhausts ordinary callback commands");
  const std::size_t eventsBefore = control->events.size();

  backend->stop(*identity);
  std::array<std::int16_t, 8> drain{};
  control->renderCallback(drain.data(), 4, 2, control->renderUserData);
  std::array<std::int16_t, 8> after{};
  control->renderCallback(after.data(), 4, 2, control->renderUserData);
  stopwatch.pause();
  require(after[0] != 0 && after[0] < before[0],
          "queue-full audio_stop removes the looping skin voice while the "
          "same-path chart voice continues");
  require(wrapper.getSoundDurationMicros(sharedPath).has_value() &&
              control->events.size() == eventsBefore,
          "queue-full audio_stop preserves chart ownership without cycling "
          "the device");
}

void testLuaSkinRepeatedStopCoalescesOwnerControlPressure() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("stop-retry-shared-skin-and-chart.ogg");
  const path_t fillerPath = PATH("stop-retry-control-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 6500), 1, 44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 0), 1, 44100),
          "stop retry fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; });
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  std::atomic_bool cancelled = false;
  const auto filler = wrapper.loadSkinSound(
      fillerPath, cancelled, 1024, 1024 * 1024, {});
  require(identity.has_value() && filler.handle.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm),
          "stop retry fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  stopwatch.start();
  std::array<std::int16_t, 8> before{};
  control->renderCallback(before.data(), 4, 2, control->renderUserData);

  for (std::size_t attempt = 0;
       attempt < kOwnerControlCommandQueueSize + 16; ++attempt) {
    require(wrapper.stopSkinSound(*filler.handle),
            "repeated stop requests coalesce behind one pending owner "
            "control");
  }
  backend->stop(*identity);
  std::array<std::int16_t, 8> drain{};
  control->renderCallback(drain.data(), 4, 2, control->renderUserData);
  std::array<std::int16_t, 8> after{};
  control->renderCallback(after.data(), 4, 2, control->renderUserData);
  stopwatch.pause();
  require(after[0] != 0 && after[0] < before[0],
          "coalesced owner pressure leaves capacity for a different skin "
          "identity stop");
}

void testLuaSkinOwnerControlPressureNeverInterruptsSharedDevice(
    audio::playback::BackendRunState teardownState) {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("owner-pressure-shared-chart.ogg");
  const path_t keysoundPath = PATH("owner-pressure-keysound.ogg");
  const path_t fillerPath = PATH("owner-pressure-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 6000), 1, 44100) &&
              wrapper.loadGeneratedSound(keysoundPath,
                                         std::vector<short>(64, 3000), 1,
                                         44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 0), 1, 44100),
          "owner pressure fixture retains private, BGM, and keysound PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 272});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  std::atomic_bool cancelled = false;
  const auto filler = wrapper.loadSkinSound(
      fillerPath, cancelled, 1024, 1024 * 1024, {});
  require(identity.has_value() && filler.handle.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Bgm) &&
              wrapper.playSound(keysoundPath, audio::Bus::Keysound),
          "owner pressure fixture starts private and unrelated chart voices");
  backend->play(*identity, 0.5F, true);
  stopwatch.start();
  std::array<std::int16_t, 8> first{};
  control->renderCallback(first.data(), 4, 2, control->renderUserData);

  for (std::size_t attempt = 0;
       attempt < kOwnerControlCommandQueueSize; ++attempt) {
    require(wrapper.playSkinSound(*filler.handle, 0.1F, true) &&
                wrapper.stopSkinSound(*filler.handle),
            "alternating play/stop fills ordinary and owner-control admission "
            "without using the retirement reserve");
  }
  backend->stop(*identity);
  control->authoritativeState = teardownState;
  if (teardownState == audio::playback::BackendRunState::Unknown) {
    control->stopResults.push_front(false);
  }
  const std::size_t eventsBefore = control->events.size();

  auto teardown = std::async(
      std::launch::async,
      [owned = std::move(backend)]() mutable { owned.reset(); });
  require(teardown.wait_for(100ms) == std::future_status::ready,
          "owner-pressure teardown terminates for Running, Stopped, and "
          "Unknown backends");
  teardown.get();
  require(control->events.size() == eventsBefore,
          "owner-pressure teardown never stops or restarts the shared device");
  if (teardownState == audio::playback::BackendRunState::Running) {
    const auto realtimeReservation = wrapper.tryReserveRealtimeSoundCommand();
    require(realtimeReservation.has_value(),
            "running selective owner retirement leaves the realtime keysound "
            "gate open");
    wrapper.cancelRealtimeSoundCommand(*realtimeReservation);
  }

  auto replacement = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 272});
  if (teardownState != audio::playback::BackendRunState::Stopped) {
    require(!replacement->load(std::filesystem::path(sharedPath), {})
                 .has_value(),
            "teardown retains private decoded quota until callback "
            "acknowledgement");
    control->authoritativeState = audio::playback::BackendRunState::Running;
    std::array<std::int16_t, 8> acknowledge{};
    control->renderCallback(acknowledge.data(), 4, 2,
                            control->renderUserData);
    require(acknowledge[0] != 0,
            "unrelated BGM and keysound output continue through owner "
            "retirement");
  }
  require(replacement->load(std::filesystem::path(sharedPath), {}).has_value(),
          "selective owner acknowledgement releases handle and decoded quota");
  if (teardownState == audio::playback::BackendRunState::Unknown) {
    control->stopResults.clear();
    control->stopResults.push_back(true);
  }
  stopwatch.pause();
}

void testLuaSkinUnknownBackendTeardownTransfersOwnerWithoutSpinning() {
  Stopwatch stopwatch;
  auto control = std::make_shared<FactoryControl>();
  AudioWrapper wrapper(&stopwatch,
                       std::make_unique<FakeConfigurableFactory>(control));
  const path_t sharedPath = PATH("unknown-teardown-shared-chart.ogg");
  const path_t fillerPath = PATH("unknown-teardown-filler.ogg");
  require(wrapper.loadGeneratedSound(sharedPath,
                                     std::vector<short>(64, 6000), 1, 44100) &&
              wrapper.loadGeneratedSound(fillerPath,
                                         std::vector<short>(4, 0), 1, 44100),
          "unknown teardown fixture retains generated PCM");
  auto backend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  const auto identity = backend->load(std::filesystem::path(sharedPath), {});
  require(identity.has_value() &&
              wrapper.playSound(sharedPath, audio::Bus::Keysound),
          "unknown teardown fixture starts private and chart owners");
  backend->play(*identity, 0.5F, true);
  std::array<std::int16_t, 8> before{};
  control->renderCallback(before.data(), 4, 2, control->renderUserData);
  std::size_t queued = 0;
  while (queued <= kAudioCommandQueueSize &&
         wrapper.playSound(fillerPath, audio::Bus::System)) {
    ++queued;
  }
  require(queued == kAudioCommandQueueSize,
          "unknown teardown fixture exhausts ordinary callback commands");
  control->authoritativeState = audio::playback::BackendRunState::Unknown;
  const std::size_t eventsBefore = control->events.size();

  auto teardown = std::async(
      std::launch::async,
      [owned = std::move(backend)]() mutable { owned.reset(); });
  const bool terminated =
      teardown.wait_for(100ms) == std::future_status::ready;
  if (!terminated) {
    control->authoritativeState = audio::playback::BackendRunState::Stopped;
  }
  teardown.wait();
  teardown.get();
  require(terminated,
          "unknown non-draining backend teardown transfers owner work without "
          "busy waiting for ordinary queue capacity");

  auto replacementBackend = skin::createLuaSkinApplicationAudioBackend(
      wrapper, [] { return 1.0F; },
      {.maximumIdentities = 1,
       .maximumEncodedBytes = 1024,
       .maximumDecodedBytes = 256});
  require(!replacementBackend->load(std::filesystem::path(sharedPath), {})
               .has_value(),
          "unknown teardown retains decoded storage until callback ownership "
          "is acknowledged");
  control->authoritativeState = audio::playback::BackendRunState::Running;
  std::array<std::int16_t, 8> acknowledge{};
  control->renderCallback(acknowledge.data(), 4, 2,
                          control->renderUserData);
  require(replacementBackend->load(std::filesystem::path(sharedPath), {})
              .has_value() &&
              wrapper.getSoundDurationMicros(sharedPath).has_value() &&
              control->events.size() == eventsBefore,
          "deferred unknown teardown acknowledgement releases quota, preserves "
          "same-path keysound ownership, and does not restart the device");
}

} // namespace

int main() {
  try {
    testUnloadOneSerializesDrainClearAndEraseAgainstPlay();
    testUnloadAllSerializesDrainClearAndEraseAgainstPlay();
    testBatchPruneNoOpDoesNotObserveOrDrainBackend();
    testBatchPruneUsesOneConfirmationForEveryCandidate();
    testBatchPruneFailureKeepsOldMapAndPartiallyLoadedNewOwner();
    testBatchPruneSerializesProducerAgainstWholeErase();
    testConcurrentStartThenUnloadUsesOneLockOrder();
    testFailedStopRetainsSingleAndAllStorage();
    testDestructorReleasesBackendBeforeDependentStorageOnStopFailure();
    testJukeboxLifecycleActionsRunOnlyAfterConfirmation();
    testPlayingSeekAtomicallyExcludesStaleSchedulerAndReaders();
    testPlayingSeekFailureLeavesSchedulerAndReadersStopped();
    testSeekTransitionSerializesConcurrentPlayAndStopEntry();
    testJukeboxProductionStateTransitionsFailClosed();
    testPlaybackSnapshotClockModesRestoreWithoutPrematureEligibility();
    testPlaybackRateRequiresStoppedPitchShiftAndScalesChartClock();
    testPausedMidBufferRateTransitionPreservesPublishedPosition();
    testRunningMidBufferStopAndRateTransitionDoesNotJump();
    testWallInterpolationUsesTheRatePublishedWithItsAnchor();
    testClockAnchorReaderWaitsForACompleteGeneration();
    testNativeTimestampClampsToPublishedAudioBuffer();
    testConfigurableWrapperRestartsAndRestoresRetainedPcm();
    testConfigurableWrapperReleasesOldStreamBeforeOpenAndRollback();
    testBufferCapabilitySelectionUsesCachedNativeLimits();
    testConfigurableWrapperRecoversFromAuthoritativeExternalStop();
    testSoundSubmissionsRecoverFromAuthoritativeExternalStop();
    testRealtimeKeysoundHandleCommitsWithoutLookupOrLifecycleWork();
    testRealtimeReservationExcludesLifecycleResetUntilCommit();
    testRealtimeReservationDoesNotContendOnUnrelatedLifecycleWork();
    testLuaSkinProductionAdapterPreservesUnrelatedAudioOwnership();
    testReplayLuaAudioBackendIsExplicitlyNoOutputAndCancellable();
    testLuaSkinRetirementWaitsForCallbackWhileChartClockContinues();
    testLuaSkinDisposeAcknowledgesWhileCallbackIsStopped();
    testLuaSkinDisposeRetriesAfterCommandQueueCapacityReturns();
    testLuaSkinBackendTeardownRetriesQueueFullRetirement();
    testLuaSkinUnknownBackendTeardownTransfersOwnerWithoutSpinning();
    testLuaSkinStopSurvivesOrdinaryCommandQueuePressure();
    testLuaSkinRepeatedStopCoalescesOwnerControlPressure();
    testLuaSkinOwnerControlPressureNeverInterruptsSharedDevice(
        audio::playback::BackendRunState::Running);
    testLuaSkinOwnerControlPressureNeverInterruptsSharedDevice(
        audio::playback::BackendRunState::Stopped);
    testLuaSkinOwnerControlPressureNeverInterruptsSharedDevice(
        audio::playback::BackendRunState::Unknown);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "audio_wrapper_lifecycle_tests: " << error.what() << '\n';
    return 1;
  }
}
