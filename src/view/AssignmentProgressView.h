#pragma once
#include "ProgressMotionView.h"
#include "TextView.h"
#include "../kombo/KomboRoundMotion.h"
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
  AssignmentProgressView(const kombo::Progress &progress,
      std::optional<kombo::AssignmentProgressChange> change, std::uint64_t startedAt, float unit = 1)
      : motion_(change.value_or(kombo::AssignmentProgressChange{
          .id=progress.requirement.id, .beforeCharts=progress.charts,
          .beforeQuota=progress.requirement.chartCount, .beforeRound=progress.completedRounds+1,
          .afterCharts=progress.charts, .afterQuota=progress.requirement.chartCount,
          .afterRound=progress.completedRounds+1}), startedAt, !change.has_value()) {
    goals_ = kombo::isGoalsAssignment(progress.requirement);
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
    const auto title = i18n::format(frame.completed ? "kombo.ui.round_complete" : "kombo.ui.round_number",
        {{"count", std::to_string(frame.round)}});
    if (title != title_) { title_=title; round_->setText(title); }
    auto count = std::to_string(frame.charts) + " / " + std::to_string(frame.quota);
    if (!frame.completed) {
      const auto remaining = std::max(0, frame.quota - frame.charts);
      count += " · " + (goals_ ? i18n::plural("kombo.ui.remaining_goals", remaining)
          : i18n::plural("kombo.ui.remaining_charts", remaining)).resolve();
    }
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
  kombo::RoundMotion motion_;
  RoundLabel *round_ = nullptr;
  TextView *count_ = nullptr;
  ProgressMotionView *meter_ = nullptr;
  std::string title_, countText_;
  bool completed_ = false;
  bool goals_ = false;
};
