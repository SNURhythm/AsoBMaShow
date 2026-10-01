#include "ThreadCompat.h"
#include "i18n/Localization.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace music_playlist {
struct MusicTrack {
  std::string title = "Cleared My Playlist.", artist = "Raw artist";
  int representativeChart = 1;
  long long durationMicros = 10000000;
};
struct Metadata { long long durationMicros = 0; };
Metadata MakeNativeMetadata(const MusicTrack &) { return {}; }
}
namespace native_music_player {
int loads = 0, starts = 0;
bool Load(const std::filesystem::path &, const music_playlist::Metadata &, std::string &) {
  ++loads;
  return true;
}
bool SetPlaybackRate(int, std::string &) { return true; }
bool Play(std::string &) { ++starts; return true; }
}
namespace chart_music_cache {
struct CacheResult {
  bool success = true;
  std::string message;
  std::filesystem::path audioPath = "controlled.wav";
  long long durationMicros = 10000000;
};
CacheResult nextResult;
CacheResult EnsureRenderedMusicFile(int, std::atomic_bool &, bool) { return nextResult; }
void PruneCacheExcept(const std::vector<std::filesystem::path> &) {}
}

class MusicPlayerService {
public:
  using Status = STATUS_TYPE;
  void PlaybackWorker(music_playlist::MusicTrack, std::uint64_t, bool,
                      Status, const std::stop_token &);
  void PublishNativeControlStatus(const Status &);
  bool ConsumeNativeControlStatus(Status &);
  void SyncNativeQueueLocked() {}
  std::vector<std::filesystem::path> PlaybackCacheKeepPathsLocked(
      const chart_music_cache::CacheResult &) { return {}; }
  std::vector<music_playlist::MusicTrack> AdjacentTracksLocked() { return {}; }
  void StartAdjacentPreloadWorker(std::vector<music_playlist::MusicTrack>, bool) {}
  void EnsureNativeControlEventPump() {}
  std::atomic<std::uint64_t> playbackRequestRevision{1};
  std::atomic_bool renderCancelled{false};
  std::mutex stateMutex, nativeControlStatusMutex;
  chart_music_cache::CacheResult lastCacheResult;
  int playbackRate = 100;
  std::optional<music_playlist::MusicTrack> loadedTrack;
  Status nativeControlStatusMessage;
  std::uint64_t nativeControlStatusRevision = 0, consumedNativeControlStatusRevision = 0;
};
SERVICE_METHODS

struct TextView {
  std::string text;
  void setText(const std::string &value) { text = value; }
  void setLocalizedText(const i18n::Text &value) { text = value.resolve(); }
};
struct Playlist { std::string name = "Cleared My Playlist."; int trackCount = 3; };
struct Player {
  struct Playback { bool supported = true, loaded = true, playing = true;
    long long positionMicros = 2000000, durationMicros = 10000000; };
  bool failClear = false;
  int clears = 0;
  std::size_t libraryTrackCount = 0, playlistTrackCount = 1;
  std::optional<music_playlist::MusicTrack> CurrentTrackSnapshot() { return music_playlist::MusicTrack{}; }
  Playback PlaybackState() { return {}; }
  std::optional<Playlist> DefaultPlaylistSnapshot() { return Playlist{}; }
  std::size_t LibraryTrackCount() { return libraryTrackCount; }
  std::vector<music_playlist::MusicTrack> DefaultPlaylistTracksSnapshot() { return std::vector<music_playlist::MusicTrack>(playlistTrackCount); }
  bool ClearDefaultPlaylist(std::string &error) {
    ++clears;
    if (failClear) error = "Cleared My Playlist.";
    return !failClear;
  }
};
namespace ui_theme {
int primaryAction, primaryActionHover, primaryActionPressed, accentBorderStrong,
    control, controlHover, controlPressed, hairlineStrong, warningAction,
    warningActionHover, warningActionPressed, accentBorder, successAction,
    successActionHover, successActionPressed, infoAction, infoActionHover, infoActionPressed;
}
template<class... Args> void styleThemedActionButton(Args...) {}
class MainMenuScene {
public:
  struct { Player musicPlayer; } context;
  MENU_STATUS_TYPE musicStatusMessage;
  TextView root, track, status, playlist, buttonText;
  TextView *musicModalRoot = &root, *musicTrackText = &track,
           *musicStatusText = &status, *musicPlaylistText = &playlist;
  TextView *musicPlayPauseButtonText = &buttonText;
  TextView *musicSelectedButtonText = nullptr, *musicAddSelectedButtonText = nullptr,
           *musicRemoveSelectedButtonText = nullptr, *musicPlaylistButtonText = nullptr,
           *musicClearPlaylistButtonText = nullptr, *musicRandomButtonText = nullptr,
           *musicPreviousButtonText = nullptr, *musicSeekBackwardButtonText = nullptr,
           *musicSeekForwardButtonText = nullptr, *musicNextButtonText = nullptr,
           *musicStopButtonText = nullptr, *musicCloseButtonText = nullptr;
  int musicSelectedButton = 0, musicAddSelectedButton = 0, musicRemoveSelectedButton = 0,
      musicPlaylistButton = 0, musicClearPlaylistButton = 0, musicRandomButton = 0,
      musicPreviousButton = 0, musicSeekBackwardButton = 0, musicPlayPauseButton = 0,
      musicSeekForwardButton = 0, musicNextButton = 0, musicStopButton = 0, musicCloseButton = 0;
  void refreshMusicModal();
  void clearSavedMusicPlaylist();
};
SCENE_METHODS

