#pragma once

#include "CompletionCelebration.h"
#include "../../rendering/UnitCircleSamples.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>
#include <utility>

// Native handoff of tools/completion-motion/src/frame.mjs, Rise direction.
// Coordinates are local to the title's center in the 1280x720 design space.
// Fixed geometry and analytic time keep live/replay/export frames identical.
namespace completion_celebration {
inline float clamp(float value) { return std::clamp(value, 0.0F, 1.0F); }
inline float out(float time, float at, float duration) {
  const float q = 1 - clamp((time - at) / duration);
  return 1 - q * q * q;
}
inline float smooth(float value) {
  const float q = clamp(value);
  return q * q * (3 - 2 * q);
}
inline bool visible(Achievement achievement, float age) {
  return achievement != Achievement::None && age >= 0 &&
      age < (achievement == Achievement::Finish ? 1.9F : achievement == Achievement::Perfect ? 2.8F : 2.5F);
}
inline std::string_view word(Achievement achievement) {
  return achievement == Achievement::Finish ? "FINISH" : achievement == Achievement::Perfect ? "PERFECT" : "FULL COMBO";
}
inline int fontSize(Achievement achievement) {
  return achievement == Achievement::Finish ? 62 : achievement == Achievement::Perfect ? 82 : 74;
}
inline int glyphOffset(Achievement achievement) {
  return achievement == Achievement::Finish ? 17 : achievement == Achievement::Perfect ? 10 : 0;
}
struct Letter {
  float x = 0, y = 0, scale = 1, alpha = 0;
};
inline Letter letter(float time, Achievement achievement, std::size_t index) {
  if (achievement == Achievement::Finish) {
    const float entrance = smooth(time / .32F);
    return {-110 + 44 * static_cast<float>(index), 6 * (1 - entrance), 1,
            entrance * (1 - smooth((time - 1.45F) / .45F))};
  }
  const bool perfect = achievement == Achievement::Perfect;
  const auto label = word(perfect ? Achievement::Perfect : Achievement::FullCombo);
  const float stagger = perfect ? 0 : std::abs(static_cast<float>(index) - 4.5F) * .009F;
  const float age = time - .08F - stagger;
  const float progress = out(age, 0, perfect ? .13F : .23F);
  const float settle = perfect ? 0 :
      std::sin(clamp(age / .48F) * std::numbers::pi_v<float>) * std::exp(-std::max(age, 0.0F) * 5);
  const float advance = perfect ? 58 : 52;
  float x = -(perfect ? 406.0F : 494.0F) * .5F;
  for (std::size_t i = 0; i < index; ++i) x += label[i] == ' ' ? 26 : advance;
  x += advance * .5F;
  return {x, perfect ? 0 : 30 * (1 - progress) - 5 * settle,
          perfect ? 1 + .16F * (1 - out(age, 0, .28F)) : 1 + .1F * settle,
          age <= 0 ? 0 : progress * (1 - smooth((time - (perfect ? 2.35F : 2.05F)) / .4F))};
}

inline constexpr std::uint32_t kWhite = 0x00FFFDF8;
inline constexpr std::uint32_t kTeal = 0x00CAD256;
inline constexpr std::uint32_t kLime = 0x005CF6B7;
inline constexpr std::uint32_t kGold = 0x0051CCFF;
inline std::uint32_t color(std::uint32_t rgb, float alpha) {
  return rgb | (static_cast<std::uint32_t>(clamp(alpha) * 255) << 24);
}

template <typename Batch>
void draw(Batch &batch, float time, Achievement achievement,
          float centerX, float centerY, float scale) {
  if (!visible(achievement, time) || scale <= 0) return;
  const bool perfect = achievement == Achievement::Perfect;
  const auto accent = perfect ? kGold : kLime;
  const auto triangle = [&](float ax, float ay, float bx, float by,
                            float cx, float cy, std::uint32_t rgb, float alpha) {
    if (alpha <= 0) return;
    batch.addTriangle(centerX + ax * scale, centerY + ay * scale,
                      centerX + bx * scale, centerY + by * scale,
                      centerX + cx * scale, centerY + cy * scale, color(rgb, alpha));
  };
  const auto line = [&](float ax, float ay, float bx, float by,
                        float width, std::uint32_t rgb, float alpha) {
    if (alpha <= 0 || width <= 0) return;
    batch.addLine(centerX + ax * scale, centerY + ay * scale,
                  centerX + bx * scale, centerY + by * scale,
                  width * scale, color(rgb, alpha));
  };
  const auto shard = [&](float x, float y, float length, float width,
                         float angle, std::uint32_t rgb, float alpha) {
    const float dx = std::cos(angle), dy = std::sin(angle);
    triangle(x - dx * length, y - dy * length, x - dy * width, y + dx * width,
             x + dx * length, y + dy * length, rgb, alpha);
    triangle(x - dx * length, y - dy * length, x + dy * width, y - dx * width,
             x + dx * length, y + dy * length, rgb, alpha);
  };
  constexpr int segments = 64;
  const auto &circle = rendering::unitCircleSamples<segments>();
  if (achievement == Achievement::Finish) {
    const float opacity = smooth(time / .32F) * (1 - smooth((time - 1.45F) / .45F));
    for (int i = 0; opacity > 0 && i < segments; ++i) {
      const auto a = circle[i], b = circle[i + 1];
      const float ax = centerX + a.x * 209 * scale, ay = centerY + a.y * 66 * scale;
      const float bx = centerX + b.x * 209 * scale, by = centerY + b.y * 66 * scale;
      const float cx = centerX + a.x * 380 * scale, cy = centerY + a.y * 120 * scale;
      const float dx = centerX + b.x * 380 * scale, dy = centerY + b.y * 120 * scale;
      const auto dim = color(0, opacity * .55F);
      batch.addTriangleColors(centerX, centerY, ax, ay, bx, by, dim, dim, dim);
      batch.addTriangleColors(ax, ay, cx, cy, dx, dy, dim, 0, 0);
      batch.addTriangleColors(ax, ay, dx, dy, bx, by, dim, 0, dim);
    }
    // Submit the glow after the whole dim layer to avoid overlapping its rim.
    for (int i = 0; opacity > 0 && i < segments; ++i)
      batch.addTriangleColors(centerX, centerY,
          centerX + circle[i].x * 260 * scale, centerY + circle[i].y * 65 * scale,
          centerX + circle[i + 1].x * 260 * scale, centerY + circle[i + 1].y * 65 * scale,
          color(0x00E1D9CD, opacity * .07F), 0, 0);
    return;
  }
  // A quiet, feathered scrim keeps stacked HUD text from competing with the
  // achievement. Hold it through the lettering's exit, including Perfect's mark.
  const float dim = .68F * out(time, 0, .16F) *
      (1 - smooth((time - (perfect ? 2.35F : 2.05F)) / .4F));
  const float backdropY = centerY - (perfect ? 28 : 0) * scale;
  for (int i = 0; dim > 0 && i < segments; ++i) {
    const float ax = centerX + circle[i].x * 320 * scale;
    const float ay = backdropY + circle[i].y * 105 * scale;
    const float bx = centerX + circle[i + 1].x * 320 * scale;
    const float by = backdropY + circle[i + 1].y * 105 * scale;
    const float cx = centerX + circle[i].x * 480 * scale;
    const float cy = backdropY + circle[i].y * 200 * scale;
    const float dx = centerX + circle[i + 1].x * 480 * scale;
    const float dy = backdropY + circle[i + 1].y * 200 * scale;
    const auto shade = color(0, dim);
    batch.addTriangleColors(centerX, backdropY, ax, ay, bx, by, shade, shade, shade);
    batch.addTriangleColors(ax, ay, cx, cy, dx, dy, shade, 0, 0);
    batch.addTriangleColors(ax, ay, dx, dy, bx, by, shade, 0, shade);
  }
  const float haze = out(time, .08F, .22F) *
      (1 - smooth((time - (perfect ? 2.35F : 2.05F)) / .4F)) * .22F;
  for (int i = 0; haze > 0 && i < segments; ++i) {
    batch.addTriangleColors(centerX, centerY,
        centerX + circle[i].x * 330 * scale, centerY + circle[i].y * 90 * scale,
        centerX + circle[i + 1].x * 330 * scale, centerY + circle[i + 1].y * 90 * scale,
        color(accent, haze), 0, 0);
    if (perfect) {
      const float markY = centerY - 88 * scale;
      batch.addTriangleColors(centerX, markY,
          centerX + circle[i].x * 70 * scale, markY + circle[i].y * 55 * scale,
          centerX + circle[i + 1].x * 70 * scale, markY + circle[i + 1].y * 55 * scale,
          color(kGold, haze * .7F), 0, 0);
    }
  }

  if (!perfect) {
    for (int i = 0; i < 7; ++i) {
      const float age = time - std::abs(i - 3) * .025F;
      if (age < 0 || age > .7F) continue;
      const float q = out(age, 0, .7F);
      const float x = (i - 3) * (60 + q * 12);
      const float y = 90 - q * (170 - std::abs(i - 3) * 14);
      const float alpha = std::sin(q * std::numbers::pi_v<float>);
      line(x, y + 25 * (1 - q), x, y, 2, kLime, alpha * .7F);
      shard(x, y, 8 * (1 - q), 1, std::numbers::pi_v<float> / 2, kWhite, alpha * .6F);
    }
    for (int i = 0; i < 10; ++i) {
      const float age = time - .18F - (i % 4) * .035F;
      if (age < 0 || age > 1.2F) continue;
      const float q = out(age, 0, 1.2F), angle = i * std::numbers::pi_v<float> / 5 + .2F;
      const float x = std::cos(angle) * (190 + q * (80 + i % 3 * 22));
      const float y = std::sin(angle) * (40 + q * (50 + i % 4 * 12)) - q * 60;
      const float alpha = std::sin(clamp(age / .12F) * std::numbers::pi_v<float> / 2) * (1 - q) * .85F;
      shard(x, y, (i % 3 == 0 ? 11 : 5) * (1 - q) + 1, 1.2F,
            angle, i % 3 == 0 ? kWhite : kLime, alpha);
    }
  } else {
    constexpr float onset = .08F;
    const float alpha = out(time, onset, .08F) * (1 - smooth((time - 2.35F) / .4F));
    const float offset = 18 * (1 - out(time, onset, .13F));
    const float markScale = 1 + .2F * (1 - out(time, onset, .28F));
    for (int i = 0; i < 4; ++i) {
      const float dx = circle[i * segments / 4].x;
      const float dy = circle[i * segments / 4].y;
      const auto point = [&](float x, float y) {
        y -= offset;
        return std::pair{(x * dx - y * dy) * markScale,
                         -88 + (x * dy + y * dx) * markScale};
      };
      const auto a = point(0, -32), b = point(24, 0), c = point(12, 0), d = point(0, -16);
      const auto rgb = i % 2 ? kWhite : kGold;
      triangle(a.first, a.second, b.first, b.second, c.first, c.second, rgb, alpha);
      triangle(a.first, a.second, c.first, c.second, d.first, d.second, rgb, alpha);
    }
    const float assembled = out(time, onset, .13F);
    for (const int side : {-1, 1}) {
      shard(side * 43 * markScale, -88, 7 * markScale, 3 * markScale, 0, kGold, alpha * assembled);
      const float age = time - onset;
      if (age < 0 || age > 1.05F) continue;
      for (int i = 0; i < 7; ++i) {
        const float q = out(age, 0, 1.05F), angle = -.85F + i * .23F;
        const float x = side * (160 + q * (125 + std::cos(angle) * 90));
        const float y = -15 + std::sin(angle) * (45 + q * 90) + 28 * q * q;
        const float opacity = out(age, 0, .06F) * (1 - smooth(age / 1.05F)) * .9F;
        shard(x, y, (i % 2 ? 12 : 22) * (1 - q) + 2, i % 2 ? 2 : 3.2F,
              side * angle, i % 3 == 0 ? kWhite : i % 2 ? kTeal : kGold, opacity);
      }
    }
    const float burst = out(time, onset, .55F);
    const float burstAlpha = out(time, onset, .035F) * (1 - smooth((time - onset) / .75F));
    // One ellipse centered on the lettering, never on the diamond/title midpoint.
    for (int i = 0; burstAlpha > 0 && i < segments; ++i) {
      line(circle[i].x * (135 + burst * 275), circle[i].y * (40 + burst * 100),
           circle[i + 1].x * (135 + burst * 275), circle[i + 1].y * (40 + burst * 100),
           4.5F * (1 - burst) + 1, kGold, burstAlpha * .9F);
    }
    line(-330 * burst, 0, 330 * burst, 0, 5 * (1 - burst) + .5F,
         kWhite, burstAlpha * (1 - burst) * .7F);
  }
  const float width = (perfect ? 202 : 235) * out(time, perfect ? .08F : .16F, perfect ? .18F : .35F);
  line(-width, 57, width, 57, 2, accent,
       out(time, perfect ? .08F : .16F, perfect ? .08F : .12F) * (1 - smooth((time - 1.1F) / .65F)));
}
} // namespace completion_celebration
