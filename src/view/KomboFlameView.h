#pragma once
#include "View.h"
#include "AppTheme.h"
#include "../kombo/KomboIgnitionMotion.h"

// The same flame has three daily states. Only a played day requests motion.
class KomboFlameView final : public View {
public:
  explicit KomboFlameView(bool played = false, bool complete = false, bool reduced = false,
                           std::optional<std::uint64_t> ignitionStartedAt = {})
      : played_(played), complete_(complete), reduced_(reduced), ignitionStartedAt_(ignitionStartedAt) {
    setHeight(108)->setFlexShrink(0);
  }
protected:
  void renderImpl(RenderContext &context) override {
    const bool animated = played_ && !reduced_;
    if (animated) ui::requestAnimationFrame();
    const auto ignition = kombo::ignitionFrame(complete_ ? ignitionStartedAt_ : std::nullopt,
                                                SDL_GetTicks(), reduced_);
    const float t = animated ? float(SDL_GetTicks() % 60000) * .001F : 0;
    const float energy = complete_ ? 1.0F : .4F;
    const float flicker = animated ? std::sin(t * 8) * .035F * energy : 0;
    const float sway = animated ? std::sin(t * 5) * .07F * energy : 0;
    const float size = std::min(getWidth() * .4F, getHeight() * .43F) *
        (complete_ ? 1 : .68F) * ignition.scale;
    const float cx = getX() + getWidth() * .5F;
    const float cy = getY() + getHeight() * .54F;
    const auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    auto diamond = [&](float x, float y, float r, Color tint) {
      const auto color = tint.toABGR();
      const std::array vertices{rendering::PosColorVertex{x,y-r,0,color},
          rendering::PosColorVertex{x+r,y,0,color}, rendering::PosColorVertex{x,y+r,0,color},
          rendering::PosColorVertex{x-r,y,0,color}};
      constexpr std::array<std::uint16_t,6> indices{0,1,2,0,2,3};
      context.appendUiColor(vertices, indices, state);
    };
    if (ignition.glow > 0) {
      for (int ring = 3; ring > 0; --ring) {
        const float radius = size * (.65F + ring*.14F);
        const auto color = app_theme::withAlpha(Color(255,151,45),
            uint8_t(ignition.glow * (ring == 1 ? 30 : 12))).toABGR();
        std::vector<rendering::PosColorVertex> vertices{{cx,cy,0,color}};
        std::vector<std::uint16_t> indices;
        for (int i = 0; i <= 32; ++i) {
          const float angle = i * 6.2831853F / 32;
          vertices.push_back({cx+std::cos(angle)*radius, cy+std::sin(angle)*radius*.75F,0,color});
          if (i > 0) indices.insert(indices.end(), {0, static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(i+1)});
        }
        context.appendUiColor(vertices, indices, state);
      }
    }
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
        return rendering::PosColorVertex{cx+p.x*size*scale*ignition.width,
            cy+(p.y*scale+offset)*size*ignition.height, 0, color.toABGR()};
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
        diamond(x, y, r, app_theme::withAlpha(Color(255,176,66), uint8_t(210*(1-phase))));
      }
    }
    if (ignition.active && ignition.burst > 0 && ignition.burst < 1) {
      const float p = ignition.burst;
      const float radius = std::min(getWidth()*.4F, getHeight()*.43F);
      for (int i = 0; i < 9; ++i) {
        const float angle = -2.85F + i * 2.55F / 8;
        const float travel = .3F + p*(i%2 ? 1.0F : .75F);
        diamond(cx+std::cos(angle)*radius*travel,
                cy+std::sin(angle)*radius*travel + p*p*radius*.15F,
                radius*.055F*(1-p), app_theme::withAlpha(Color(255,190,75), uint8_t(240*(1-p))));
      }
    }
  }
private:
  bool played_, complete_, reduced_;
  std::optional<std::uint64_t> ignitionStartedAt_;
};
