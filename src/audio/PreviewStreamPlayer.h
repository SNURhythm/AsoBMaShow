#pragma once
#include "AudioBackend.h"
#include "AudioSpectrum.h"
#include <cstdint>
#include <memory>
#include <string>

namespace audio {
class PagedPreviewSource;
// Foreground-only chart preview output. It owns a separate backend instance;
// gameplay's mixer, callbacks, scheduling, and complete-sound ownership remain
// unchanged. Preparation runs in the paged source; callbacks mix published PCM.
class PreviewStreamPlayer {
public:
  struct State {
    bool playing = false;
    bool buffering = false;
    bool finished = false;
    std::uint64_t positionFrames = 0;
    std::string error;
  };
  PreviewStreamPlayer();
  explicit PreviewStreamPlayer(std::unique_ptr<IBackendFactory> factory,
                               StreamRequest request = {});
  ~PreviewStreamPlayer();
  PreviewStreamPlayer(const PreviewStreamPlayer &) = delete;
  PreviewStreamPlayer &operator=(const PreviewStreamPlayer &) = delete;

  // Opens the device, then waits for the source startup reserve before
  // starting output. Async failures are logged and exposed through GetState.
  bool Start(std::shared_ptr<PagedPreviewSource> source, std::string &error);
  // Cancels/joins preparation and control, then quiesces output before returning.
  void Stop();
  // Device settings take effect on the next Start. Gain applies immediately.
  void SetRequest(StreamRequest request);
  void SetGain(float gain);
  [[nodiscard]] State GetState() const;
  // Normalized RMS of the most recent output block. Lock-free for UI animation.
  [[nodiscard]] float GetOutputLevel() const noexcept;
  // UI-thread consumer: FFT of the newest post-gain output, without player locks.
  [[nodiscard]] Spectrum GetOutputSpectrum();
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
