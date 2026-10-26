#pragma once
#include "../ui/Motion.h"

namespace streak {
struct OverlayMetrics { float unit, width, height, left, top; };
inline OverlayMetrics resultOverlayMetrics(float width, float height, float pixelScale,
    float uiScale, float top, float right, float bottom, float left) {
  const float unit = pixelScale > 0 && uiScale > 0 ? pixelScale / uiScale : 1;
  const float margin = 12 * unit;
  const float panelWidth = std::max(1.0F, std::min(440 * unit, width-left-right-2*margin));
  return {unit, panelWidth, std::max(1.0F, height-top-bottom-2*margin),
          width-right-margin-panelWidth, top+margin};
}

struct ResultEligibility {
  bool saved = false;
  bool manual = false;
  bool practice = false;
  bool replay = false;
  bool course = false;
  bool currentDay = false;
  bool activeRoutine = false;
};
inline bool showsResultProgress(const ResultEligibility &e) {
  return e.saved && e.manual && !e.practice && !e.replay && !e.course &&
      e.currentDay && e.activeRoutine;
}

// Survives view rebuilds. A successful retry can arm an unseen result, but
// resizing, redraws and subsequent retries never reopen a dismissed drawer.
class ResultPresentation {
public:
  bool arm(const ResultEligibility &eligibility, std::uint64_t now, bool reduced = false) {
    if (phase_ != Phase::Unseen || !showsResultProgress(eligibility)) return false;
    phase_ = Phase::Waiting;
    opensAt_ = now + (reduced ? 0 : 450);
    slide_.start(0, 1, now, 420, reduced ? 0 : 450, ui::motion::Ease::Smooth, reduced);
    return true;
  }
  bool open(std::uint64_t now) {
    if (phase_ != Phase::Waiting || now < opensAt_) return false;
    phase_ = Phase::Open;
    return true;
  }
  void dismiss() { if (active()) phase_ = Phase::Dismissed; slide_.settle(); }
  [[nodiscard]] bool active() const { return phase_ == Phase::Waiting || visible(); }
  [[nodiscard]] bool visible() const { return phase_ == Phase::Open; }
  [[nodiscard]] bool unseen() const { return phase_ == Phase::Unseen; }
  [[nodiscard]] float fraction(std::uint64_t now) const { return slide_.value(now); }
  [[nodiscard]] bool animating(std::uint64_t now) const { return active() && slide_.active(now); }
private:
  enum class Phase { Unseen, Waiting, Open, Dismissed } phase_ = Phase::Unseen;
  std::uint64_t opensAt_ = 0;
  ui::motion::Track slide_;
};
} // namespace streak
