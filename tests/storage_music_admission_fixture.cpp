#include "ThreadCompat.h"
#include "i18n/Localization.h"
#include <atomic>
#include <cassert>
#include <functional>
#include <future>
#include <utility>
#include <vector>
#include <mutex>
#include <optional>
#include <string>
namespace music_playlist { struct MusicTrack {}; }
namespace native_music_player {
int plays = 0, unloads = 0, stops = 0, pauses = 0;
enum class ControlEvent { Previous, Next, Finished };
struct State { bool loaded = true, playing = false; };
std::vector<ControlEvent> events;
std::function<void()> afterDrain;
std::vector<ControlEvent> DrainControlEvents() {
  auto result = std::exchange(events, {});
  if (afterDrain) afterDrain();
  return result;
}
std::vector<std::string> DrainPlaybackErrors() { return {}; }
State GetState() { return {}; }
bool Stop(std::string &) { ++stops; return true; }
bool Pause(std::string &) { ++pauses; return true; }
bool controlsEnabled = true;
bool failUnload = false;
void SetControlEventsEnabled(bool enabled) { controlsEnabled = enabled; }
bool IsSupported() { return true; }
bool Play(std::string &) { ++plays; return true; }
bool Unload(std::string &error) { ++unloads; if (failUnload) error = "native unload failed"; return !failUnload; }
}
class MusicPlayerService {
public:
  enum class PlaybackRequest { Current, Next, Previous };
  std::mutex stateMutex, playbackAdmissionMutex, playbackThreadMutex;
  bool storageMaintenance = false, clubMode = false;
  std::atomic_bool renderCancelled{false};
  std::atomic<std::uint64_t> playbackRequestRevision{0};
  std::jthread playbackThread;
  struct Queue {
    int moves = 0;
    music_playlist::MusicTrack track;
    const auto *Current() { return &track; }
    const auto *Next() { ++moves; return &track; }
    const auto *Previous() { ++moves; return &track; }
  } queue;
  int syncStarts = 0;
  int stops = 0, timerClears = 0, pumpStops = 0, pumpStarts = 0;
  bool pumpRunning = true;
  std::jthread eventThread;
  std::function<void()> releaseEvent;
  std::optional<music_playlist::MusicTrack> loadedTrack = music_playlist::MusicTrack{};
  bool PlayCurrentLocked(std::string &) { ++syncStarts; return true; }
  bool PlayNextLocked(std::string &) { queue.Next(); ++syncStarts; return true; }
  bool PlayPreviousLocked(std::string &) { queue.Previous(); ++syncStarts; return true; }
  void PersistQueueCursorLocked() {}
  void PlaybackWorker(music_playlist::MusicTrack, std::uint64_t, bool, i18n::Text, const std::stop_token &) {}
  void StopPlaybackWorker() { if (playbackThread.joinable()) playbackThread.join(); }
  bool Stop(std::string &) { ++stops; loadedTrack.reset(); StopPlaybackWorker(); return true; }
  void ClearSleepTimer() { ++timerClears; }
  void StopNativeControlEventPump() {
    ++pumpStops;
    pumpRunning = false;
    if (releaseEvent) releaseEvent();
    if (eventThread.joinable()) eventThread.join();
  }
  void EnsureNativeControlEventPump() { ++pumpStarts; pumpRunning = true; }
  bool ProcessNativeControlEvents(i18n::Text &);
  bool PlayNextAsync(i18n::Text &status, i18n::Text success = {}) {
    return StartPlaybackAsync(PlaybackRequest::Next, status, std::move(success));
  }
  bool PlayPreviousAsync(i18n::Text &status) {
    return StartPlaybackAsync(PlaybackRequest::Previous, status, {});
  }
  bool SetStorageMaintenance(bool, std::string &);
  bool PlayCurrent(std::string &);
  bool PlayNext(std::string &);
  bool PlayPrevious(std::string &);
  bool Resume(std::string &);
  bool StartPlaybackAsync(PlaybackRequest, i18n::Text &, i18n::Text);
};
METHODS
int main() {
  for (auto event : {native_music_player::ControlEvent::Finished,
                     native_music_player::ControlEvent::Next,
                     native_music_player::ControlEvent::Previous}) {
    MusicPlayerService retained;
    std::promise<void> drained, release;
    auto released = release.get_future();
    bool didRelease = false;
    retained.releaseEvent = [&] {
      if (!didRelease) { didRelease = true; release.set_value(); }
    };
    native_music_player::events = {event};
    native_music_player::afterDrain = [&] { drained.set_value(); released.wait(); };
    native_music_player::failUnload = true;
    native_music_player::stops = native_music_player::pauses = 0;
    retained.eventThread = std::jthread([&] {
      i18n::Text status;
      assert(retained.ProcessNativeControlEvents(status));
    });
    drained.get_future().wait();
    std::string error;
    assert(!retained.SetStorageMaintenance(true, error));
    // Let the old implementation finish too, so its stale-event mutation
    // fails an assertion instead of leaving the fixture waiting forever.
    retained.releaseEvent();
    if (retained.eventThread.joinable()) retained.eventThread.join();
    assert(native_music_player::stops == 0 && native_music_player::pauses == 0);
    assert(retained.loadedTrack && retained.queue.moves == 0);
    assert(retained.pumpStops == 1 && retained.pumpStarts == 1 && retained.pumpRunning);
    assert(retained.timerClears == 0 && error == "native unload failed");
    assert(retained.SetStorageMaintenance(false, error));
    native_music_player::afterDrain = {};
  }
  MusicPlayerService service;
  std::string error;
  i18n::Text status;
  assert(service.PlayCurrent(error));
  native_music_player::failUnload = true;
  assert(!service.SetStorageMaintenance(true, error));
  assert(error == "native unload failed" && service.loadedTrack && service.stops == 0);
  assert(service.timerClears == 0 && service.pumpStops == 1 && service.pumpStarts == 1);
  assert(service.SetStorageMaintenance(false, error));
  native_music_player::failUnload = false;
  native_music_player::unloads = 0;
  assert(service.SetStorageMaintenance(true, error));
  assert(native_music_player::unloads == 1);
  assert(!service.loadedTrack && service.timerClears == 1 && service.pumpStops == 2 && !service.pumpRunning);
  assert(!native_music_player::controlsEnabled);
  assert(!service.PlayCurrent(error));
  assert(!service.PlayNext(error));
  assert(!service.PlayPrevious(error));
  assert(!service.Resume(error));
  for (auto request : {MusicPlayerService::PlaybackRequest::Current,
                       MusicPlayerService::PlaybackRequest::Next,
                       MusicPlayerService::PlaybackRequest::Previous})
    assert(!service.StartPlaybackAsync(request, status, {}));
  assert(service.queue.moves == 0 && service.syncStarts == 1 && native_music_player::plays == 0);
  assert(service.SetStorageMaintenance(false, error));
  assert(native_music_player::controlsEnabled);
  assert(native_music_player::plays == 0); // no automatic resume
  assert(service.StartPlaybackAsync(MusicPlayerService::PlaybackRequest::Next, status, {}));
  service.StopPlaybackWorker();
  assert(service.queue.moves == 1);
}
