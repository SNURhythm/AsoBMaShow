#pragma once
#include "ProgressMotionView.h"
#include "TextView.h"
#include "../streak/StreakRoundMotion.h"
#include "../i18n/Localization.h"

// The animation clock belongs to the scene, so rebuilding a view cannot replay it.
class AssignmentProgressView final : public View {
  class RoundLabel final : public TextView {
  public:
    explicit RoundLabel(int size) : TextView(app_theme::boldFont, size) {}
    float opacity = 1;
  protected:
    float renderOpacity() const noexcept override { return opacity; }
  };
public:
  AssignmentProgressView(const streak::Progress &progress,
      std::optional<streak::AssignmentProgressChange> change, std::uint64_t startedAt, float unit = 1)
      : motion_(change.value_or(streak::AssignmentProgressChange{
          .id=progress.requirement.id, .beforeCharts=progress.charts,
          .beforeQuota=progress.requirement.chartCount, .beforeRound=progress.completedRounds+1,
          .afterCharts=progress.charts, .afterQuota=progress.requirement.chartCount,
          .afterRound=progress.completedRounds+1}), startedAt, !change.has_value()) {
    setFlexDirection(FlexDirection::Column)->setGap(8*unit)->setFlexShrink(0);
    round_ = new RoundLabel(static_cast<int>(std::lround(21*unit)));
    round_->setMinHeight(34*unit)->setFlexShrink(0);
    round_->setThemedColor(app_theme::textPrimary);
    addView(round_);
    count_ = new TextView(app_theme::font, static_cast<int>(std::lround(20*unit)));
    count_->setMinHeight(30*unit)->setFlexShrink(0);
    count_->setWrap(true);
    count_->setThemedColor(app_theme::textSecondary);
    addView(count_);
    meter_ = new ProgressMotionView(0, false, true);
    meter_->setHeight(18*unit);
    addView(meter_);
    refresh(SDL_GetTicks());
  }
protected:
  void renderImpl(RenderContext &) override {
    if (refresh(SDL_GetTicks())) ui::requestAnimationFrame();
  }
private:
  bool refresh(std::uint64_t now) {
    const auto frame = motion_.frame(now);
    const auto title = i18n::format(frame.completed ? "streak.ui.round_complete" : "streak.ui.round_number",
        {{"count", std::to_string(frame.round)}});
    if (title != title_) { title_=title; round_->setText(title); }
    auto count = std::to_string(frame.charts) + " / " + std::to_string(frame.quota);
    if (!frame.completed)
      count += " · " + i18n::plural("streak.ui.remaining_charts", std::max(0, frame.quota - frame.charts)).resolve();
    if (count != countText_) { countText_=count; count_->setText(count); }
    if (frame.completed != completed_) {
      completed_=frame.completed;
      round_->setThemedColor(completed_ ? app_theme::amber : app_theme::textPrimary);
    }
    round_->opacity = .25F + .75F*frame.promotion;
    round_->setRotationDegrees(-2.5F*(1-frame.promotion));
    meter_->showFraction(frame.fraction, frame.completed);
    return frame.active;
  }
  streak::RoundMotion motion_;
  RoundLabel *round_ = nullptr;
  TextView *count_ = nullptr;
  ProgressMotionView *meter_ = nullptr;
  std::string title_, countText_;
  bool completed_ = false;
};
