#pragma once
#include "View.h"
#include "AppTheme.h"

// The same flame has three daily states. Only a played day requests motion.
class StreakFlameView final : public View {
public:
  explicit StreakFlameView(bool played = false, bool complete = false, bool reduced = false)
      : played_(played), complete_(complete), reduced_(reduced) {
    setHeight(108)->setFlexShrink(0);
  }
protected:
  void renderImpl(RenderContext &context) override {
    const bool animated = played_ && !reduced_;
    if (animated) ui::requestAnimationFrame();
    const float t = animated ? float(SDL_GetTicks() % 60000) * .001F : 0;
    const float energy = complete_ ? 1.0F : .4F;
    const float flicker = animated ? std::sin(t * 8) * .035F * energy : 0;
    const float sway = animated ? std::sin(t * 5) * .07F * energy : 0;
    const float size = std::min(getWidth() * .4F, getHeight() * .43F) *
        (complete_ ? 1 : .68F);
    const float cx = getX() + getWidth() * .5F;
    const float cy = getY() + getHeight() * .54F;
    const auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    struct Point { float x, y; };
    auto flame = [&](float scale, float offset, Color color) {
      std::vector<Point> outline{{0, .88F}};
      const auto curve = [&](Point a, Point b, Point end) {
        const auto start = outline.back();
        for (int step = 1; step <= 10; ++step) {
          const float u = float(step) / 10, v = 1-u;
          outline.push_back({v*v*v*start.x + 3*v*v*u*a.x + 3*v*u*u*b.x + u*u*u*end.x,
                             v*v*v*start.y + 3*v*v*u*a.y + 3*v*u*u*b.y + u*u*u*end.y});
        }
      };
      curve({-.85F,.82F}, {-.64F,.02F}, {-.35F,-.25F});
      curve({-.15F,-.46F}, {-.06F,-.74F}, {-.12F+sway,-1.0F-flicker});
      curve({.29F,-.72F}, {.38F,-.32F}, {.22F,-.04F});
      curve({.43F,-.12F}, {.49F,-.30F}, {.45F,-.48F});
      curve({.98F,.14F}, {.63F,.87F}, {0,.88F});
      const auto vertex = [&](Point p) {
        return rendering::PosColorVertex{cx+p.x*size*scale,
            cy+(p.y*scale+offset)*size, 0, color.toABGR()};
      };
      std::vector<rendering::PosColorVertex> vertices{vertex({0,.3F})};
      for (auto p : outline) vertices.push_back(vertex(p));
      std::vector<std::uint16_t> indices;
      for (std::uint16_t i = 1; i+1 < vertices.size(); ++i)
        indices.insert(indices.end(), {0,i,static_cast<std::uint16_t>(i+1)});
      context.appendUiColor(vertices, indices, state);
    };
    flame(1, 0, played_ ? Color(245,107,39) : app_theme::textSecondary());
    flame(.61F, .31F, played_ ? Color(255,192,66) : app_theme::hairlineStrong());
    if (complete_) {
      flame(.29F, .53F, Color(255,238,171));
      if (animated) for (int i = 0; i < 3; ++i) {
        const float phase = std::fmod(t * .65F + i / 3.0F, 1.0F);
        const float x = cx + (i-1) * size * .65F + std::sin(t*3+i)*size*.06F;
        const float y = cy + size * (.05F-phase*1.05F);
        const float r = size * .045F * (1-phase);
        const auto color = app_theme::withAlpha(Color(255,176,66), uint8_t(210*(1-phase))).toABGR();
        const std::array vertices{rendering::PosColorVertex{x,y-r,0,color},
            rendering::PosColorVertex{x+r,y,0,color}, rendering::PosColorVertex{x,y+r,0,color},
            rendering::PosColorVertex{x-r,y,0,color}};
        constexpr std::array<std::uint16_t,6> indices{0,1,2,0,2,3};
        context.appendUiColor(vertices, indices, state);
      }
    }
  }
private:
  bool played_, complete_, reduced_;
};
