#include "audio/SkinSystemSoundService.h"

#include "music_select_skin_ledger_evidence.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using skin::MusicSelectSystemSound;
using skin::SkinSystemSoundService;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class SoundSandbox {
public:
  SoundSandbox() {
    root_ = std::filesystem::temp_directory_path() /
            ("music_select_system_sound_tests_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(root_);
  }
  ~SoundSandbox() { std::filesystem::remove_all(root_); }
  SoundSandbox(const SoundSandbox &) = delete;
  SoundSandbox &operator=(const SoundSandbox &) = delete;

  void write(std::string_view filename) {
    std::ofstream stream(root_ / std::filesystem::path(filename));
    stream << "fixture";
  }

  [[nodiscard]] const std::filesystem::path &root() const { return root_; }

private:
  std::filesystem::path root_;
};

void testWiredSoundsRouteToPinnedPaths() {
  SoundSandbox sandbox;
  for (const char *filename :
       {"o-change.wav", "scratch.wav", "f-open.wav", "f-close.wav"}) {
    sandbox.write(filename);
  }
  std::vector<std::filesystem::path> played;
  SkinSystemSoundService service(
      sandbox.root(),
      [&played](const std::filesystem::path &path) { played.push_back(path); });
  service.playOptionChange();
  service.playScratch();
  service.playFolderOpen();
  service.playFolderClose();
  const std::vector<std::filesystem::path> expected{
      sandbox.root() / "o-change.wav",
      sandbox.root() / "scratch.wav",
      sandbox.root() / "f-open.wav",
      sandbox.root() / "f-close.wav"};
  expect(played == expected,
         "option-change/scratch/folder-open/folder-close route through the "
         "injected playback boundary to their resolved asset paths");
}

void testRemainingSelectSubsetRoutesToPinnedPaths() {
  SoundSandbox sandbox;
  for (const char *filename :
       {"o-open.wav", "o-close.wav", "select.wav", "decide.wav"}) {
    sandbox.write(filename);
  }
  std::vector<std::filesystem::path> played;
  SkinSystemSoundService service(
      sandbox.root(),
      [&played](const std::filesystem::path &path) { played.push_back(path); });
  service.playOptionOpen();
  service.playOptionClose();
  service.playSelect();
  service.playDecide();
  const std::vector<std::filesystem::path> expected{
      sandbox.root() / "o-open.wav",
      sandbox.root() / "o-close.wav",
      sandbox.root() / "select.wav",
      sandbox.root() / "decide.wav"};
  expect(played == expected,
         "the remaining select subset (option-open/close/select/decide) "
         "routes to its pinned Beatoraja filenames");
}

void testFilenameMapMatchesPinnedSystemSoundManager() {
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::Scratch) == "scratch.wav",
         "SCRATCH maps to scratch.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::FolderOpen) == "f-open.wav",
         "FOLDER_OPEN maps to f-open.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::FolderClose) == "f-close.wav",
         "FOLDER_CLOSE maps to f-close.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::OptionChange) == "o-change.wav",
         "OPTION_CHANGE maps to o-change.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::OptionOpen) == "o-open.wav",
         "OPTION_OPEN maps to o-open.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::OptionClose) == "o-close.wav",
         "OPTION_CLOSE maps to o-close.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::Select) == "select.wav",
         "SELECT maps to select.wav");
  expect(skin::musicSelectSystemSoundFilename(
             MusicSelectSystemSound::Decide) == "decide.wav",
         "DECIDE maps to decide.wav");
}

void testMissingAssetRootWarnsAndSkipsPlayback() {
  std::vector<std::filesystem::path> played;
  std::vector<std::string> warnings;
  SkinSystemSoundService service(
      "/does/not/exist",
      [&played](const std::filesystem::path &path) { played.push_back(path); },
      [&warnings](std::string_view message) { warnings.emplace_back(message); });
  service.playOptionChange();
  service.playScratch();
  service.playFolderOpen();
  service.playFolderClose();
  expect(played.empty(),
         "missing assets never reach the playback boundary");
  expect(warnings.size() == 4,
         "every missing sound logs a warning instead of failing");
}

void testMissingAssetWarnsEvenWithoutPlaybackBoundary() {
  std::vector<std::string> warnings;
  SkinSystemSoundService service(
      "/does/not/exist", {},
      [&warnings](std::string_view message) { warnings.emplace_back(message); });
  service.playOptionChange();
  service.playScratch();
  service.playFolderOpen();
  service.playFolderClose();
  expect(warnings.size() == 4,
         "missing assets warn even when no playback boundary is injected");
}

} // namespace

int main(int argc, char **argv) {
  testWiredSoundsRouteToPinnedPaths();
  testRemainingSelectSubsetRoutesToPinnedPaths();
  testFilenameMapMatchesPinnedSystemSoundManager();
  testMissingAssetRootWarnsAndSkipsPlayback();
  testMissingAssetWarnsEvenWithoutPlaybackBoundary();
  return music_select_skin_ledger_evidence::finish(
      argc, argv, "music_select_system_sound_tests", failures,
      {"select.sound.effect.option-change", "select.sound.effect.scratch",
       "select.sound.folder-open", "select.sound.folder-close"},
      "music-select system sound assertion(s) failed",
      "music-select system sound tests passed");
}