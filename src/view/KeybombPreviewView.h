#pragma once

#include "View.h"
#include "UiTheme.h"
#include "../scene/play/KeybombEffect.h"

// Uses the gameplay geometry for both samples, so appearance edits and animation
// changes are previewed without loading a chart or publishing gameplay events.
class KeybombPreviewView final : public View {
public:
  KeybombPreviewView(Judgement judgement, std::function<keybomb::Style()> style)
      : judgement(judgement), style(std::move(style)) {
    setBackgroundColor(Color(18, 22, 30));
    setCornerRadius(0);
  }

  void renderAt(RenderContext &context, long long now) {
    ScissorScope clip(context, getX(), getY(), getWidth(), getHeight());
    auto appearance = keybomb::sanitize(style());
    appearance.color = keybomb::resolvedColor(judgement, appearance,
        ui_theme::activeMode() == ui_theme::ThemeMode::Light);
    // Frame large custom bursts without clipping or overlapping the hold sample.
    // Keeping the lane guide at the same scale makes relative size visible.
    const float size = appearance.sizePercent / 100.0F;
    const float laneWidth = std::min({64.0F, getWidth() * 0.22F / (1.5F * size),
                                     getHeight() * 0.44F / (1.5F * size)});
    if (laneWidth <= 0) return;
    const auto program = rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    const auto state = context.makeUiBatchState(program,
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    for (int sample = 0; sample < 2; ++sample) {
      Batch batch{context, state, getX() + getWidth() * (sample ? 0.75F : 0.25F),
                  getY() + getHeight() * 0.5F};
      batch.addLine(0, -getHeight() * 0.42F, 0, getHeight() * 0.42F,
                    laneWidth, 0xFF302820);
      batch.addLine(-laneWidth * 1.5F, 0, laneWidth * 1.5F, 0, 2, 0xB0FFFFFF);
      if (sample == 0) {
        const auto age = now % 800'000;
        keybomb::draw(batch, {judgement, now - age}, now, 0, 0, laneWidth, appearance);
      } else {
        keybomb::drawHold(batch, judgement, now, 0, 0, laneWidth, appearance);
      }
    }
  }

protected:
  void renderImpl(RenderContext &context) override {
    renderAt(context, static_cast<long long>(SDL_GetTicksNS() / 1000));
  }

private:
  struct Batch {
    RenderContext &context;
    rendering::UiBatchState state;
    float originX, originY;

    void addTriangleColors(float x, float y, float a, float b, float d, float e,
                           std::uint32_t first, std::uint32_t second, std::uint32_t third) {
      const std::array vertices{
          rendering::PosColorVertex{originX + x, originY - y, 0, first},
          rendering::PosColorVertex{originX + a, originY - b, 0, second},
          rendering::PosColorVertex{originX + d, originY - e, 0, third}};
      constexpr std::array<std::uint16_t, 3> indices{0, 1, 2};
      context.appendUiColor(vertices, indices, state);
    }
    void addTriangle(float x, float y, float a, float b, float d, float e, std::uint32_t color) {
      addTriangleColors(x, y, a, b, d, e, color, color, color);
    }
    void addLine(float x, float y, float a, float b, float width, std::uint32_t color) {
      const float length = std::hypot(a - x, b - y);
      if (length <= 0) return;
      const float dx = (b - y) / length * width * 0.5F;
      const float dy = (x - a) / length * width * 0.5F;
      addTriangle(x + dx, y + dy, a + dx, b + dy, a - dx, b - dy, color);
      addTriangle(a - dx, b - dy, x - dx, y - dy, x + dx, y + dy, color);
    }
  };
  Judgement judgement;
  std::function<keybomb::Style()> style;
};
