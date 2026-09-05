#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

namespace skin {

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

  // assetRoot is resolved per sound as assetRoot/filename. Missing assets log
  // a warning (see Warning) and are skipped; the scene never fails on them.
  SkinSystemSoundService(std::filesystem::path assetRoot, Playback playback,
                         Warning warning = {});

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

  std::filesystem::path assetRoot_;
  Playback playback_;
  Warning warning_;
};

} // namespace skin