#include "audio/AudioWrapper.h"
#include "audio/Jukebox.h"
#include "audio/JukeboxLifecycle.h"
#include "audio/decoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

bool decodeAudioToPCM(const path_t &, std::vector<short> &, SF_INFO &,
                      std::atomic<bool> &) {
  return false;
}

bool decodeAudioBytesToPCM(const path_t &, const std::vector<unsigned char> &,
                           std::vector<short> &, SF_INFO &,
                           std::atomic<bool> &) {
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
  bool failStop = false;
  bool failStart = false;
  bool destroyed = false;
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
    std::lock_guard lock(control->mutex);
    return {.state = control->state};
  }

  int outputSampleRate() const override { return 44100; }

  audio::playback::BackendOperationResult stopAndDrain() override {
    control->stopCalls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(control->mutex);
    control->stopEntered = true;
    control->condition.notify_all();
    control->condition.wait(
        lock, [this] { return !control->blockStop || control->releaseStop; });
    if (control->failStop) {
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
  Stopwatch stopwatch;
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
            .stopwatch = stopwatch,
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
    const auto snapshot = jukebox_lifecycle::CaptureSeekState(state);
    lifecycle.stopResult = {.success = false,
                            .diagnostic = "seek drain failure"};
    auto result = jukebox_lifecycle::StopForSeek(
        lifecycle, "Jukebox::seek", state, snapshot, fixture.wakeScheduler());
    require(!result.success && fixture.isPlaying &&
                fixture.stopwatch.elapsedMicros() == 123456 &&
                fixture.audioCursor == 7 && fixture.schedulerWakes == 0,
            "seek stop failure leaves every logical position untouched");

    lifecycle.stopResult = {.success = true};
    result = jukebox_lifecycle::StopForSeek(lifecycle, "Jukebox::seek", state,
                                            snapshot, fixture.wakeScheduler());
    require(result.success && fixture.isPlaying &&
                fixture.stopwatch.elapsedMicros() == 123456 &&
                fixture.schedulerWakes == 1,
            "confirmed seek stop pauses without advancing the target position");

    lifecycle.startResult = {.success = false,
                             .diagnostic = "seek restart failure"};
    bool committedSeek = false;
    const jukebox_lifecycle::CursorPosition target{
        .audio = 4, .bmp = 5, .bmpLayer = 6};
    result = jukebox_lifecycle::RestartAndCommitSeek(
        lifecycle, "Jukebox::seek", state, snapshot, target, 654321,
        [&] { committedSeek = true; }, fixture.wakeScheduler());
    require(
        !result.success && !committedSeek && !fixture.isPlaying &&
            !fixture.stopwatch.isRunning() &&
            fixture.stopwatch.elapsedMicros() == 123456 &&
            fixture.audioCursor == 7 && fixture.bmpCursor == 8 &&
            fixture.bmpLayerCursor == 9 && fixture.schedulerWakes == 2,
        "seek restart failure stops the session without claiming target state");
  }

  {
    JukeboxStateFixture fixture;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    const auto snapshot = jukebox_lifecycle::CaptureSeekState(state);
    const jukebox_lifecycle::CursorPosition target{
        .audio = 4, .bmp = 5, .bmpLayer = 6};
    bool committedSeek = false;
    const auto result = jukebox_lifecycle::RestartAndCommitSeek(
        lifecycle, "Jukebox::seek", state, snapshot, target, 654321,
        [&] {
          require(fixture.audioCursor == 4 && fixture.bmpCursor == 5 &&
                      fixture.bmpLayerCursor == 6 &&
                      fixture.stopwatch.elapsedMicros() == 654321 &&
                      fixture.isPlaying,
                  "seek target is prepared before dependent audio commands");
          committedSeek = true;
        },
        fixture.wakeScheduler());
    require(
        result.success && committedSeek && fixture.isPlaying &&
            !fixture.stopwatch.isRunning() && fixture.schedulerWakes == 1,
        "confirmed paused seek commits target state without resuming clock");
  }

  {
    JukeboxStateFixture fixture;
    fixture.isPlaying = false;
    FakeJukeboxLifecycle lifecycle;
    auto state = fixture.state();
    const auto snapshot = jukebox_lifecycle::CaptureSeekState(state);
    const jukebox_lifecycle::CursorPosition target{
        .audio = 10, .bmp = 11, .bmpLayer = 12};
    bool committedSeek = false;
    jukebox_lifecycle::CommitStoppedSeek(
        state, snapshot, target, 777777, [&] { committedSeek = true; },
        fixture.wakeScheduler());
    require(
        committedSeek && !fixture.isPlaying && !fixture.stopwatch.isRunning() &&
            fixture.stopwatch.elapsedMicros() == 777777 &&
            fixture.audioCursor == 10 && fixture.bmpCursor == 11 &&
            fixture.bmpLayerCursor == 12 && lifecycle.events.empty() &&
            fixture.schedulerWakes == 1,
        "stopped seek commits position without starting or claiming playback");
  }
}

} // namespace

int main() {
  try {
    testUnloadOneSerializesDrainClearAndEraseAgainstPlay();
    testUnloadAllSerializesDrainClearAndEraseAgainstPlay();
    testConcurrentStartThenUnloadUsesOneLockOrder();
    testFailedStopRetainsSingleAndAllStorage();
    testDestructorReleasesBackendBeforeDependentStorageOnStopFailure();
    testJukeboxLifecycleActionsRunOnlyAfterConfirmation();
    testJukeboxProductionStateTransitionsFailClosed();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "audio_wrapper_lifecycle_tests: " << error.what() << '\n';
    return 1;
  }
}
