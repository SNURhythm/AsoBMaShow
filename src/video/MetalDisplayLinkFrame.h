#pragma once

#include <cmath>
#include <cstdint>

namespace video {
// Display links choose display-supported rates. Preserve arbitrary and very
// low application caps with the timer instead of silently rounding them down.
inline float metalDisplayLinkRate(float refreshRate, std::uint32_t cap) noexcept {
  if (!std::isfinite(refreshRate) || refreshRate < 1.0F || refreshRate > 1000.0F)
    return 0.0F;
  if (cap == 0 || cap >= refreshRate) return refreshRate;
  if (cap < 30) return 0.0F;
  const float divisor = refreshRate / static_cast<float>(cap);
  return std::abs(divisor - std::round(divisor)) < 0.001F
      ? static_cast<float>(cap) : 0.0F;
}

// One worker-owned lease per scene iteration. A rejected managed lease must
// remain managed until finish(), so bgfx cannot acquire a replacement drawable.
class MetalDisplayLinkFrame {
public:
  void begin(std::uint64_t generation) noexcept {
    generation_ = generation;
    phase_ = Phase::Waiting;
  }
  bool receive(double now, double deadline) noexcept {
    if (phase_ != Phase::Waiting || !std::isfinite(now) ||
        !std::isfinite(deadline) || now <= 0.0 || deadline <= now) return false;
    phase_ = Phase::Ready;
    return true;
  }
  bool claim(bool active, std::uint64_t generation, bool geometryMatches) noexcept {
    if (phase_ != Phase::Ready) return false;
    phase_ = Phase::Claimed;
    return active && generation == generation_ && geometryMatches;
  }
  bool managed() const noexcept { return phase_ != Phase::Idle; }
  bool ready() const noexcept { return phase_ == Phase::Ready; }
  void finish() noexcept { phase_ = Phase::Idle; }

private:
  enum class Phase { Idle, Waiting, Ready, Claimed };
  Phase phase_ = Phase::Idle;
  std::uint64_t generation_ = 0;
};
} // namespace video
