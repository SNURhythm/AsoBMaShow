#pragma once

#include "ArtworkGlow.h"

// Static attributes consumed by vs_artwork_glow.sc. Layout: Position Float3,
// TexCoord0 Float4, TexCoord1 Float2, Color0 normalized Uint8x4.
struct ArtworkGlowProfileVertex {
  float u, v, radius;
  float nx, ny, body, contactTaper;
  float bandIndex, blend;
  uint32_t abgr;
};
static_assert(sizeof(ArtworkGlowProfileVertex) == 40);
static_assert(audio::kSpectrumBandCount == 16);

struct ArtworkGlowProfile {
  std::array<ArtworkGlowProfileVertex,
             ArtworkGlowMesh::segments * (ArtworkGlowMesh::layers + 1)> vertices;
  std::array<uint16_t, ArtworkGlowMesh::segments * ArtworkGlowMesh::layers * 6> indices;
};

// Cache per palette and layout size. Translation and audio animation only change
// uniforms; palette sampling, rounding, diffusion and radial falloff stay here.
inline ArtworkGlowProfile makeArtworkGlowProfile(float width, float height,
                                                  const ArtworkPalette &palette) {
  ArtworkGlowProfile profile;
  struct RadialTerms {
    float radius, body, contactTaper, diffusion;
  };
  std::array<RadialTerms, ArtworkGlowMesh::layers + 1> radial;
  for (std::size_t ring = 0; ring < radial.size(); ++ring) {
    const float radius = static_cast<float>(ring) / ArtworkGlowMesh::layers;
    const float radiusSquared = radius * radius;
    const float taper = (1 - radiusSquared) * (1 - radiusSquared);
    const float contact = std::exp(-32 * radiusSquared);
    const float body = std::clamp(1 - radiusSquared * radius *
        (radius * (6 * radius - 15) + 10), 0.0f, 1.0f);
    radial[ring] = {radius, body, contact * taper,
                    radiusSquared * (3 - 2 * radius)};
  }
  for (std::size_t i = 0; i < ArtworkGlowMesh::segments; ++i) {
    const float phase = static_cast<float>(i) / ArtworkGlowMesh::segments;
    const auto edge = artworkGlowEdge(phase, width, height);
    const float u = edge.x / width, v = edge.y / height;
    const float frequencyPhase = phase < 0.5f ? phase * 2 : (1 - phase) * 2;
    const float bandPosition = 4 + frequencyPhase * (audio::kSpectrumBandCount - 5);
    const auto bandIndex = static_cast<std::size_t>(bandPosition);
    const float fraction = bandPosition - bandIndex;
    const float blend = fraction * fraction * (3 - 2 * fraction);
    const auto color = artworkGlowColor(palette, u, v);
    const std::array<Color, 4> neighbors = {
        artworkGlowColor(palette, u - 0.18f, v), artworkGlowColor(palette, u + 0.18f, v),
        artworkGlowColor(palette, u, v - 0.18f), artworkGlowColor(palette, u, v + 0.18f)};
    const auto diffuse = [&](uint8_t Color::*channel) {
      float sum = 2 * (color.*channel);
      for (const auto &neighbor : neighbors) sum += neighbor.*channel;
      return sum / 6;
    };
    const float diffuseR = diffuse(&Color::r), diffuseG = diffuse(&Color::g),
                diffuseB = diffuse(&Color::b);
    for (std::size_t ring = 0; ring < radial.size(); ++ring) {
      const auto &terms = radial[ring];
      auto feathered = color;
      feathered.r = static_cast<uint8_t>(std::lround(std::lerp(float(color.r), diffuseR, terms.diffusion)));
      feathered.g = static_cast<uint8_t>(std::lround(std::lerp(float(color.g), diffuseG, terms.diffusion)));
      feathered.b = static_cast<uint8_t>(std::lround(std::lerp(float(color.b), diffuseB, terms.diffusion)));
      profile.vertices[ring * ArtworkGlowMesh::segments + i] = {
          u, v, terms.radius, edge.nx, edge.ny, terms.body, terms.contactTaper,
          static_cast<float>(bandIndex), blend, feathered.toABGR()};
      if (ring == ArtworkGlowMesh::layers) continue;
      const auto first = static_cast<uint16_t>(ring * ArtworkGlowMesh::segments + i);
      const auto next = static_cast<uint16_t>(ring * ArtworkGlowMesh::segments +
                                             (i + 1) % ArtworkGlowMesh::segments);
      const auto outer = static_cast<uint16_t>(first + ArtworkGlowMesh::segments);
      const auto outerNext = static_cast<uint16_t>(next + ArtworkGlowMesh::segments);
      const std::array<uint16_t, 6> quad = {first, next, outer, next, outerNext, outer};
      std::copy(quad.begin(), quad.end(),
                profile.indices.begin() + (ring * ArtworkGlowMesh::segments + i) * 6);
    }
  }
  return profile;
}