int failures = 0;
void expect(bool value, const char *message) {
  if (!value) { ++failures; std::cerr << message << '\n'; }
}
std::string resolve(const std::string &value) { return value; }
std::string resolve(const i18n::Text &value) { return value.resolve(); }
template<class Status> Status successMessage() {
  if constexpr (std::is_same_v<Status, std::string>)
    return i18n::tr("menu.playing_selected_chart.message");
  else
    return i18n::message("menu.playing_selected_chart.message");
}

int main() {
  i18n::setLanguage(i18n::Language::English);
  MainMenuScene menu;
  menu.clearSavedMusicPlaylist();
  i18n::setLanguage(i18n::Language::Korean);
  menu.refreshMusicModal();
  expect(menu.status.text.starts_with("내 재생 목록을 비웠습니다.\n"),
         "retained menu status must resolve in the newly selected language");
  expect(menu.context.musicPlayer.clears == 1,
         "refresh must not repeat the playlist operation");
  expect(menu.track.text == "Cleared My Playlist. / Raw artist",
         "chart metadata matching English UI copy stays raw");
  expect(menu.status.text.find("Cleared My Playlist.: 3") != std::string::npos,
         "playlist names matching English UI copy stay raw");
  menu.context.musicPlayer.libraryTrackCount = 42;
  menu.context.musicPlayer.playlistTrackCount = 8;
  menu.refreshMusicModal();
  expect(menu.status.text.find("라이브러리 곡: 42") != std::string::npos,
         "music summary localizes the library count in Korean");
  expect(menu.playlist.text.ends_with("+3곡 더") &&
             menu.playlist.text.find("Cleared My Playlist. / Raw artist") != std::string::npos,
         "playlist overflow localizes the hidden count while preserving track metadata");
  i18n::setLanguage(i18n::Language::Japanese);
  menu.refreshMusicModal();
  expect(menu.status.text.find("ライブラリの曲: 42") != std::string::npos &&
             menu.playlist.text.ends_with("ほか3曲"),
         "music summaries follow a later language change without changing tracks");
  menu.context.musicPlayer.failClear = true;
  menu.clearSavedMusicPlaylist();
  i18n::setLanguage(i18n::Language::Japanese);
  menu.refreshMusicModal();
  expect(menu.status.text.starts_with("Cleared My Playlist.\n"),
         "raw provider diagnostics must not acquire a message binding");

  i18n::setLanguage(i18n::Language::English);
  auto queued = successMessage<MusicPlayerService::Status>();
  MusicPlayerService service;
  i18n::setLanguage(i18n::Language::Korean);
  service.PlaybackWorker({}, 1, false, std::move(queued), {});
  MusicPlayerService::Status completion;
  expect(service.ConsumeNativeControlStatus(completion), "worker publishes completion once");
  expect(resolve(completion) == "선택한 채보를 재생합니다.",
         "completion queued before a language change retains its message ID");
  i18n::setLanguage(i18n::Language::Japanese);
  expect(resolve(completion) == "選択した譜面を再生します。",
         "consumed completion remains localizable after another language change");
  expect(!service.ConsumeNativeControlStatus(completion), "refresh does not consume completion twice");
  expect(native_music_player::loads == 1 && native_music_player::starts == 1,
         "language changes do not restart native playback");

  chart_music_cache::nextResult = {.success = false, .message = "Playing selected chart."};
  service.PlaybackWorker({}, 1, false, successMessage<MusicPlayerService::Status>(), {});
  expect(service.ConsumeNativeControlStatus(completion), "failed rendering publishes its diagnostic");
  i18n::setLanguage(i18n::Language::Korean);
  expect(resolve(completion) == "Playing selected chart.",
         "async diagnostics matching catalog copy stay raw");
  i18n::setLanguage(i18n::Language::English);
  return failures == 0 ? 0 : 1;
}
