#pragma once

#include "PlaySkinViewport.h"
#include "SkinDestinationEvaluator.h"
#include "SkinGeneratedTexture.h"
#include "SkinResourceCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace skin {

struct SkinVertex {
  float x = 0.0F;
  float y = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
  std::uint32_t rgba = 0xffffffffU;
};

struct SkinRenderState {
  struct DistanceField {
    bool colored = false;
    double outlineDistance = 0.5;
    std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
    std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
    double shadowSmoothing = 0.0;
    double shadowOffsetU = 0.0;
    double shadowOffsetV = 0.0;
    auto operator<=>(const DistanceField &) const = default;
  };
  SkinBlendMode blend = SkinBlendMode::Normal;
  SkinFilterMode filter = SkinFilterMode::Nearest;
  std::optional<UiLogicalRect> scissor;
  std::optional<DistanceField> distanceField;
};

struct SkinTexturedQuadCommand {
  SkinResourceId resource = 0;
  std::array<SkinVertex, 4> vertices{};
  SkinRenderState state;
};

// Pixmap-backed Beatoraja widgets own a source texture independently from the
// destination which samples it. Pixels remain value-owned by the immutable
// command buffer until whole-frame texture preflight completes.
struct SkinGeneratedTexturedQuadCommand {
  SkinGeneratedTextureKey key;
  SkinGeneratedTextureData texture;
  std::array<SkinVertex, 4> vertices{};
  SkinRenderState state;
};

struct SkinGlyphInstance {
  char32_t codepoint = 0;
  std::array<SkinVertex, 4> vertices{};
};

struct SkinGlyphRunCommand {
  SkinTextAtlasId atlas = 0;
  std::vector<SkinGlyphInstance> glyphs;
  SkinRenderState state;
};

enum class SkinPrimitiveKind : std::uint8_t {
  SolidQuad,
  LineStrip,
  TriangleStrip,
};

struct SkinPrimitiveCommand {
  SkinPrimitiveKind kind = SkinPrimitiveKind::SolidQuad;
  std::vector<SkinVertex> vertices;
  SkinRenderState state;
};

struct SkinBgaCommand {
  AuthoredDestinationGeometry authoredGeometry;
  PlaySkinViewport viewport;
  std::uint32_t authoredOrdinal = 0;
};

using SkinDrawPayload =
    std::variant<SkinTexturedQuadCommand, SkinGeneratedTexturedQuadCommand,
                 SkinGlyphRunCommand, SkinPrimitiveCommand, SkinBgaCommand>;

struct SkinDrawCommand {
  std::uint32_t authoredOrdinal = 0;
  SkinObjectId sourceObject = 0;
  SkinDrawPayload payload;
};

struct SkinBatchRange {
  std::size_t firstCommand = 0;
  std::size_t commandCount = 0;
};

struct SkinCommandBuffer {
  std::uint64_t frameSerial = 0;
  std::vector<SkinDrawCommand> commands;
  std::vector<SkinBatchRange> adjacentBatches;
};

} // namespace skin
