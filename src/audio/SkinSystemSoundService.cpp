#include "SkinSystemSoundService.h"

#include <cstdio>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace skin {

namespace {

// The Beatoraja default filename carries a ".wav" hint; the resolver searches
// every supported extension against the base name, so strip it.
std::string_view musicSelectSystemSoundFileBase(MusicSelectSystemSound sound) {
  const std::string_view filename = musicSelectSystemSoundFilename(sound);
  const std::size_t dot = filename.rfind('.');
  return dot == std::string_view::npos ? filename : filename.substr(0, dot);
}

} // namespace

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

std::optional<std::filesystem::path>
musicSelectSystemSoundPath(
    std::span<const std::filesystem::path> searchRoots,
    MusicSelectSystemSound sound) noexcept {
  const std::string_view base = musicSelectSystemSoundFileBase(sound);
  for (const std::filesystem::path &root : searchRoots) {
    for (const std::string_view extension : kBeatorajaSoundExtensions) {
      const auto candidate = root / std::filesystem::path(std::string(base) +
                                                          std::string(extension));
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error)) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

SkinSystemSoundService::SkinSystemSoundService(
    std::span<const std::filesystem::path> searchRoots, Playback playback,
    Warning warning)
    : searchRoots_(searchRoots.begin(), searchRoots.end()),
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
  // Resolve and warn on a missing asset even when no playback boundary is
  // injected, so callers always see why nothing plays.
  const auto path = pathFor(sound);
  if (!path) return;
  if (playback_) playback_(*path);
}

std::optional<std::filesystem::path>
SkinSystemSoundService::pathFor(MusicSelectSystemSound sound) const {
  const auto path = musicSelectSystemSoundPath(searchRoots_, sound);
  if (!path) {
    warn("Music select system sound missing, skipping: " +
         std::string(musicSelectSystemSoundFilename(sound)));
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