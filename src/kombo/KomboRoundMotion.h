#pragma once
#include "KomboModel.h"
#include "../ui/Motion.h"
#include <optional>

namespace kombo {
inline constexpr std::uint64_t roundFillMillis = 850;
inline constexpr std::uint64_t roundPromotionMillis = 1550;
inline constexpr std::uint64_t roundSettleMillis = 2000;
struct RoundFrame {
  float fraction = 0, promotion = 1;
  int round = 1, charts = 0, quota = 1;
  bool completed = false, active = false;
};
class RoundMotion {
public:
  RoundMotion(AssignmentProgressChange change, std::uint64_t start, bool reduced = false)
      : change_(std::move(change)), start_(start), reduced_(reduced) {}
  [[nodiscard]] RoundFrame frame(std::uint64_t now) const {
    const float after = std::clamp(float(change_.afterCharts) / std::max(1, change_.afterQuota), 0.0F, 1.0F);
    RoundFrame result{.fraction=after, .round=change_.afterRound,
                      .charts=change_.afterCharts, .quota=change_.afterQuota};
    const auto elapsed = now > start_ ? now-start_ : 0;
    if (reduced_) return result;
    if (change_.completedRound()) {
      if (elapsed < roundPromotionMillis) {
        const float before = std::clamp(float(change_.beforeCharts) / std::max(1, change_.beforeQuota), 0.0F, 1.0F);
        const float fill = ui::motion::ease(float(elapsed) / roundFillMillis, ui::motion::Ease::Smooth);
        result.fraction = before + (1-before)*fill;
        result.round = change_.beforeRound;
        result.quota = change_.beforeQuota;
        result.charts = elapsed < roundFillMillis ? change_.beforeCharts : change_.beforeQuota;
        result.completed = elapsed >= roundFillMillis;
      } else {
        result.promotion = ui::motion::ease(float(elapsed-roundPromotionMillis) /
            (roundSettleMillis-roundPromotionMillis), ui::motion::Ease::Smooth);
      }
      result.active = elapsed < roundSettleMillis;
    } else if (elapsed < roundFillMillis) {
      const float before = std::clamp(float(change_.beforeCharts) / std::max(1, change_.beforeQuota), 0.0F, 1.0F);
      const float fill = ui::motion::ease(float(elapsed) / roundFillMillis, ui::motion::Ease::Smooth);
      result.fraction = before + (after-before)*fill;
      result.charts = elapsed < roundFillMillis ? change_.beforeCharts : change_.afterCharts;
      result.active = true;
    }
    return result;
  }
private:
  AssignmentProgressChange change_;
  std::uint64_t start_;
  bool reduced_;
};

// One cue for simultaneous promotions; no delayed chime after background resume.
class RoundPromotionCue {
public:
  void start(const PlayProgress &progress, std::uint64_t now) {
    pending_ = std::any_of(progress.assignments.begin(), progress.assignments.end(),
        [](const auto &change) { return change.completedRound(); });
    at_ = now + roundPromotionMillis;
  }
  bool consume(std::uint64_t now) {
    if (!pending_ || now < at_) return false;
    pending_ = false;
    return now - at_ < 750;
  }
private:
  bool pending_ = false;
  std::uint64_t at_ = 0;
};
inline bool dashboardAssignmentVisible(bool complete, bool showOptional, bool bonusAvailable,
    const std::optional<AssignmentProgressChange> &change) {
  return !complete || (showOptional && bonusAvailable) || change.has_value();
}
inline std::optional<AssignmentProgressChange> progressChange(
    const std::optional<PlayProgress> &progress, const Progress &assignment) {
  if (!progress) return {};
  for (const auto &change : progress->assignments)
    if (change.id == assignment.requirement.id && change.afterRound == assignment.completedRounds + 1 &&
        change.afterCharts == assignment.charts && change.afterQuota == assignment.requirement.chartCount)
      return change;
  return {};
}
} // namespace kombo
