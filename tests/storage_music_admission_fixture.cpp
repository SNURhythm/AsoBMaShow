#include "ThreadCompat.h"
#include "i18n/Localization.h"
#include <atomic>
#include <cassert>
#include <mutex>
#include <optional>
#include <string>
namespace music_playlist { struct MusicTrack {}; }
namespace native_music_player {
int plays = 0, unloads = 0;
bool controlsEnabled = true;
void SetControlEventsEnabled(bool enabled) { controlsEnabled = enabled; }
bool IsSupported() { return true; }
bool Play(std::string &) { ++plays; return true; }
bool Unload(std::string &) { ++unloads; return true; }
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
  bool PlayCurrentLocked(std::string &) { ++syncStarts; return true; }
  bool PlayNextLocked(std::string &) { queue.Next(); ++syncStarts; return true; }
  bool PlayPreviousLocked(std::string &) { queue.Previous(); ++syncStarts; return true; }
  void PersistQueueCursorLocked() {}
  void PlaybackWorker(music_playlist::MusicTrack, std::uint64_t, bool, i18n::Text, const std::stop_token &) {}
  void StopPlaybackWorker() { if (playbackThread.joinable()) playbackThread.join(); }
  bool Stop(std::string &) { StopPlaybackWorker(); return true; }
  bool SetStorageMaintenance(bool, std::string &);
  bool PlayCurrent(std::string &);
  bool PlayNext(std::string &);
  bool PlayPrevious(std::string &);
  bool Resume(std::string &);
  bool StartPlaybackAsync(PlaybackRequest, i18n::Text &, i18n::Text);
};
METHODS
int main() {
  MusicPlayerService service;
  std::string error;
  i18n::Text status;
  assert(service.PlayCurrent(error));
  assert(service.SetStorageMaintenance(true, error));
  assert(native_music_player::unloads == 1);
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
