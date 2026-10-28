#pragma once
#include <algorithm>
#include <cstdint>

// Owned by the result scene, so view rebuilds cannot replay a completion cue.
class GoalResultPresentation {
public:
  static constexpr std::uint64_t duration = 4500;
  bool arm(bool achieved) {
    if (!achieved || phase_ != Phase::Unseen) return false;
    phase_ = Phase::Waiting;
    return true;
  }
  bool start(std::uint64_t now, bool blocked) {
    if (blocked || phase_ != Phase::Waiting) return false;
    phase_ = Phase::Visible; startedAt_ = now;
    return true;
  }
  bool finish(std::uint64_t now) {
    if (!visible() || now < startedAt_ || now - startedAt_ < duration) return false;
    dismiss(); return true;
  }
  void dismiss() { phase_ = Phase::Done; }
  bool active() const { return phase_ == Phase::Waiting || visible(); }
  bool visible() const { return phase_ == Phase::Visible; }
  std::uint64_t startedAt() const { return startedAt_; }
  static float opacity(std::uint64_t startedAt, std::uint64_t now) {
    const auto elapsed = now >= startedAt ? now - startedAt : 0;
    return std::min(std::clamp(float(elapsed) / 220, 0.0F, 1.0F),
        std::clamp((float(duration) - elapsed) / 500, 0.0F, 1.0F));
  }
private:
  enum class Phase { Unseen, Waiting, Visible, Done } phase_ = Phase::Unseen;
  std::uint64_t startedAt_ = 0;
};
