#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace video {
// Worker-owned CPU budget. No GPU completion or drawable lifetime is involved.
class MetalPreparationPacer {
  std::array<double, 120> costs_{};
  std::size_t count_ = 0, next_ = 0;
public:
  void reset() noexcept { count_ = next_ = 0; costs_.fill(0); }
  void observe(double seconds) noexcept {
    if (!std::isfinite(seconds) || seconds <= 0) { reset(); return; }
    costs_[next_] = seconds;
    next_ = (next_ + 1) % costs_.size();
    count_ = std::min(count_ + 1, costs_.size());
  }
  double startTime(double now, double deadline, double interval) const noexcept {
    if (count_ < 30 || !std::isfinite(now) || !std::isfinite(deadline) ||
        !std::isfinite(interval) || interval <= 0 || deadline <= now) return now;
    const double cost = std::max(.003, *std::max_element(costs_.begin(), costs_.end()));
    const double delay = std::min({deadline - now - cost - .0015, .004, interval * .5});
    return now + std::max(0.0, delay);
  }
};

// Dependency-injected clock/sleep keep cancellation and oversleep deterministic
// in tests. The owner never pumps another display callback while holding a lease.
template<class Current, class Clock, class Sleep>
bool waitForMetalPreparation(double start, double deadline, Current current,
                             Clock clock, Sleep sleep) {
  if (!std::isfinite(start) || !std::isfinite(deadline)) return false;
  for (;;) {
    if (!current()) return false;
    const double now = clock();
    if (!std::isfinite(now) || now >= deadline) return false;
    if (now >= start) return true;
    sleep(std::min(.001, start - now));
  }
}
} // namespace video
