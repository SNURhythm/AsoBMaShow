#pragma once

#include "CompletionCelebration.h"
#include <algorithm>
#include <cmath>

namespace completion_celebration::wave {
inline float ease(float value) {
  const float q = 1 - std::clamp(value, 0.0F, 1.0F);
  return 1 - q * q * q;
}

// The selected lane-frame.mjs Wave in lane coordinates. Fixed gradient strips
// are projected by the gameplay camera, with no textures or particle storage.
template <typename Batch>
void draw(Batch &batch, Achievement achievement, float age,
          float left, float width, float judgeY, float upperBound) {
  const bool perfect = achievement == Achievement::Perfect;
  const bool max = achievement == Achievement::Max;
  if (achievement == Achievement::Finish) {
    if (age <= 0 || age >= .8F || width <= 0 || upperBound <= judgeY) return;
    const auto smooth = [](float value) {
      const float q = std::clamp(value, 0.0F, 1.0F);
      return q * q * (3 - 2 * q);
    };
    const float alpha = smooth(age / .18F) * (1 - smooth((age - .25F) / .55F)) * .16F;
    const auto shade = 0x00E1D9CDU | (static_cast<unsigned>(alpha * 255) << 24);
    if ((shade >> 24) != 0)
      batch.addRectVerticalGradient(left, judgeY, width, (upperBound - judgeY) * .2F,
                                     shade, 0x00E1D9CDU);
    return;
  }
  const float duration = max ? .95F : perfect ? .65F : .55F;
  if (achievement == Achievement::None || age <= (max ? .14F : 0) || age >= duration ||
      width <= 0 || upperBound <= judgeY) return;
  const float q = max ? (age - .14F) / .8F : age / duration;
  const float exit = std::clamp((age - .55F) / .4F, 0.0F, 1.0F);
  const float fade = max ? ease((age - .14F) / .06F) * (1 - exit * exit * (3 - 2 * exit)) : ease(age / .045F) *
      std::pow(1 - std::clamp((q - .55F) / .45F, 0.0F, 1.0F), 1.4F);
  const float depth = 1 - ease(q) * (max ? 600.0F / 499 : 1.12F);
  const float front = depth - (max ? 0 : .12F);
  const float length = max ? 290.0F / 499 : .12F + (perfect ? .62F : .52F);
  const auto color = [&](float offset) {
    if (max) {
      // Focus's travelling wash: a white/teal crest and a long soft wake.
      // Stops are measured from its upper edge, opposite SVG's bottom-to-top gradient.
      const float blend = offset < .25F ? offset / .25F :
                          1 - std::clamp((offset - .25F) / .3F, 0.0F, 1.0F);
      const float alpha = (offset < .25F ? .7F * blend : offset < .55F ?
          .5F + .2F * blend : .5F * (1 - offset) / .45F) * fade;
      const auto channel = [blend](float teal, float white) {
        return static_cast<unsigned>(teal + (white - teal) * blend);
      };
      return channel(86, 232) | (channel(210, 255) << 8) | (channel(202, 252) << 16) |
          (static_cast<unsigned>(std::clamp(alpha, 0.0F, 1.0F) * 255) << 24);
    }
    const float sigma = offset < .22F ? .11F : .29F;
    const float distance = (offset - .22F) / sigma;
    const float alpha = offset <= 0 || offset >= 1 ? 0 :
        std::exp(-.5F * distance * distance) * (perfect ? .9F : .82F) * fade;
    return (perfect ? 0x0051CCFFU : 0x005CF6B7U) |
        (static_cast<unsigned>(std::clamp(alpha, 0.0F, 1.0F) * 255) << 24);
  };
  constexpr int strips = 24;
  for (int i = 0; i < strips; ++i) {
    const float a = std::clamp(front + length * i / strips, 0.0F, 1.0F);
    const float b = std::clamp(front + length * (i + 1) / strips, 0.0F, 1.0F);
    if (b <= a) continue;
    const auto highColor = color((a - front) / length);
    const auto lowColor = color((b - front) / length);
    if (((highColor | lowColor) >> 24) == 0) continue;
    const float lowY = upperBound - b * (upperBound - judgeY);
    const float height = (b - a) * (upperBound - judgeY);
    batch.addRectVerticalGradient(left, lowY, width, height, lowColor, highColor);
  }
}
} // namespace completion_celebration::wave
