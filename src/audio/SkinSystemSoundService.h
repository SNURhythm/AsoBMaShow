#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace skin {

// Beatoraja AudioDriver.getPaths extension order
// (AudioDriver.java:158). A music-select system sound resolves against these
// four extensions in this exact order, matching libsndfile's content-based
// decoding.
inline constexpr std::array<std::string_view, 4> kBeatorajaSoundExtensions = {
    ".wav", ".flac", ".ogg", ".mp3"};

// Beatoraja SystemSoundManager.SoundType select subset
// (SystemSoundManager.java:129-151). The named entry points mirror the pinned
// select intents: SCRATCH, FOLDER_OPEN, FOLDER_CLOSE, OPTION_CHANGE,
// OPTION_OPEN, OPTION_CLOSE, SELECT, DECIDE.
enum class MusicSelectSystemSound {
  Select,
  Decide,
  FolderOpen,
  FolderClose,
  OptionChange,
  OptionOpen,
  OptionClose,
  Scratch,
};

// Beatoraja default filename for a system sound (SystemSoundManager.java).
[[nodiscard]] std::string_view
musicSelectSystemSoundFilename(MusicSelectSystemSound sound) noexcept;

// Resolves a music-select system sound across searchRoots, tried in order,
// using Beatoraja extension order within each root. searchRoots are the
// configured sound-set folder (when set) followed by the bundled `assets/`
// root. Returns the first existing file or std::nullopt when nothing matches;
// the caller decides whether to warn. Never throws.
[[nodiscard]] std::optional<std::filesystem::path>
musicSelectSystemSoundPath(std::span<const std::filesystem::path> searchRoots,
                           MusicSelectSystemSound sound) noexcept;

// Owns the sound *decision* (which asset to play, and whether it exists) and
// delegates *playback* to an injected boundary so the sound routing is
// testable without a real AudioWrapper. The scene wires the default playback,
// which goes through AudioWrapper::{loadSkinSound,playSkinSound} exactly like
// the preview service. Loads are synchronous on the UI thread and short, so no
// worker thread is needed.
class SkinSystemSoundService final {
public:
  using Playback = std::function<void(const std::filesystem::path &)>;
  using Warning = std::function<void(std::string_view)>;

  // searchRoots are resolved per sound in order (configured sound-set folder
  // first, bundled root last), with Beatoraja extension order inside each
  // root. Missing assets log a warning (see Warning) and are skipped; the
  // scene never fails on them.
  SkinSystemSoundService(std::span<const std::filesystem::path> searchRoots,
                         Playback playback, Warning warning = {});

  void playOptionChange();
  void playOptionOpen();
  void playOptionClose();
  void playScratch();
  void playFolderOpen();
  void playFolderClose();
  void playSelect();
  void playDecide();

  void play(MusicSelectSystemSound sound);

private:
  [[nodiscard]] std::optional<std::filesystem::path>
  pathFor(MusicSelectSystemSound sound) const;
  void warn(std::string_view message) const;

  std::vector<std::filesystem::path> searchRoots_;
  Playback playback_;
  Warning warning_;
};

} // namespace skin