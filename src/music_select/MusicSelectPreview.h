#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

struct MusicSelectPreviewSelection {
  std::string id;
  std::filesystem::path folder;
  std::filesystem::path previewPath;
  bool operator==(const MusicSelectPreviewSelection &) const = default;
};

struct MusicSelectPreviewMoveResult {
  bool stopAudio = false;
};

struct MusicSelectPreviewSwitch {
  std::optional<std::filesystem::path> path;
};

class MusicSelectPreviewController {
public:
  static constexpr std::int64_t kPreviewDelayMicros = 400'000;

  [[nodiscard]] MusicSelectPreviewMoveResult
  selectedBarMoved(std::optional<MusicSelectPreviewSelection>,
                   std::int64_t nowMicros);
  void observeSelection(std::optional<MusicSelectPreviewSelection>,
                        std::int64_t songBarChangeMicros);
  [[nodiscard]] std::optional<MusicSelectPreviewSwitch>
  update(std::int64_t nowMicros, bool launching);
  void reset();

private:
  std::optional<MusicSelectPreviewSelection> active_;
  std::optional<MusicSelectPreviewSelection> pending_;
  std::int64_t dueMicros_ = -1;
};

// Owns the worker that plays chart previews (and the looping default select BGM
// that Beatoraja's PreviewMusicProcessor uses as its fallback
// (PreviewMusicProcessor.java:41-43,79-95)). The AudioWrapper operations are
// injected through AudioPort so the routing is testable without miniaudio; the
// music-select scene wires the AudioWrapper-backed port. A missing asset is
// treated as an idle switch (the worker never fails).
class MusicSelectPreviewAudioService {
public:
  // Replace whatever the preview worker is playing with `path`, looping when
  // `loop`. `cancellation` is the shared per-request flag the service sets when
  // a newer switch supersedes this one while a decode is in flight, and `stop`
  // is the worker's teardown token; the port must not start playback when
  // either is set. Returns whether a sound is now playing.
  using PlayRequest =
      std::function<bool(const std::filesystem::path &path, bool loop,
                         const std::shared_ptr<std::atomic_bool> &cancellation,
                         std::stop_token stop)>;
  // Stop whatever the preview worker is playing (idle/teardown).
  using StopRequest = std::function<void()>;

  struct AudioPort {
    PlayRequest play;
    StopRequest stop;
  };

  explicit MusicSelectPreviewAudioService(AudioPort port,
                                          std::filesystem::path defaultPath = {});
  ~MusicSelectPreviewAudioService();

  MusicSelectPreviewAudioService(const MusicSelectPreviewAudioService &) =
      delete;
  MusicSelectPreviewAudioService &
  operator=(const MusicSelectPreviewAudioService &) = delete;

  // Replace whatever the preview worker is playing. `std::nullopt` means the
  // looping default select BGM (Beatoraja's PreviewMusicProcessor fallback), so
  // moving to a chart without `#PREVIEW` routes back to the SELECT sound.
  void switchTo(std::optional<std::filesystem::path> path);

  // Stop whatever the preview worker is playing and stay silent. Unlike
  // switchTo(nullopt), silence() does NOT start the default select BGM; the
  // scene uses it for pause and error/teardown, where Beatoraja's MusicSelector
  // stops audio entirely instead of resuming the select BGM.
  void silence();

  // Resume the looping default select BGM after silence() (e.g. when the scene
  // is brought back to the foreground). The worker treats nullopt as "play the
  // default", so resetting to that state restarts the BGM.
  void resumeDefaultBgm();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
