#pragma once

#include "../rendering/Color.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

// Bounded sampling of already-decoded pixels, performed only when an image is
// installed. Saturated pixels outweigh black/white borders and lettering.
inline std::optional<Color> artworkAmbientColor(std::span<const unsigned char> rgba,
                                                int width, int height) {
  if (width <= 0 || height <= 0 ||
      rgba.size() / 4 / static_cast<std::size_t>(width) < static_cast<std::size_t>(height))
    return std::nullopt;
  double red = 0, green = 0, blue = 0, weight = 0;
  const int columns = std::min(width, 32), rows = std::min(height, 32);
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < columns; ++x) {
      const auto offset = (static_cast<std::size_t>((y + 0.5) * height / rows) * width +
                           static_cast<std::size_t>((x + 0.5) * width / columns)) * 4;
      const auto r = rgba[offset], g = rgba[offset + 1], b = rgba[offset + 2];
      const double chroma = (std::max({r, g, b}) - std::min({r, g, b})) / 255.0;
      const double sampleWeight = rgba[offset + 3] / 255.0 * (0.02 + chroma * chroma);
      red += r * sampleWeight;
      green += g * sampleWeight;
      blue += b * sampleWeight;
      weight += sampleWeight;
    }
  }
  if (weight == 0) return std::nullopt;
  // Lift dark artwork into a visible light color without changing its hue.
  const double peak = std::max({red, green, blue}) / weight;
  if (peak < 1) return Color(180, 180, 180);
  const double scale = std::max(peak, 210.0) / peak / weight;
  return Color(static_cast<uint8_t>(std::clamp(std::lround(red * scale), 0L, 255L)),
               static_cast<uint8_t>(std::clamp(std::lround(green * scale), 0L, 255L)),
               static_cast<uint8_t>(std::clamp(std::lround(blue * scale), 0L, 255L)));
}
