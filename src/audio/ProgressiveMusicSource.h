#pragma once

#include "../bms_parser.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace audio { class DecodedAssetCache; }

namespace music_stream {

inline constexpr int kSampleRate = 44100;
inline constexpr int kChannels = 2;
inline constexpr std::uint64_t kInitialBufferFrames = kSampleRate * 2;
inline constexpr std::uint64_t kReadAheadFrames = kSampleRate * 10;

enum class ReadStatus { Data, Pending, End, Cancelled, Error };
struct ReadResult {
  ReadStatus status = ReadStatus::Pending;
  std::size_t frames = 0;
  std::string error;
};
struct State {
  std::uint64_t firstFrame = 0;
  // End of the contiguous prepared prefix beginning at firstFrame. Later
  // sought ranges may also be readable without preparing the intervening gap.
  std::uint64_t availableFrames = 0;
  // Starts at chart duration and expands as sound tails are discovered.
  // Exact once complete. Never use this estimate to signal playback completion.
  std::uint64_t durationFrames = 0;
  // True only when every frame in the source range is prepared and finalized.
  // ReadFrames may report End for a sought suffix before this becomes true.
  bool complete = false;
  bool cancelled = false;
  std::string error;
};
struct PreviewGains {
  float bgm = 1.0f;
  float keysound = 1.0f;
};
struct Statistics {
  std::uint64_t decoderOpens = 0;
};

// Listening-only source. Never decode, read files, or wait from an audio
// callback. Native feeder workers may call ReadFrames with bounded waits.
// Prepared audio is private temporary data, removed when the source is released.
class Source {
public:
  static std::shared_ptr<Source> Create(std::unique_ptr<bms_parser::Chart> chart,
                                        std::filesystem::path stagingParent,
                                        bool clubMode = false);
  // Starts directly at an arbitrary chart frame, including overlapping tails.
  // Snapshots chart data before returning.
  static std::shared_ptr<Source> CreatePreview(
      const bms_parser::Chart &chart, std::filesystem::path stagingParent,
      std::shared_ptr<audio::DecodedAssetCache> decodedAssets,
      std::uint64_t firstFrame, PreviewGains gains = {});
  ~Source();
  Source(const Source &) = delete;
  Source &operator=(const Source &) = delete;

  ReadResult ReadFrames(std::uint64_t firstFrame, std::span<short> interleaved,
                        std::chrono::milliseconds wait = std::chrono::milliseconds(100));
  [[nodiscard]] State GetState() const;
  [[nodiscard]] Statistics GetStatistics() const;
  // Prioritize a transport seek without waiting for an in-flight feeder read.
  // The single feeder must next read this target; stale reads return Pending.
  void RequestSeek(std::uint64_t firstFrame);
  // Play/resume enables production; pause/stop disables further read-ahead.
  void SetRenderingEnabled(bool enabled);
  // Terminal cancellation. Bounded reads immediately wake.
  void Cancel();

private:
  struct Impl;
  explicit Source(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

} // namespace music_stream
