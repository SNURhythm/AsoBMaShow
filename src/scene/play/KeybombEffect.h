#pragma once

#include "LaneEffects.h"
#include "../../settings/KeybombSettings.h"
#include <cmath>
#include <numbers>

namespace keybomb {
// Stateless, bounded geometry: only the renderer evaluates the animation.
// The newest judgement replaces the previous burst on that lane, keeping dense
// jacks bounded without queues or particle objects in the input path.
template <typename Batch>
void draw(Batch &batch, LaneKeybombEvent event, long long now,
          float x, float y, float laneWidth, Style style) {
  if (!valid(event.judgement) || now < event.timeMicros || laneWidth <= 0) return;
  style = sanitize(style);
  if (!style.visible || style.opacityPercent == 0) return;
  const bool perfect = event.judgement == PGreat;
  const bool great = event.judgement == Great;
  const bool good = event.judgement == Good;
  const bool hit = perfect || great || good;
  const long long duration = perfect ? 380000 : great ? 310000 : good ? 240000 : 200000;
  const auto elapsed = now - event.timeMicros;
  if (elapsed >= duration) return;
  const float t = static_cast<float>(elapsed) / static_cast<float>(duration);
  const float fade = (1 - t) * (1 - t);
  const float expansion = 1 - (1 - t) * (1 - t) * (1 - t);
  const float radius = laneWidth * style.sizePercent / 100.0F *
      (perfect ? 0.82F : great ? 0.70F : good ? 0.52F : 0.42F);
  const float opacity = style.opacityPercent / 100.0F;
  // ABGR: ice-blue prism, gold, mint, coral, muted red, violet.
  const std::uint32_t rgb = perfect ? 0x00FFE8A0 : great ? 0x006EDCFF :
      good ? 0x00B2E39C : event.judgement == Bad ? 0x007892F0 :
      event.judgement == Poor ? 0x00626CC2 : 0x00BE87A1;
  const auto color = [&](std::uint32_t base, float alpha) {
    return base | (static_cast<std::uint32_t>(std::clamp(alpha * opacity, 0.0F, 255.0F)) << 24);
  };
  const float ringRadius = radius * (0.18F + expansion * 0.82F);
  // World-space triangle fans avoid the pixel-sized rounding threshold in UI
  // circles. A soft center-to-edge gradient uses fixed geometry at every size.
  const auto glow = [&](float r, std::uint32_t rgbColor, float alpha) {
    constexpr int segments = 32;
    for (int i = 0; i < segments; ++i) {
      const float a = 2 * std::numbers::pi_v<float> * i / segments;
      const float b = 2 * std::numbers::pi_v<float> * (i + 1) / segments;
      batch.addTriangleColors(x, y, x + std::cos(a) * r, y + std::sin(a) * r,
                              x + std::cos(b) * r, y + std::sin(b) * r,
                              color(rgbColor, alpha), rgbColor, rgbColor);
    }
  };
  glow(radius * 0.7F * (1 - t * 0.65F), rgb, (hit ? 180 : 65) * fade);
  const auto ring = [&](float r, float alpha, bool broken) {
    constexpr int segments = 40;
    for (int i = 0; i < segments; ++i) {
      if (broken && i % 10 >= 6) continue;
      const float a = 2 * std::numbers::pi_v<float> * i / segments;
      const float b = 2 * std::numbers::pi_v<float> * (i + 1) / segments;
      batch.addLine(x + std::cos(a) * r, y + std::sin(a) * r,
                    x + std::cos(b) * r, y + std::sin(b) * r,
                    radius * 0.025F * (1 - t * 0.6F), color(rgb, alpha * fade));
    }
  };
  if (hit || event.judgement == Kpoor) ring(ringRadius, hit ? 180 : 75, !good);
  if (perfect) ring(ringRadius * 0.73F, 100, false);
  if (perfect || great) {
    const int sparks = perfect ? 8 : 6;
    for (int i = 0; i < sparks; ++i) {
      const float a = 2 * std::numbers::pi_v<float> * i / sparks + 0.23F;
      const float dx = std::cos(a), dy = std::sin(a);
      const float r = radius * (0.28F + expansion * (i % 2 ? 0.80F : 1.15F));
      const float length = radius * (i % 2 ? 0.14F : 0.24F) * (1 - t);
      const float width = radius * 0.035F * (1 - t);
      const float cx = x + dx * r, cy = y + dy * r;
      const auto c = color(i % 2 ? rgb : 0x00FFFFFF, 230 * fade);
      batch.addTriangle(cx + dx * length, cy + dy * length,
                        cx - dy * width, cy + dx * width,
                        cx + dy * width, cy - dx * width, c);
      batch.addTriangle(cx - dx * length, cy - dy * length,
                        cx - dy * width, cy + dx * width,
                        cx + dy * width, cy - dx * width, c);
    }
  } else if (!hit && event.judgement != Kpoor) {
    const bool miss = event.judgement == Poor;
    for (int i = 0; i < 5; ++i) {
      const float dx = (i - 2) * radius * 0.35F * (0.3F + expansion);
      const float dy = radius * (miss ? -1 : 1) * expansion * (i % 2 ? 0.5F : 0.9F);
      batch.addLine(x + dx, y + dy, x + dx * 1.2F,
                    y + dy + radius * (miss ? -0.15F : 0.15F),
                    radius * 0.045F, color(rgb, 150 * fade));
    }
  }
  if (hit) {
    const float core = radius * 0.15F * (1 - t);
    glow(core * 1.6F, 0x00FFFFFF, 255 * fade);
    if (perfect) {
      batch.addLine(x - core * 2.5F, y, x + core * 2.5F, y,
                    core * 0.35F, color(0x00FFFFFF, 200 * fade));
      batch.addLine(x, y - core * 2.5F, x, y + core * 2.5F,
                    core * 0.35F, color(0x00FFFFFF, 200 * fade));
    }
  }
}
} // namespace keybomb
