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

// Original rhythm spark. Geometry scales with its bounds; the one-shot bounce
// and celebration settle, so a dashboard does not keep requesting idle frames.
class RhythmSparkView final : public View {
public:
  explicit RhythmSparkView(bool celebrate = false, bool reduced = false) : celebrate_(celebrate) {
    setHeight(108)->setFlexShrink(0);
    entrance_.start(0, 1, SDL_GetTicks(), 650, 0, ui::motion::Ease::Pop, reduced);
    burst_.start(0, 1, SDL_GetTicks(), 1100, 0, ui::motion::Ease::Smooth, reduced);
  }
protected:
  void renderImpl(RenderContext &context) override {
    const auto now = SDL_GetTicks();
    if (entrance_.active(now) || (celebrate_ && burst_.active(now))) ui::requestAnimationFrame();
    const auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    const float cx = getX() + getWidth() * .5F, cy = getY() + getHeight() * .53F;
    const float radius = std::min(getWidth() * .22F, getHeight() * .34F) * (.7F + .3F * entrance_.value(now));
    auto triangle = [&](float ax, float ay, float bx, float by, float dx, float dy, Color color) {
      const std::array vertices{rendering::PosColorVertex{ax, ay, 0, color.toABGR()},
          rendering::PosColorVertex{bx, by, 0, color.toABGR()},
          rendering::PosColorVertex{dx, dy, 0, color.toABGR()}};
      constexpr std::array<std::uint16_t, 3> indices{0, 1, 2};
      context.appendUiColor(vertices, indices, state);
    };
    auto circle = [&](float x, float y, float r, Color color) {
      for (int i = 0; i < 24; ++i) {
        const float a = i * 6.2831853F / 24, b = (i + 1) * 6.2831853F / 24;
        triangle(x, y, x + std::cos(a) * r, y + std::sin(a) * r,
                 x + std::cos(b) * r, y + std::sin(b) * r, color);
      }
    };
    circle(cx, cy + radius * .5F, radius * .85F, app_theme::withAlpha(app_theme::amber(), 28));
    for (int i = 0; i < 10; ++i) {
      const float a = -1.5707963F + i * 6.2831853F / 10, b = a + 6.2831853F / 10;
      const float ar = radius * (i % 2 ? .62F : 1.16F), br = radius * (i % 2 ? 1.16F : .62F);
      triangle(cx, cy, cx + std::cos(a) * ar, cy + std::sin(a) * ar,
               cx + std::cos(b) * br, cy + std::sin(b) * br, Color(255, 176, 66));
    }
    circle(cx - radius * .23F, cy - radius * .07F, radius * .075F, app_theme::darkText());
    circle(cx + radius * .23F, cy - radius * .07F, radius * .075F, app_theme::darkText());
    triangle(cx - radius * .15F, cy + radius * .2F, cx + radius * .15F, cy + radius * .2F,
             cx, cy + radius * .34F, app_theme::darkText());
    if (celebrate_ && burst_.active(now)) {
      const float t = burst_.value(now);
      for (int i = 0; i < 12; ++i) {
        const float a = i * 6.2831853F / 12;
        const float distance = radius * (1.1F + t * 1.1F);
        circle(cx + std::cos(a) * distance, cy + std::sin(a) * distance,
               3 * (1 - t), i % 2 ? app_theme::lime() : app_theme::cyan());
      }
    }
  }
private:
  ui::motion::Track entrance_, burst_;
  bool celebrate_;
};
