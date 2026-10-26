#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui::motion {
enum class Ease { Linear, Smooth, Pop };
inline float ease(float t, Ease curve) {
  t = std::clamp(t, 0.0F, 1.0F);
  if (curve == Ease::Linear) return t;
  if (curve == Ease::Smooth) return 1 - std::pow(1 - t, 3);
  // Finite overshoot, with exact endpoints and no persistent spring simulation.
  const float x = t - 1;
  return 1 + 2.70158F * x * x * x + 1.70158F * x * x;
}

// Owner-held, deterministic scalar track. Explicit monotonic milliseconds make
// interruption, delayed starts and background/resume independent of frame rate.
class Track {
public:
  explicit Track(float value = 0) : from_(value), to_(value) {}
  void start(float from, float to, std::uint64_t now, std::uint64_t duration = 400,
             std::uint64_t delay = 0, Ease curve = Ease::Smooth, bool reduced = false) {
    from_ = from; to_ = to; start_ = now; duration_ = reduced ? 0 : duration;
    delay_ = reduced ? 0 : delay; curve_ = curve;
  }
  [[nodiscard]] float value(std::uint64_t now) const {
    if (!active(now)) return to_;
    if (now <= start_ || now - start_ <= delay_) return from_;
    return from_ + (to_ - from_) * ease(float(now - start_ - delay_) / duration_, curve_);
  }
  [[nodiscard]] bool active(std::uint64_t now) const {
    return from_ != to_ && duration_ > 0 &&
        (now < start_ || now - start_ < delay_ + duration_);
  }
  void settle() { from_ = to_; duration_ = delay_ = 0; }
  void cancel(std::uint64_t now) { from_ = to_ = value(now); duration_ = delay_ = 0; }
  void retarget(float to, std::uint64_t now, std::uint64_t duration = 400) {
    start(value(now), to, now, duration);
  }
private:
  float from_, to_;
  std::uint64_t start_ = 0, duration_ = 0, delay_ = 0;
  Ease curve_ = Ease::Smooth;
};
} // namespace ui::motion
