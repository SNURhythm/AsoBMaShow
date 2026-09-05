#include "SkinSystemSoundService.h"

#include <cstdio>
#include <string>

namespace skin {

std::string_view
musicSelectSystemSoundFilename(MusicSelectSystemSound sound) noexcept {
  switch (sound) {
  case MusicSelectSystemSound::Select:
    return "select.wav";
  case MusicSelectSystemSound::Decide:
    return "decide.wav";
  case MusicSelectSystemSound::FolderOpen:
    return "f-open.wav";
  case MusicSelectSystemSound::FolderClose:
    return "f-close.wav";
  case MusicSelectSystemSound::OptionChange:
    return "o-change.wav";
  case MusicSelectSystemSound::OptionOpen:
    return "o-open.wav";
  case MusicSelectSystemSound::OptionClose:
    return "o-close.wav";
  case MusicSelectSystemSound::Scratch:
    return "scratch.wav";
  }
  return {};
}

SkinSystemSoundService::SkinSystemSoundService(std::filesystem::path assetRoot,
                                               Playback playback,
                                               Warning warning)
    : assetRoot_(std::move(assetRoot)),
      playback_(std::move(playback)),
      warning_(std::move(warning)) {}

void SkinSystemSoundService::playOptionChange() {
  play(MusicSelectSystemSound::OptionChange);
}

void SkinSystemSoundService::playOptionOpen() {
  play(MusicSelectSystemSound::OptionOpen);
}

void SkinSystemSoundService::playOptionClose() {
  play(MusicSelectSystemSound::OptionClose);
}

void SkinSystemSoundService::playScratch() {
  play(MusicSelectSystemSound::Scratch);
}

void SkinSystemSoundService::playFolderOpen() {
  play(MusicSelectSystemSound::FolderOpen);
}

void SkinSystemSoundService::playFolderClose() {
  play(MusicSelectSystemSound::FolderClose);
}

void SkinSystemSoundService::playSelect() {
  play(MusicSelectSystemSound::Select);
}

void SkinSystemSoundService::playDecide() {
  play(MusicSelectSystemSound::Decide);
}

void SkinSystemSoundService::play(MusicSelectSystemSound sound) {
  if (!playback_) return;
  const auto path = pathFor(sound);
  if (path) playback_(*path);
}

std::optional<std::filesystem::path>
SkinSystemSoundService::pathFor(MusicSelectSystemSound sound) const {
  const auto path = assetRoot_ / musicSelectSystemSoundFilename(sound);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    warn("Music select system sound missing, skipping: " + path.string());
    return std::nullopt;
  }
  return path;
}

void SkinSystemSoundService::warn(std::string_view message) const {
  if (warning_) {
    warning_(message);
    return;
  }
  std::fprintf(stderr, "[SkinSystemSoundService] %.*s\n",
               static_cast<int>(message.size()), message.data());
}

} // namespace skin