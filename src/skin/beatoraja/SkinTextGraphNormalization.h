#pragma once

#include "BeatorajaSkinModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace skin {

// These values are the pinned SkinText overflow constants at Beatoraja
// c2ed5db1: 0=overflow, 1=shrink, 2=truncate.
enum class SkinTextOverflow : int { Overflow = 0, Shrink = 1, Truncate = 2 };

// Raw, already-sandboxed Text fields. Binding IDs are supplied by the binding
// decoder; this helper has no Lua or filesystem dependency.
struct SkinTextNormalizationInput {
  std::string fontName;
  std::optional<SkinStringPropertyId> value;
  std::optional<SkinStringWriterId> writer;
  std::string literal;
  int pointSize = 0;
  int alignment = 0;
  bool wrapping = false;
  SkinTextOverflow overflow = SkinTextOverflow::Overflow;
  std::array<std::uint8_t, 4> outlineRgba{255, 255, 255, 0};
  double outlineWidth = 0.0;
  std::array<std::uint8_t, 4> shadowRgba{255, 255, 255, 0};
  double shadowOffsetX = 0.0;
  double shadowOffsetY = 0.0;
  double shadowSmoothness = 0.0;
  bool editable = false;
};

// The fields mirror JsonSkin.Graph's loader precedence: value is selected
// first; otherwise isRefNum chooses the integer min/max source; otherwise the
// type field is a rate source.
struct SkinGraphNormalizationInput {
  SkinSpriteFrames fill;
  std::optional<SkinFloatPropertyId> explicitRate;
  SkinFloatPropertyId implicitRate{};
  std::optional<SkinSliderObject::IntegerRangeSource> integerRange;
  bool isRefNum = false;
  int direction = 1;
};

enum class SkinTextGraphNormalizationError : std::uint8_t {
  None,
  MissingFont,
  AmbiguousFont,
  InvalidFont,
  InvalidUtf8,
  TextLimitExceeded,
  InvalidTextLayout,
  InvalidTextStyle,
  InvalidTextBinding,
  InvalidGraphSprite,
  InvalidGraphBinding,
  InvalidGraphRange,
};

struct SkinTextNormalizationResult {
  std::optional<SkinTextObject> text;
  SkinTextGraphNormalizationError error = SkinTextGraphNormalizationError::None;
};

struct SkinGraphNormalizationResult {
  std::optional<SkinGraphObject> graph;
  SkinTextGraphNormalizationError error = SkinTextGraphNormalizationError::None;
};

[[nodiscard]] SkinTextNormalizationResult
normalizeSkinText(const SkinTextNormalizationInput &input,
                  std::span<const SkinFontResource> fonts);

[[nodiscard]] SkinGraphNormalizationResult
normalizeSkinGraph(const SkinGraphNormalizationInput &input);

} // namespace skin
