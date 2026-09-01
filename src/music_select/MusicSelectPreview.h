#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class AudioWrapper;

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
  [[nodiscard]] std::optional<MusicSelectPreviewSwitch>
  update(std::int64_t nowMicros, bool launching);
  void reset();

private:
  std::optional<MusicSelectPreviewSelection> active_;
  std::optional<MusicSelectPreviewSelection> pending_;
  std::int64_t dueMicros_ = -1;
};

class MusicSelectPreviewAudioService {
public:
  explicit MusicSelectPreviewAudioService(AudioWrapper &);
  ~MusicSelectPreviewAudioService();

  MusicSelectPreviewAudioService(const MusicSelectPreviewAudioService &) =
      delete;
  MusicSelectPreviewAudioService &
  operator=(const MusicSelectPreviewAudioService &) = delete;

  void switchTo(std::optional<std::filesystem::path> path);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
