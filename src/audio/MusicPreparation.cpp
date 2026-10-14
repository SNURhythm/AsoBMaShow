#include "MusicPreparation.h"

#include "../PlayOptionUtils.h"
#include "../Utils.h"
#include "../targets.h"
#include "ProgressiveMusicSource.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>
#include <utility>

namespace music_preparation {
namespace {
std::filesystem::path legacyCacheDirectory() {
#if TARGET_OS_ANDROID
  return std::filesystem::temp_directory_path() / "music_cache";
#else
  return Utils::GetDocumentsPath("music_cache");
#endif
}
} // namespace

void RemoveLegacyMusicCacheFiles(const std::filesystem::path &directory) {
  std::error_code error;
  if (std::filesystem::symlink_status(directory, error).type() !=
          std::filesystem::file_type::directory || error) {
    return;
  }
  std::filesystem::directory_iterator entries(directory, error);
  const std::filesystem::directory_iterator end;
  while (!error && entries != end) {
    const auto path = entries->path();
    const auto type = entries->symlink_status(error).type();
    if (error) break;
    if (type == std::filesystem::file_type::regular) {
      auto extension = path.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(),
                     [](unsigned char c) {
                       return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
                     });
      if (extension == ".wav" || extension == ".tmp") {
        std::filesystem::remove(path, error);
      }
    } else if (type == std::filesystem::file_type::directory &&
               path.filename().string().starts_with(".music-stream-")) {
      std::filesystem::remove_all(path, error);
    }
    if (error) break;
    entries.increment(error);
  }
  if (!error) {
    // Only succeeds if no unrecognized entries remain.
    std::filesystem::remove(directory, error);
  }
}

PreparedMusic PrepareMusicForPlayback(const bms_parser::ChartMeta &meta,
                                      std::atomic_bool &cancelled, bool clubMode,
                                      chart_audio::LogCallback log) {
  const auto failure = [](std::string message) -> PreparedMusic {
    return {.message = std::move(message)};
  };
  if (cancelled.load()) return failure("Music preparation cancelled");
  RemoveLegacyMusicCacheFiles(legacyCacheDirectory());
  if (meta.BmsPath.empty()) return failure("Chart path is empty");
  auto chart = play_options::parseChart(meta.BmsPath, cancelled, "music stream");
  if (cancelled.load()) return failure("Music preparation cancelled");
  if (!chart) return failure("Could not parse chart for music playback");
  auto source = music_stream::Source::Create(
      std::move(chart), std::filesystem::temp_directory_path() / "music-streams", clubMode);
  std::array<short, 2> probe{};
  for (;;) {
    if (cancelled.load()) {
      source->Cancel();
      return failure("Music preparation cancelled");
    }
    const auto state = source->GetState();
    if (!state.error.empty()) return failure(state.error);
    if (state.cancelled) return failure("Music preparation cancelled");
    if (state.complete || state.availableFrames >= music_stream::kInitialBufferFrames) {
      if (log) log("Music opening buffer ready: " + std::to_string(state.availableFrames) + " frames");
      const auto durationMicros =
          (state.durationFrames / music_stream::kSampleRate) * 1'000'000 +
          ((state.durationFrames % music_stream::kSampleRate) * 1'000'000 +
           music_stream::kSampleRate - 1) / music_stream::kSampleRate;
      return {.success = true,
              .message = "Music opening buffer ready",
              .durationMicros = static_cast<long long>(durationMicros),
              .stream = std::move(source)};
    }
    // A bounded source wait also lets a superseding selection cancel preparation.
    source->ReadFrames(state.availableFrames, probe, std::chrono::milliseconds(50));
  }
}

} // namespace music_preparation
