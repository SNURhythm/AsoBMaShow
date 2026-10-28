#pragma once
#include "View.h"
#include "TextView.h"
#include "IconText.h"
#include "AppTheme.h"
#include "../goals/GoalResultPresentation.h"

// A finite, non-interactive cue; it never captures result controls.
class GoalAchievementView final : public View {
public:
  GoalAchievementView(std::uint64_t startedAt, const std::string &chart,
                      const std::string &target, float unit)
      : startedAt_(startedAt) {
    setName("goalResultAchievement");
    setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignCenter);
    setPadding(Edge::All, 16 * unit)->setGap(4 * unit)->setCornerRadius(16 * unit);
    setThemedBackgroundColor(app_theme::panel);
    setThemedBorderColor(app_theme::amber); setBorderWidth(2 * unit);
    auto *heading = new View();
    heading->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(10 * unit);
    auto *flag = new TextView(ui_icons::kFontAwesomeSolidPath, 27 * unit);
    flag->setText(ui_icons::textForCodepoint(0xf024));
    flag->setThemedColor(app_theme::amber); flag->setHeight(38 * unit);
    heading->addView(flag);
    auto *caption = new TextView(app_theme::font, 27 * unit);
    caption->setLocalizedText(i18n::message("goals.ui.congratulations"));
    caption->setThemedColor(app_theme::amber); caption->setHeight(38 * unit);
    heading->addView(caption); addView(heading);
    for (const auto &[text, size] : std::array{std::pair{chart, 21}, std::pair{target, 17}}) {
      auto *line = new TextView(app_theme::font, size * unit);
      line->setText(text); line->setWrap(false); line->setAlign(TextView::CENTER);
      line->setOverflow(TextView::TextOverflow::Marquee);
      line->setWidthPercent(100)->setHeight((size + 8) * unit)->setFlexShrink(0);
      line->setThemedColor(app_theme::textPrimary); addView(line);
    }
  }
protected:
  float renderOpacity() const noexcept override {
    return GoalResultPresentation::opacity(startedAt_, SDL_GetTicks());
  }
  void renderImpl(RenderContext &context) override {
    const auto now = SDL_GetTicks();
    const float elapsed = now >= startedAt_ ? float(now - startedAt_) : 0;
    if (elapsed < GoalResultPresentation::duration) ui::requestAnimationFrame();
    const float t = std::clamp(elapsed / 1500, 0.0F, 1.0F);
    if (t == 0 || t == 1) return;
    const auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    constexpr std::array<std::uint16_t, 6> indices{0,1,2,0,2,3};
    for (int i = 0; i < 24; ++i) {
      const float angle = i * 6.2831853F / 24;
      const float travel = (1 - (1-t)*(1-t)) * (.65F + (i%3)*.12F);
      const float x = getX() + getWidth() * (.5F + std::cos(angle)*travel*.48F);
      const float y = getY() + getHeight() * (.45F + std::sin(angle)*travel*.35F + t*t*.12F);
      const float radius = std::max(2.0F, getWidth() * .009F) * (1-t*.6F);
      const auto color = app_theme::withAlpha(i%2 ? app_theme::amber() : app_theme::lime(),
          static_cast<std::uint8_t>(230*(1-t))).toABGR();
      const std::array vertices{rendering::PosColorVertex{x,y-radius,0,color},
          rendering::PosColorVertex{x+radius,y,0,color}, rendering::PosColorVertex{x,y+radius,0,color},
          rendering::PosColorVertex{x-radius,y,0,color}};
      context.appendUiColor(vertices, indices, state);
    }
  }
private:
  std::uint64_t startedAt_;
};
