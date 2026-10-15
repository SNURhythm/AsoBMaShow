#include "view/ArtworkGlowProfile.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

// Evaluate the shader's scalar operations against the independent, unchanged
// CPU mesh. This checks the uploaded profile, including its palette diffusion,
// radial terms and band mapping, without creating a GPU/device.
rendering::PosColorVertex evaluate(const ArtworkGlowProfileVertex &vertex,
                                   float x, float y, float width, float height,
                                   const ArtworkGlow &glow, float contextOpacity) {
  const auto band = static_cast<std::size_t>(vertex.bandIndex);
  const float wave = std::clamp(glow.spectrum[band] * (1 - vertex.blend) +
      glow.spectrum[std::min(band + 1, glow.spectrum.size() - 1)] * vertex.blend,
      0.0f, 1.0f);
  const float opacity = wave * wave;
  const float bass = std::clamp(glow.bass, 0.0f, 1.0f);
  const float extent = glow.spread * (0.02f + 0.08f * bass + 0.90f * opacity * wave) *
      (0.75f + 0.25f * bass);
  const float offset = -1 * (1 - vertex.radius) + extent * vertex.radius;
  const float alpha = 0.87f * opacity * vertex.body +
      (0.10f * opacity + 0.025f * bass) * vertex.contactTaper;
  const float paletteAlpha = static_cast<float>(vertex.abgr >> 24) / 255;
  const auto quantizedAlpha = static_cast<uint32_t>(std::floor(std::floor(
      paletteAlpha * glow.intensity * alpha * 255 + 0.5f) * contextOpacity));
  return {x + vertex.u * width + vertex.nx * offset,
          y + vertex.v * height + vertex.ny * offset, 0,
          (vertex.abgr & 0x00ffffff) | (quantizedAlpha << 24)};
}

void compareReference(const ArtworkGlowProfile &profile, float x, float y,
                      float width, float height, const ArtworkPalette &palette,
                      const ArtworkGlow &glow, float contextOpacity) {
  const auto reference = makeArtworkGlowMesh(x, y, width, height, palette, glow);
  require(profile.indices == reference.indices,
          "retained topology preserves all CPU triangles and the perimeter seam");
  for (std::size_t i = 0; i < profile.vertices.size(); ++i) {
    const auto actual = evaluate(profile.vertices[i], x, y, width, height, glow,
                                 contextOpacity);
    const auto &expected = reference.vertices[i];
    require(std::abs(actual.x - expected.x) < 0.0005f &&
                std::abs(actual.y - expected.y) < 0.0005f && actual.z == expected.z,
            "GPU profile preserves CPU wave positions across layouts and pulses");
    require((actual.abgr & 0x00ffffff) == (expected.abgr & 0x00ffffff),
            "cached palette diffusion exactly preserves every CPU RGB value");
    const auto expectedAlpha = static_cast<uint8_t>((expected.abgr >> 24) * contextOpacity);
    require(std::abs(static_cast<int>(actual.abgr >> 24) - expectedAlpha) <= 1,
            "GPU alpha retains CPU quantization within one rounding LSB");
    if (i >= ArtworkGlowMesh::layers * ArtworkGlowMesh::segments)
      require((actual.abgr >> 24) == 0, "the outer boundary remains transparent");
    require(actual.x >= x - glow.spread - 0.001f &&
                actual.x <= x + width + glow.spread + 0.001f &&
                actual.y >= y - glow.spread - 0.001f &&
                actual.y <= y + height + glow.spread + 0.001f,
            "all animated vertices remain inside the existing glow bounds");
  }
}

void testProfilesMatchReferenceAcrossPalettesLayoutsAndPulses() {
  std::array<ArtworkPalette, 3> palettes;
  palettes[0].fill(Color(255, 120, 3, 255));
  palettes[1].fill(Color(0, 0, 0, 0));
  for (std::size_t i = 0; i < palettes[2].size(); ++i)
    palettes[2][i] = Color(static_cast<uint8_t>(i * 37),
                           static_cast<uint8_t>(i * 71),
                           static_cast<uint8_t>(i * 113),
                           static_cast<uint8_t>(i * 19));
  const std::array<std::array<float, 4>, 4> rectangles = {{
      {100, 100, 400, 300}, {-37.25f, 23.125f, 96.5f, 421.25f},
      {0, 0, 460, 460}, {1220.25f, -303.75f, 1024, 32}}};
  for (const auto &palette : palettes) {
    for (const auto &rect : rectangles) {
      const auto profile = makeArtworkGlowProfile(rect[2], rect[3], palette);
      require(profile.vertices.size() == 3328 && profile.indices.size() == 18432,
              "retained profile preserves the bounded mesh resolution");
      for (int pulse = 0; pulse < 12; ++pulse) {
        ArtworkGlow glow{.intensity = pulse % 3 == 0 ? 0.0f : 0.17f * (pulse % 6),
                         .spread = pulse % 4 == 0 ? 0.0f : 44.0f + pulse,
                         .bass = (pulse - 1) / 8.0f};
        for (std::size_t band = 0; band < glow.spectrum.size(); ++band) {
          if (pulse == 1) glow.spectrum[band] = 0;
          else if (pulse == 2) glow.spectrum[band] = 1;
          else glow.spectrum[band] = static_cast<float>((band * 7 + pulse * 11) % 23) / 19;
        }
        for (const float contextOpacity : {0.0f, 0.23f, 0.7f, 1.0f})
          compareReference(profile, rect[0], rect[1], rect[2], rect[3], palette,
                           glow, contextOpacity);
      }
      // Reusing the profile under translation must not retain the old origin.
      ArtworkGlow moved{.intensity = 1, .spread = 44, .bass = 1};
      moved.spectrum.fill(1);
      compareReference(profile, rect[0] + 17.5f, rect[1] - 2.25f,
                       rect[2], rect[3], palette, moved, 1);
    }
  }
}
}

int main() {
  testProfilesMatchReferenceAcrossPalettesLayoutsAndPulses();
  std::cout << "artwork_glow_tests passed\n";
}
