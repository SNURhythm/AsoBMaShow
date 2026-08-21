#pragma once

#include "BeatorajaSkinModel.h"
#include "../SkinSafetyPolicy.h"

#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace skin {

enum class SkinBitmapFontSourceFormat : std::uint8_t {
  BmFont,
  Lr2Font,
};

struct SkinParsedBitmapFont {
  SkinBitmapFontResource resource;
  std::vector<std::string> pagePaths;
  std::map<char32_t, SkinBitmapGlyph> glyphs;
  std::map<std::pair<char32_t, char32_t>, int> kerning;
  int lineHeight = 0;
  int base = 0;
  int pageWidth = 0;
  int pageHeight = 0;
  int margin = 0;
  bool lr2Font = false;
  bool auxiliaryMetricsComplete = false;
};

struct SkinBitmapFontParseResult {
  std::optional<SkinParsedBitmapFont> font;
  std::string error;
};

[[nodiscard]] SkinBitmapFontParseResult parseSkinBitmapFont(
    SkinBitmapFontResource resource, std::span<const std::byte> bytes,
    SkinBitmapFontSourceFormat format,
    SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});

} // namespace skin
