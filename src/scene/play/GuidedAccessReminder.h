#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gameplay {
// Animate for one foreground second, then allow another second for system UI to settle.
class GuidedAccessReminder {
public:
  static bool required(bool enabled, bool ipad, bool replay, bool autoPlay,
                       std::size_t courseTrackIndex, bool guidedAccessEnabled) {
    return enabled && ipad && !replay && !autoPlay && courseTrackIndex == 0 &&
           !guidedAccessEnabled;
  }

  void reset() {
    startedAt_.reset();
    elapsed_ = 0;
  }
  void update(bool guidedAccessEnabled, bool foreground, std::uint64_t now) {
    if (!guidedAccessEnabled || !foreground) {
      reset();
      return;
    }
    if (!startedAt_.has_value()) startedAt_ = now;
    elapsed_ = now >= *startedAt_ ? now - *startedAt_ : 0;
  }
  [[nodiscard]] bool confirming() const { return startedAt_.has_value(); }
  [[nodiscard]] bool completed() const { return confirming() && elapsed_ >= 2000; }
  [[nodiscard]] float progress() const {
    return static_cast<float>(std::min<std::uint64_t>(elapsed_, 1000)) / 1000.0F;
  }

private:
  std::optional<std::uint64_t> startedAt_;
  std::uint64_t elapsed_ = 0;
};
} // namespace gameplay
