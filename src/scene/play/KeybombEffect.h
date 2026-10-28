#pragma once

#include "LaneEffects.h"
#include "../../settings/KeybombSettings.h"
#include "../../rendering/UnitCircleSamples.h"
#include <cmath>
#include <numbers>

namespace keybomb {
template <int Sparks>
const std::array<rendering::UnitCircleSample, Sparks> &burstSparkDirections() {
  static const auto samples = [] {
    std::array<rendering::UnitCircleSample, Sparks> result{};
    for (int i = 0; i < Sparks; ++i) {
      const float angle = 2 * std::numbers::pi_v<float> * i / Sparks + 0.23F;
      result[i] = {std::cos(angle), std::sin(angle)};
    }
    return result;
  }();
  return samples;
}

// Stateless, bounded geometry: only the renderer evaluates the animation.
// The newest judgement replaces the previous burst on that lane, keeping dense
// jacks bounded without queues or particle objects in the input path.
template <typename Batch>
void draw(Batch &batch, LaneKeybombEvent event, long long now,
          float x, float y, float laneWidth, Style style) {
  if (!valid(event.judgement) || event.judgement == Poor ||
      now < event.timeMicros || laneWidth <= 0) return;
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
  // Keep successful hits legible after the first frame, then fade smoothly.
  const float fade = hit ? 1 - t * t : (1 - t) * (1 - t);
  const float expansion = 1 - (1 - t) * (1 - t) * (1 - t);
  const float radius = laneWidth * style.sizePercent / 100.0F *
      (perfect ? 0.92F : great ? 0.80F : good ? 0.64F : 0.42F);
  const float opacity = style.opacityPercent / 100.0F;
  const std::uint32_t rgb = effectColor(event.judgement, style);
  const auto color = [&](std::uint32_t base, float alpha) {
    return base | (static_cast<std::uint32_t>(std::clamp(alpha * opacity, 0.0F, 255.0F)) << 24);
  };
  const float ringRadius = radius * (0.18F + expansion * 0.82F);
  // World-space triangle fans avoid the pixel-sized rounding threshold in UI
  // circles. A soft center-to-edge gradient uses fixed geometry at every size.
  const auto glow = [&](float r, std::uint32_t rgbColor, float alpha) {
    const auto center = color(rgbColor, alpha);
    if ((center >> 24) == 0) return;
    batch.addRadialGradientCircle(x, y, r, center, rgbColor);
  };
  glow(radius * (hit ? 0.9F : 0.7F) * (1 - t * 0.45F), rgb, (hit ? 240 : 65) * fade);
  const auto ring = [&](float r, float alpha, bool broken) {
    const auto ringColor = color(rgb, alpha * fade);
    if ((ringColor >> 24) == 0) return;
    constexpr int segments = 40;
    const auto &samples = rendering::unitCircleSamples<segments>();
    for (int i = 0; i < segments; ++i) {
      if (broken && i % 10 >= 6) continue;
      const auto a = samples[i], b = samples[i + 1];
      batch.addLine(x + a.x * r, y + a.y * r,
                    x + b.x * r, y + b.y * r,
                    radius * (hit ? 0.055F : 0.025F) * (1 - t * 0.6F), ringColor);
    }
  };
  if (hit || event.judgement == Kpoor) ring(ringRadius, hit ? 230 : 75, !good);
  if (perfect) ring(ringRadius * 0.73F, 160, false);
  if (perfect || great) {
    const int sparks = perfect ? 8 : 6;
    for (int i = 0; i < sparks; ++i) {
      const auto c = color(i % 2 ? rgb : 0x00FFFFFF, 230 * fade);
      if ((c >> 24) == 0) continue;
      const auto direction = perfect ? burstSparkDirections<8>()[i]
                                     : burstSparkDirections<6>()[i];
      const float dx = direction.x, dy = direction.y;
      const float r = radius * (0.28F + expansion * (i % 2 ? 0.80F : 1.15F));
      const float length = radius * (i % 2 ? 0.18F : 0.28F) * (1 - t);
      const float width = radius * 0.055F * (1 - t);
      const float cx = x + dx * r, cy = y + dy * r;
      batch.addTriangle(cx + dx * length, cy + dy * length,
                        cx - dy * width, cy + dx * width,
                        cx + dy * width, cy - dx * width, c);
      batch.addTriangle(cx - dx * length, cy - dy * length,
                        cx - dy * width, cy + dx * width,
                        cx + dy * width, cy - dx * width, c);
    }
  } else if (!hit && event.judgement != Kpoor) {
    const auto fragmentColor = color(rgb, 150 * fade);
    for (int i = 0; i < 5 && (fragmentColor >> 24) != 0; ++i) {
      const float dx = (i - 2) * radius * 0.35F * (0.3F + expansion);
      const float dy = radius * expansion * (i % 2 ? 0.5F : 0.9F);
      batch.addLine(x + dx, y + dy, x + dx * 1.2F,
                    y + dy + radius * 0.15F,
                    radius * 0.045F, fragmentColor);
    }
  }
  if (hit) {
    const float core = radius * 0.23F * (1 - t);
    glow(core * 1.6F, 0x00FFFFFF, 255 * fade);
    const auto crossColor = color(0x00FFFFFF, 200 * fade);
    if (perfect && (crossColor >> 24) != 0) {
      batch.addLine(x - core * 2.5F, y, x + core * 2.5F, y,
                    core * 0.35F, crossColor);
      batch.addLine(x, y - core * 2.5F, x, y + core * 2.5F,
                    core * 0.35F, crossColor);
    }
  }
}
// A continuous emitter, evaluated only while the renderer sees an active hold.
// Staggered particle ages keep sparks moving without creating input-side events.
template <typename Batch>
void drawHold(Batch &batch, Judgement judgement, long long now,
              float x, float y, float laneWidth, Style style) {
  style = sanitize(style);
  if (!valid(judgement) || judgement == Poor || !style.visible || style.opacityPercent == 0 || laneWidth <= 0) return;
  const auto rgb = effectColor(judgement, style);
  const float opacity = style.opacityPercent / 100.0F;
  const float radius = laneWidth * style.sizePercent / 100.0F * 0.65F;
  // Reduce before float conversion to preserve motion throughout long sessions.
  const float phase = static_cast<float>((now % 2'400'000 + 2'400'000) % 2'400'000) / 2'400'000.0F;
  const float angle = phase * 2 * std::numbers::pi_v<float>;
  const float pulse = 0.75F + 0.25F * std::sin(angle * 4);
  const auto color = [&](float alpha) {
    return rgb | (static_cast<std::uint32_t>(std::clamp(alpha * opacity, 0.0F, 255.0F)) << 24);
  };
  const float glowRadius = radius * (0.36F + pulse * 0.12F);
  const auto glowColor = color(140 * pulse);
  if ((glowColor >> 24) != 0)
    batch.addRadialGradientCircle(x, y, glowRadius, glowColor, rgb);
  for (int i = 0; i < 8; ++i) {
    const float age = std::fmod(phase * 4 + i / 8.0F, 1.0F);
    const auto c = color(230 * std::sin(age * std::numbers::pi_v<float>));
    if ((c >> 24) == 0) continue;
    const float a = 2 * std::numbers::pi_v<float> * i / 8 + angle;
    const float dx = std::cos(a), dy = std::sin(a);
    const float distance = radius * (0.15F + age * 1.15F);
    const float cx = x + dx * distance;
    const float cy = y + dy * distance + radius * age * age * 0.5F;
    const float length = radius * (0.05F + (1 - age) * 0.10F);
    const float width = radius * 0.035F * (1 - age);
    batch.addTriangle(cx + dx * length, cy + dy * length,
                      cx - dy * width, cy + dx * width,
                      cx + dy * width, cy - dx * width, c);
    batch.addTriangle(cx - dx * length, cy - dy * length,
                      cx - dy * width, cy + dx * width,
                      cx + dy * width, cy - dx * width, c);
  }
}
} // namespace keybomb
