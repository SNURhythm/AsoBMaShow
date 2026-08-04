#include "SkinTextGraphNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinTableDecoder.h"

#include <cmath>
#include <cstddef>
#include <string_view>

namespace skin {
namespace {

bool validUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }

    std::size_t continuationCount = 0;
    std::uint32_t codePoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuationCount = 1;
      codePoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuationCount = 2;
      codePoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuationCount = 3;
      codePoint = first & 0x07;
    } else {
      return false;
    }
    if (continuationCount >= value.size() - index) {
      return false;
    }
    for (std::size_t continuation = 1; continuation <= continuationCount;
         ++continuation) {
      const auto byte = static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0) != 0x80) {
        return false;
      }
      codePoint = (codePoint << 6) | (byte & 0x3f);
    }
    if ((continuationCount == 2 && codePoint < 0x800) ||
        (continuationCount == 3 && codePoint < 0x10000) ||
        (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > 0x10ffff) {
      return false;
    }
    index += continuationCount + 1;
  }
  return true;
}

bool boundedUtf8(std::string_view value) {
  return value.size() <= LuaSkinTableDecoderPolicy::maxCopiedTextBytes &&
         validUtf8(value);
}

bool finiteAndBounded(double value) {
  return std::isfinite(value) &&
         std::abs(value) <= LuaSkinTableDecoderPolicy::maxAuthoredDimension;
}

SkinTextNormalizationResult textFailure(SkinTextGraphNormalizationError error) {
  return {.text = std::nullopt, .error = error};
}

SkinGraphNormalizationResult
graphFailure(SkinTextGraphNormalizationError error) {
  return {.graph = std::nullopt, .error = error};
}

bool validSprite(const SkinSpriteFrames &sprite) {
  if (sprite.resource == 0 || sprite.frames.empty() ||
      sprite.frames.size() >
          LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames ||
      (sprite.timer && !*sprite.timer)) {
    return false;
  }
  for (const auto &frame : sprite.frames) {
    if (frame.gridColumns <= 0 || frame.gridRows <= 0 || frame.gridColumn < 0 ||
        frame.gridRow < 0 || frame.gridColumn >= frame.gridColumns ||
        frame.gridRow >= frame.gridRows || frame.w < -1 || frame.h < -1) {
      return false;
    }
  }
  return true;
}

} // namespace

