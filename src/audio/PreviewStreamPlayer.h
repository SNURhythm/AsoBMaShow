#pragma once
#include "AudioBackend.h"
#include <cstdint>
#include <memory>
#include <string>

namespace music_stream { class Source; }

namespace audio {
// Foreground-only chart preview output. It owns a separate backend instance;
// gameplay's mixer, callbacks, scheduling, and complete-sound ownership remain
// unchanged. Source reads and resampling run on the feeder, never the callback.
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

  // Opens the device, then asynchronously fills the startup reserve (or EOF) before
  // starting output. Async failures are logged and exposed through GetState.
  bool Start(std::shared_ptr<music_stream::Source> source,
             std::uint64_t firstFrame, std::string &error);
  // Cancels preparation, joins the feeder, and quiesces output before returning.
  void Stop();
  // Device settings take effect on the next Start. Gain applies immediately.
  void SetRequest(StreamRequest request);
  void SetGain(float gain);
  [[nodiscard]] State GetState() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
