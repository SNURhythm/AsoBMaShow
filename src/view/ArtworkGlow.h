#pragma once

#include "ArtworkColor.h"
#include "PreviewGlow.h"
#include "../rendering/common.h"

struct ArtworkGlow {
  float intensity = 0;
  float spread = 0;
  std::array<float, PreviewGlow::kWaveformSize> waveform{};
};

struct ArtworkGlowMesh {
  static constexpr std::size_t segments = 256;
  static constexpr std::size_t layers = 12;
  std::array<rendering::PosColorVertex, segments * (layers + 1)> vertices;
  std::array<uint16_t, segments * layers * 6> indices;
};

struct ArtworkGlowEdgePoint {
  float x, y, nx, ny;
};

inline ArtworkGlowEdgePoint artworkGlowEdge(float phase, float width, float height) {
  constexpr float pi = 3.141592654f;
  const float radius = std::min(width, height) * 0.08f;
  const float horizontal = width - 2 * radius, vertical = height - 2 * radius;
  const float arc = pi * radius * 0.5f;
  const std::array<float, 8> lengths = {horizontal, arc, vertical, arc,
                                        horizontal, arc, vertical, arc};
  float distance = phase * (2 * horizontal + 2 * vertical + 4 * arc);
  std::size_t section = 0;
  while (section + 1 < lengths.size() && distance > lengths[section]) {
    distance -= lengths[section++];
  }
  if (section == 0) return {radius + distance, 0, 0, -1};
  if (section == 2) return {width, radius + distance, 1, 0};
  if (section == 4) return {width - radius - distance, height, 0, 1};
  if (section == 6) return {0, height - radius - distance, -1, 0};
  const std::size_t corner = section / 2;
  const float angle = (-0.5f + corner * 0.5f) * pi + distance / radius;
  const float nx = std::cos(angle), ny = std::sin(angle);
  const float cx = corner < 2 ? width - radius : radius;
  const float cy = corner == 0 || corner == 3 ? radius : height - radius;
  return {cx + nx * radius, cy + ny * radius, nx, ny};
}

inline Color artworkGlowColor(const ArtworkPalette &palette, float u, float v) {
  const float px = std::clamp(u, 0.0f, 1.0f) * (kArtworkPaletteSide - 1);
  const float py = std::clamp(v, 0.0f, 1.0f) * (kArtworkPaletteSide - 1);
  const auto x = static_cast<std::size_t>(px), y = static_cast<std::size_t>(py);
  const auto nextX = std::min(x + 1, kArtworkPaletteSide - 1);
  const auto nextY = std::min(y + 1, kArtworkPaletteSide - 1);
  const auto &a = palette[y * kArtworkPaletteSide + x];
  const auto &b = palette[y * kArtworkPaletteSide + nextX];
  const auto &c = palette[nextY * kArtworkPaletteSide + x];
  const auto &d = palette[nextY * kArtworkPaletteSide + nextX];
  const auto channel = [&](uint8_t Color::*member) {
    return static_cast<uint8_t>(std::lround(std::lerp(
        std::lerp(float(a.*member), float(b.*member), px - x),
        std::lerp(float(c.*member), float(d.*member), px - x), py - y)));
  };
  return Color(channel(&Color::r), channel(&Color::g), channel(&Color::b), channel(&Color::a));
}

inline ArtworkGlowMesh makeArtworkGlowMesh(float x, float y, float width, float height,
                                           const ArtworkPalette &palette,
                                           const ArtworkGlow &glow) {
  ArtworkGlowMesh mesh;
  for (std::size_t i = 0; i < mesh.segments; ++i) {
    const float phase = static_cast<float>(i) / mesh.segments;
    const auto edge = artworkGlowEdge(phase, width, height);
    const float edgeX = x + edge.x, edgeY = y + edge.y;
    const float nx = edge.nx, ny = edge.ny;
    const float wavePosition = phase * glow.waveform.size();
    const auto waveIndex = static_cast<std::size_t>(wavePosition);
    float blend = wavePosition - waveIndex;
    blend = blend * blend * (3 - 2 * blend);
    const float wave = std::clamp(std::lerp(glow.waveform[waveIndex],
        glow.waveform[(waveIndex + 1) % glow.waveform.size()], blend), 0.0f, 1.0f);
    const float extent = glow.spread * (0.45f + 0.55f * wave);
    const Color color = artworkGlowColor(palette, edge.x / width, edge.y / height);
    for (std::size_t ring = 0; ring <= mesh.layers; ++ring) {
      const float radius = static_cast<float>(ring) / mesh.layers;
      const float offset = std::lerp(-1.0f, extent, radius);
      const float aura = std::exp(-2.0f * radius * radius) * (1 - radius);
      // A soft luminous ribbon follows the same audio-shaped boundary as the
      // broader color bleed. Its feathered edges avoid a hard outline.
      const float ribbonDistance = (radius - 0.58f) / 0.075f;
      const float ribbon = 0.35f * std::exp(-ribbonDistance * ribbonDistance);
      const float fade = ring == mesh.layers ? 0 : aura + ribbon;
      auto feathered = color;
      feathered.a = static_cast<uint8_t>(std::lround(color.a * 0.90f * glow.intensity *
                                                   (0.65f + 0.35f * wave) * fade));
      mesh.vertices[ring * mesh.segments + i] = {
          edgeX + nx * offset, edgeY + ny * offset, 0, feathered.toABGR()};
      if (ring == mesh.layers) continue;
      const auto first = static_cast<uint16_t>(ring * mesh.segments + i);
      const auto next = static_cast<uint16_t>(ring * mesh.segments + (i + 1) % mesh.segments);
      const auto outer = static_cast<uint16_t>(first + mesh.segments);
      const auto outerNext = static_cast<uint16_t>(next + mesh.segments);
      const std::array<uint16_t, 6> quad = {first, next, outer, next, outerNext, outer};
      std::copy(quad.begin(), quad.end(), mesh.indices.begin() + (ring * mesh.segments + i) * 6);
    }
  }
  return mesh;
}
