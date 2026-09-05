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
  const std::vector<std::filesystem::path> roots{sandbox.root()};
  std::vector<std::filesystem::path> played;
  SkinSystemSoundService service(
      roots,
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
  const std::vector<std::filesystem::path> roots{sandbox.root()};
  SkinSystemSoundService service(
      roots,
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
  const std::vector<std::filesystem::path> roots{"/does/not/exist"};
  SkinSystemSoundService service(
      roots,
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
  const std::vector<std::filesystem::path> roots{"/does/not/exist"};
  SkinSystemSoundService service(
      roots, {},
      [&warnings](std::string_view message) { warnings.emplace_back(message); });
  service.playOptionChange();
  service.playScratch();
  service.playFolderOpen();
  service.playFolderClose();
  expect(warnings.size() == 4,
         "missing assets warn even when no playback boundary is injected");
}

void testResolutionHelperFindsOggInConfiguredSet() {
  SoundSandbox setDir;
  setDir.write("f-open.ogg");
  const std::vector<std::filesystem::path> roots{setDir.root()};
  const auto resolved = skin::musicSelectSystemSoundPath(
      roots, MusicSelectSystemSound::FolderOpen);
  expect(resolved && *resolved == setDir.root() / "f-open.ogg",
         "a configured sound-set dir with only f-open.ogg resolves FolderOpen "
         "to that .ogg");
}

void testResolutionHelperPicksWavOverOgg() {
  SoundSandbox setDir;
  setDir.write("scratch.wav");
  setDir.write("scratch.ogg");
  const std::vector<std::filesystem::path> roots{setDir.root()};
  const auto resolved =
      skin::musicSelectSystemSoundPath(roots, MusicSelectSystemSound::Scratch);
  expect(resolved && *resolved == setDir.root() / "scratch.wav",
         "Beatoraja extension order resolves scratch.wav over scratch.ogg");
}

void testResolutionHelperPrefersConfiguredSetOverBundled() {
  SoundSandbox setDir;
  SoundSandbox bundled;
  setDir.write("o-change.flac");
  bundled.write("o-change.wav");
  const std::vector<std::filesystem::path> roots{setDir.root(), bundled.root()};
  const auto resolved = skin::musicSelectSystemSoundPath(
      roots, MusicSelectSystemSound::OptionChange);
  expect(resolved && *resolved == setDir.root() / "o-change.flac",
         "the configured sound-set dir wins over a bundled root even when the "
         "bundled root has a higher-priority extension");
}

void testResolutionHelperFallsBackToBundledRoot() {
  SoundSandbox setDir;
  SoundSandbox bundled;
  bundled.write("o-change.flac");
  const std::vector<std::filesystem::path> roots{setDir.root(), bundled.root()};
  const auto resolved = skin::musicSelectSystemSoundPath(
      roots, MusicSelectSystemSound::OptionChange);
  expect(resolved && *resolved == bundled.root() / "o-change.flac",
         "a sound-set dir with nothing falls back to the bundled root");
}

void testEachBeatorajaExtensionResolvesAlone() {
  for (const std::string_view extension : skin::kBeatorajaSoundExtensions) {
    const std::string filename = std::string("decide") + std::string(extension);
    SoundSandbox setDir;
    setDir.write(filename);
    const std::vector<std::filesystem::path> roots{setDir.root()};
    const auto resolved =
        skin::musicSelectSystemSoundPath(roots, MusicSelectSystemSound::Decide);
    expect(resolved && *resolved == setDir.root() / filename,
           filename + " alone resolves Decide to that path");
  }
}

void testMissingEverywhereReturnsNulloptWithWarningAndNoPlayback() {
  SoundSandbox setDir;
  SoundSandbox bundled;
  std::vector<std::filesystem::path> played;
  std::vector<std::string> warnings;
  const std::vector<std::filesystem::path> roots{setDir.root(), bundled.root()};
  SkinSystemSoundService service(
      roots,
      [&played](const std::filesystem::path &path) { played.push_back(path); },
      [&warnings](std::string_view message) { warnings.emplace_back(message); });
  service.playOptionChange();
  expect(played.empty(),
         "a sound-set sound missing everywhere never reaches playback");
  expect(warnings.size() == 1,
         "a sound-set sound missing everywhere warns once per call without "
         "throwing");
}

void testServiceIntegrationRoutsConfiguredOggThroughPlayback() {
  SoundSandbox setDir;
  SoundSandbox bundled;
  setDir.write("f-open.ogg");
  std::vector<std::filesystem::path> played;
  const std::vector<std::filesystem::path> roots{setDir.root(), bundled.root()};
  SkinSystemSoundService service(
      roots,
      [&played](const std::filesystem::path &path) { played.push_back(path); });
  service.playFolderOpen();
  const std::vector<std::filesystem::path> expected{
      setDir.root() / "f-open.ogg"};
  expect(played == expected,
         "the service routes a configured-set .ogg to the playback boundary");
}

} // namespace

int main(int argc, char **argv) {
  testWiredSoundsRouteToPinnedPaths();
  testRemainingSelectSubsetRoutesToPinnedPaths();
  testFilenameMapMatchesPinnedSystemSoundManager();
  testMissingAssetRootWarnsAndSkipsPlayback();
  testMissingAssetWarnsEvenWithoutPlaybackBoundary();
  testResolutionHelperFindsOggInConfiguredSet();
  testResolutionHelperPicksWavOverOgg();
  testResolutionHelperPrefersConfiguredSetOverBundled();
  testResolutionHelperFallsBackToBundledRoot();
  testEachBeatorajaExtensionResolvesAlone();
  testMissingEverywhereReturnsNulloptWithWarningAndNoPlayback();
  testServiceIntegrationRoutsConfiguredOggThroughPlayback();
  return music_select_skin_ledger_evidence::finish(
      argc, argv, "music_select_system_sound_tests", failures,
      {"select.sound.effect.option-change", "select.sound.effect.scratch",
       "select.sound.folder-open", "select.sound.folder-close"},
      "music-select system sound assertion(s) failed",
      "music-select system sound tests passed");
}