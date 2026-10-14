#pragma once
#include <algorithm>
#include <cstdint>

namespace ios_music_stream {
// Source-frame clock for one AVAudioPlayerNode timeline. The node's sample clock
// can continue through starvation; never expose frames beyond queued audio.
class Timeline {
public:
  void Reset(std::uint64_t first) { base = first; queued = 0; }
  void Queue(std::uint64_t count) { queued += count; }
  std::uint64_t Position(std::int64_t nodeFrame, std::uint64_t played) const {
    return base + std::min(queued, std::max(played,
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, nodeFrame))));
  }
  bool Drained(std::uint64_t played) const { return played >= queued; }
  bool NeedsRestart(std::int64_t nodeFrame, std::uint64_t played) const {
    return queued && (Drained(played) ||
        (nodeFrame >= 0 && static_cast<std::uint64_t>(nodeFrame) >= queued));
  }
  std::uint64_t End() const { return base + queued; }
  std::uint64_t Queued() const { return queued; }
private:
  std::uint64_t base = 0;
  std::uint64_t queued = 0;
};
}
