#pragma once

#include "ArtworkColor.h"
#include "../audio/AudioSpectrum.h"
#include "../rendering/common.h"

struct ArtworkGlow {
  float intensity = 0;
  float spread = 0;
  audio::Spectrum spectrum{};
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
    // Mirror fixed bass-to-treble bands across the two halves of the aura.
    // Each band controls how far its color diffuses from the artwork.
    const float frequencyPhase = phase < 0.5f ? phase * 2 : (1 - phase) * 2;
    const float bandPosition = frequencyPhase * (glow.spectrum.size() - 1);
    const auto bandIndex = static_cast<std::size_t>(bandPosition);
    const float wave = std::clamp(std::lerp(glow.spectrum[bandIndex],
        glow.spectrum[std::min(bandIndex + 1, glow.spectrum.size() - 1)],
        bandPosition - bandIndex), 0.0f, 1.0f);
    const float reach = 0.28f + 0.72f * std::pow(wave, 0.75f);
    const Color color = artworkGlowColor(palette, edge.x / width, edge.y / height);
    for (std::size_t ring = 0; ring <= mesh.layers; ++ring) {
      const float radius = static_cast<float>(ring) / mesh.layers;
      const float offset = std::lerp(-1.0f, glow.spread, radius);
      // Fill the entire spread with color that decays with distance. The
      // spectrum changes the falloff length, never draws a luminous rim.
      const float distance = radius / reach;
      const float distanceSquared = distance * distance;
      const float radiusSquared = radius * radius;
      // A filled, stronger core drops away more sharply than a Gaussian.
      const float taper = 1 - radiusSquared * radiusSquared;
      const float fade = std::exp(-5 * distanceSquared * distanceSquared) * taper;
      auto feathered = color;
      feathered.a = static_cast<uint8_t>(std::lround(color.a * glow.intensity *
                                                   (0.55f + 0.45f * wave) * fade));
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
