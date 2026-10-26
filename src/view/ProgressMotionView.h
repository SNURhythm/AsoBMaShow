#pragma once
#include "View.h"
#include "AppTheme.h"
#include "../ui/Motion.h"

// Reusable finite progress animation. No timers or callbacks outlive the view.
class ProgressMotionView final : public View {
public:
  explicit ProgressMotionView(float fraction, bool celebrate = false, bool reduced = false)
      : celebrate_(celebrate) {
    setHeight(18)->setFlexShrink(0);
    fill_.start(0, std::clamp(fraction, 0.0F, 1.0F), SDL_GetTicks(), 850, 80,
                ui::motion::Ease::Smooth, reduced);
  }
  void showFraction(float fraction, bool celebrate = false) {
    fill_ = ui::motion::Track(std::clamp(fraction, 0.0F, 1.0F));
    celebrate_ = celebrate;
  }
protected:
  void renderImpl(RenderContext &context) override {
    const auto now = SDL_GetTicks();
    if (fill_.active(now)) ui::requestAnimationFrame();
    const auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    auto pill = [&](float width, Color color) {
      const float h = getHeight(), r = std::min(h * .5F, width * .5F);
      if (width <= 0 || h <= 0) return;
      std::vector<rendering::PosColorVertex> vertices;
      std::vector<std::uint16_t> indices;
      vertices.push_back({getX() + width * .5F, getY() + h * .5F, 0, color.toABGR()});
      for (int end = 0; end < 2; ++end) for (int i = 0; i <= 12; ++i) {
        const float angle = (end ? .5F : -.5F) * 3.14159265F + i * 3.14159265F / 12;
        vertices.push_back({getX() + (end ? r : width - r) + std::cos(angle) * r,
                            getY() + h * .5F + std::sin(angle) * h * .5F, 0, color.toABGR()});
      }
      for (std::uint16_t i = 1; i < vertices.size(); ++i)
        indices.insert(indices.end(), {0, i, static_cast<std::uint16_t>(i + 1 == vertices.size() ? 1 : i + 1)});
      context.appendUiColor(vertices, indices, state);
    };
    pill(getWidth(), app_theme::insetSurface());
    pill(getWidth() * fill_.value(now), celebrate_ ? app_theme::amber() : app_theme::lime());
  }
private:
  ui::motion::Track fill_;
  bool celebrate_;
};
