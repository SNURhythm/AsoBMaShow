#pragma once

#include "../rendering/Color.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>

constexpr std::size_t kArtworkPaletteSide = 8;
using ArtworkPalette = std::array<Color, kArtworkPaletteSide * kArtworkPaletteSide>;

// A blurred 8 x 8 color field preserves spatial color differences in the jacket.
// Sample existing decoded pixels only when installing a texture.
inline std::optional<ArtworkPalette> artworkAmbientPalette(std::span<const unsigned char> rgba,
                                                           int width, int height) {
  if (width <= 0 || height <= 0 ||
      rgba.size() / 4 / static_cast<std::size_t>(width) < static_cast<std::size_t>(height))
    return std::nullopt;
  ArtworkPalette palette;
  bool visible = false;
  for (std::size_t i = 0; i < palette.size(); ++i) {
    const float u = static_cast<float>(i % kArtworkPaletteSide) / (kArtworkPaletteSide - 1);
    const float v = static_cast<float>(i / kArtworkPaletteSide) / (kArtworkPaletteSide - 1);
    double red = 0, green = 0, blue = 0, weight = 0, coverage = 0, kernelWeight = 0;
    for (int y = -3; y <= 3; ++y) {
      for (int x = -3; x <= 3; ++x) {
        const auto px = std::clamp(static_cast<int>((u + x * 0.07f) * width), 0, width - 1);
        const auto py = std::clamp(static_cast<int>((v + y * 0.07f) * height), 0, height - 1);
        const auto offset = (static_cast<std::size_t>(py) * width + px) * 4;
        const auto r = rgba[offset], g = rgba[offset + 1], b = rgba[offset + 2];
        const double kernel = std::exp(-(x * x + y * y) / 8.0);
        const double alpha = rgba[offset + 3] / 255.0;
        const double chroma = (std::max({r, g, b}) - std::min({r, g, b})) / 255.0;
        const double sampleWeight = kernel * alpha * (0.04 + chroma * chroma);
        red += r * sampleWeight;
        green += g * sampleWeight;
        blue += b * sampleWeight;
        weight += sampleWeight;
        coverage += kernel * alpha;
        kernelWeight += kernel;
      }
    }
    const double peak = weight > 0 ? std::max({red, green, blue}) / weight : 0;
    if (peak < 1) {
      palette[i] = Color(0, 0, 0, 0);
      continue;
    }
    const double minimum = std::min({red, green, blue}) / weight;
    const double saturation = (peak - minimum) / peak;
    // Lift colored regions gently; neutral/dark borders must stay dark instead
    // of turning into a gray light source. Cap amplification of near-black noise.
    const double lift = saturation >= 0.15 ? std::clamp(210.0 / peak, 1.0, 3.0) : 1.0;
    const double scale = lift / weight;
    palette[i] = Color(
        static_cast<uint8_t>(std::clamp(std::lround(red * scale), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(green * scale), 0L, 255L)),
        static_cast<uint8_t>(std::clamp(std::lround(blue * scale), 0L, 255L)),
        static_cast<uint8_t>(std::lround(255 * coverage / kernelWeight)));
    visible |= palette[i].a > 0;
  }
  return visible ? std::optional(palette) : std::nullopt;
}