SkinTextNormalizationResult
normalizeSkinText(const SkinTextNormalizationInput &input,
                  std::span<const SkinFontResource> fonts) {
  if (input.fontName.size() > LuaSkinTableDecoderPolicy::maxCopiedTextBytes ||
      input.literal.size() > LuaSkinTableDecoderPolicy::maxCopiedTextBytes) {
    return textFailure(SkinTextGraphNormalizationError::TextLimitExceeded);
  }
  if (!validUtf8(input.fontName) || !validUtf8(input.literal)) {
    return textFailure(SkinTextGraphNormalizationError::InvalidUtf8);
  }
  if (input.pointSize <= 0 ||
      input.pointSize > LuaSkinTableDecoderPolicy::maxAuthoredDimension ||
      input.alignment < 0 || input.alignment > 2 ||
      static_cast<int>(input.overflow) <
          static_cast<int>(SkinTextOverflow::Overflow) ||
      static_cast<int>(input.overflow) >
          static_cast<int>(SkinTextOverflow::Truncate)) {
    return textFailure(SkinTextGraphNormalizationError::InvalidTextLayout);
  }
  if (!finiteAndBounded(input.outlineWidth) || input.outlineWidth < 0.0 ||
      !finiteAndBounded(input.shadowOffsetX) ||
      !finiteAndBounded(input.shadowOffsetY) ||
      !finiteAndBounded(input.shadowSmoothness) ||
      input.shadowSmoothness < 0.0) {
    return textFailure(SkinTextGraphNormalizationError::InvalidTextStyle);
  }
  if ((input.value && !*input.value) || (input.writer && !*input.writer)) {
    return textFailure(SkinTextGraphNormalizationError::InvalidTextBinding);
  }

  const SkinFontResource *font = nullptr;
  for (const auto &candidate : fonts) {
    if (candidate.authoredName != input.fontName) {
      continue;
    }
    if (font != nullptr) {
      return textFailure(SkinTextGraphNormalizationError::AmbiguousFont);
    }
    font = &candidate;
  }
  if (font == nullptr) {
    return textFailure(SkinTextGraphNormalizationError::MissingFont);
  }
  if (font->id == 0 || font->virtualPath.empty() ||
      !boundedUtf8(font->authoredName) || !boundedUtf8(font->virtualPath)) {
    return textFailure(SkinTextGraphNormalizationError::InvalidFont);
  }
  for (const auto &fallback : font->fallbacks) {
    // Pinned JsonSkinObjectLoader leaves null fallback array slots when an
    // entry has no path, and both text implementations skip those slots.
    // Keep resource ownership outside this helper and accept the equivalent
    // empty source-neutral placeholder without mutating the font definition.
    if (fallback.virtualPath.empty()) {
      continue;
    }
    if (!boundedUtf8(fallback.virtualPath)) {
      return textFailure(SkinTextGraphNormalizationError::InvalidFont);
    }
  }

  return {.text = SkinTextObject{.font = font->id,
                                 .value = input.value,
                                 .writer = input.writer,
                                 .literal = input.literal,
                                 .pointSize = input.pointSize,
                                 .alignment = input.alignment,
                                 .wrapping = input.wrapping,
                                 .overflow = static_cast<int>(input.overflow),
                                 .outlineRgba = input.outlineRgba,
                                 .outlineWidth = input.outlineWidth,
                                 .shadowRgba = input.shadowRgba,
                                 .shadowOffsetX = input.shadowOffsetX,
                                 .shadowOffsetY = input.shadowOffsetY,
                                 .shadowSmoothness = input.shadowSmoothness,
                                 .editable = input.authoredEditable ||
                                             (!input.writerWasExplicit &&
                                              input.writer.has_value())},
          .error = SkinTextGraphNormalizationError::None};
}

SkinGraphNormalizationResult
normalizeSkinGraph(const SkinGraphNormalizationInput &input) {
  if (input.type < 0) {
    // Pinned JsonSkinObjectLoader reserves negative types for 11/28-row
    // SkinDistributionGraph layouts. The v1 model has no representation for
    // those objects, so regular Graph normalization must never absorb them.
    return graphFailure(
        SkinTextGraphNormalizationError::UnsupportedDistributionGraph);
  }
  if (!validSprite(input.fill)) {
    return graphFailure(SkinTextGraphNormalizationError::InvalidGraphSprite);
  }

  SkinGraphObject graph;
  graph.fill = input.fill;
  // Pinned SkinGraph recognizes exactly 1 as down; every other authored
  // value uses the rightward branch.
  graph.direction = input.direction == 1 ? 1 : 0;

  if (input.explicitRate) {
    if (!*input.explicitRate) {
      return graphFailure(SkinTextGraphNormalizationError::InvalidGraphBinding);
    }
    graph.value = *input.explicitRate;
  } else if (input.isRefNum) {
    if (!input.integerRange || !input.integerRange->value ||
        input.integerRange->minimum > input.integerRange->maximum) {
      return graphFailure(SkinTextGraphNormalizationError::InvalidGraphRange);
    }
    graph.value = *input.integerRange;
  } else {
    if (!input.implicitRate) {
      return graphFailure(SkinTextGraphNormalizationError::InvalidGraphBinding);
    }
    graph.value = input.implicitRate;
  }
  return {.graph = std::move(graph),
          .error = SkinTextGraphNormalizationError::None};
}

} // namespace skin

#endif
