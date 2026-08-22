#include "SkinBitmapFontParser.h"

#include "../../Utils.h"

#include <utf8proc.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>

namespace skin {
namespace {

constexpr std::size_t maximumDescriptorBytes = 32U * 1024U * 1024U;
constexpr std::size_t maximumPages = 512;
constexpr std::size_t maximumGlyphs = 8192;
constexpr std::size_t maximumKerningPairs = 16384;
constexpr int maximumDimension = 8192;

std::size_t limit(const SkinSafetyPolicy &policy,
                  std::size_t standard) noexcept {
  return static_cast<std::size_t>(policy.limit(
      SkinSafetyGuard::ResourceAllocationLimit, standard));
}

bool parseInteger(std::string_view value, int &output) noexcept {
  if (value.empty()) return false;
  const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                      output);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

std::map<std::string, std::string, std::less<>>
attributes(std::string_view line) {
  std::map<std::string, std::string, std::less<>> result;
  const auto firstSpace = line.find_first_of(" \t");
  std::size_t offset = firstSpace == std::string_view::npos
                           ? line.size()
                           : firstSpace;
  while (offset < line.size()) {
    while (offset < line.size() &&
           std::isspace(static_cast<unsigned char>(line[offset]))) {
      ++offset;
    }
    const std::size_t keyStart = offset;
    while (offset < line.size() && line[offset] != '=' &&
           !std::isspace(static_cast<unsigned char>(line[offset]))) {
      ++offset;
    }
    if (offset == keyStart || offset == line.size() || line[offset] != '=') {
      while (offset < line.size() &&
             !std::isspace(static_cast<unsigned char>(line[offset]))) {
        ++offset;
      }
      continue;
    }
    std::string key(line.substr(keyStart, offset - keyStart));
    ++offset;
    std::string value;
    if (offset < line.size() && line[offset] == '"') {
      const std::size_t valueStart = ++offset;
      while (offset < line.size() && line[offset] != '"') ++offset;
      value.assign(line.substr(valueStart, offset - valueStart));
      if (offset < line.size()) ++offset;
    } else {
      const std::size_t valueStart = offset;
      while (offset < line.size() &&
             !std::isspace(static_cast<unsigned char>(line[offset]))) {
        ++offset;
      }
      value.assign(line.substr(valueStart, offset - valueStart));
    }
    result.insert_or_assign(std::move(key), std::move(value));
  }
  return result;
}

bool getInteger(const std::map<std::string, std::string, std::less<>> &values,
                std::string_view key, int &output) noexcept {
  const auto found = values.find(key);
  return found != values.end() && parseInteger(found->second, output);
}

bool validCodepoint(int value) noexcept {
  return value >= 0 && value <= 0x10ffff &&
         (value < 0xd800 || value > 0xdfff);
}

SkinBitmapFontParseResult fail(std::string message) {
  return {.error = std::move(message)};
}

std::vector<std::string_view> lines(std::string_view text) {
  std::vector<std::string_view> result;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const auto end = text.find('\n', offset);
    auto line = text.substr(offset, end == std::string_view::npos
                                        ? text.size() - offset
                                        : end - offset);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    result.push_back(line);
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return result;
}

SkinBitmapFontParseResult parseBmFont(SkinBitmapFontResource resource,
                                     std::string_view text,
                                     const SkinSafetyPolicy &policy) {
  if (text.size() >= 4 && text.substr(0, 3) == "BMF") {
    return fail("binary BMFont descriptors are unsupported");
  }
  for (const unsigned char byte : text) {
    if (byte == 0 || (byte < 0x20 && byte != '\r' && byte != '\n' &&
                      byte != '\t')) {
      return fail("BMFont descriptor is not bounded text");
    }
  }

  SkinParsedBitmapFont font{.resource = std::move(resource)};
  int declaredPages = 0;
  int declaredGlyphs = -1;
  int declaredKernings = -1;
  std::size_t parsedGlyphs = 0;
  std::size_t parsedKernings = 0;
  bool sawCommon = false;
  bool auxiliaryMetricsComplete = font.resource.originalSize > 0;
  for (const auto line : lines(text)) {
    if (line.empty()) continue;
    const auto values = attributes(line);
    if (line.starts_with("info ")) {
      int size = 0;
      if (getInteger(values, "size", size) && size != 0 &&
          size != std::numeric_limits<int>::min() &&
          font.resource.originalSize == 0) {
        font.resource.originalSize = std::abs(size);
        auxiliaryMetricsComplete = true;
      }
    } else if (line.starts_with("common ")) {
      if (!getInteger(values, "lineHeight", font.lineHeight) ||
          !getInteger(values, "base", font.base) ||
          !getInteger(values, "scaleW", font.pageWidth) ||
          !getInteger(values, "scaleH", font.pageHeight)) {
        return fail("BMFont common metrics are incomplete");
      }
      if (!getInteger(values, "pages", declaredPages)) declaredPages = 1;
      if (font.lineHeight <= 0 || font.pageWidth <= 0 ||
          font.pageHeight <= 0 || declaredPages <= 0 ||
          font.lineHeight > maximumDimension ||
          std::llabs(static_cast<long long>(font.base)) > maximumDimension ||
          font.pageWidth > maximumDimension ||
          font.pageHeight > maximumDimension ||
          static_cast<std::size_t>(declaredPages) > limit(policy, maximumPages)) {
        return fail("BMFont common metrics exceed preparation limits");
      }
      font.pagePaths.resize(static_cast<std::size_t>(declaredPages));
      sawCommon = true;
    } else if (line.starts_with("page ")) {
      int id = -1;
      const auto file = values.find("file");
      if (!sawCommon || !getInteger(values, "id", id) || id < 0 ||
          id >= declaredPages || file == values.end() || file->second.empty() ||
          file->second.size() > limit(policy, maximumDescriptorBytes)) {
        return fail("BMFont page declaration is invalid");
      }
      font.pagePaths[static_cast<std::size_t>(id)] = file->second;
    } else if (line.starts_with("chars ")) {
      if (!getInteger(values, "count", declaredGlyphs) || declaredGlyphs < 0 ||
          static_cast<std::size_t>(declaredGlyphs) > limit(policy, maximumGlyphs)) {
        return fail("BMFont glyph count exceeds preparation limits");
      }
    } else if (line.starts_with("char ")) {
      int codepoint = 0;
      SkinBitmapGlyph glyph;
      if (!getInteger(values, "id", codepoint) || !validCodepoint(codepoint) ||
          !getInteger(values, "x", glyph.region.x) ||
          !getInteger(values, "y", glyph.region.y) ||
          !getInteger(values, "width", glyph.region.w) ||
          !getInteger(values, "height", glyph.region.h) ||
          !getInteger(values, "xoffset", glyph.xOffset) ||
          !getInteger(values, "yoffset", glyph.yOffset) ||
          !getInteger(values, "xadvance", glyph.xAdvance) ||
          !getInteger(values, "page", glyph.page) || glyph.page < 0 ||
          glyph.page >= declaredPages || glyph.region.w < 0 ||
          glyph.region.h < 0) {
        return fail("BMFont glyph declaration is invalid");
      }
      glyph.codepoint = static_cast<char32_t>(codepoint);
      font.glyphs.insert_or_assign(glyph.codepoint, glyph);
      if (++parsedGlyphs > limit(policy, maximumGlyphs)) {
        return fail("BMFont glyph count exceeds preparation limits");
      }
    } else if (line.starts_with("kernings ")) {
      if (!getInteger(values, "count", declaredKernings) ||
          declaredKernings < 0 ||
          static_cast<std::size_t>(declaredKernings) >
              limit(policy, maximumKerningPairs)) {
        return fail("BMFont kerning count exceeds preparation limits");
      }
    } else if (line.starts_with("kerning ")) {
      int first = 0;
      int second = 0;
      int amount = 0;
      if (!getInteger(values, "first", first) ||
          !getInteger(values, "second", second) ||
          !getInteger(values, "amount", amount) || !validCodepoint(first) ||
          !validCodepoint(second)) {
        return fail("BMFont kerning declaration is invalid");
      }
      font.kerning.insert_or_assign(
          {static_cast<char32_t>(first), static_cast<char32_t>(second)},
          amount);
      if (++parsedKernings > limit(policy, maximumKerningPairs)) {
        return fail("BMFont kerning count exceeds preparation limits");
      }
    }
  }
  if (!sawCommon ||
      std::ranges::any_of(font.pagePaths,
                          [](const std::string &path) { return path.empty(); }) ||
      (declaredGlyphs >= 0 &&
       parsedGlyphs != static_cast<std::size_t>(declaredGlyphs)) ||
      (declaredKernings >= 0 &&
       parsedKernings != static_cast<std::size_t>(declaredKernings))) {
    return fail("BMFont descriptor is incomplete");
  }
  if (!auxiliaryMetricsComplete) {
    // SkinTextBitmap keeps a libGDX-valid face when its independent first-line
    // size/common scan fails. BitmapFontData uses lineHeight for the source
    // size and base-capHeight for its unflipped ascent.
    font.resource.originalSize = font.lineHeight;
    int capHeight = 0;
    constexpr std::u32string_view caps = U"MNBDCEFKAGHIJLOPQRSTUVWXYZ";
    for (const char32_t codepoint : caps) {
      const auto found = font.glyphs.find(codepoint);
      if (found != font.glyphs.end() && found->second.region.h > 0) {
        capHeight = found->second.region.h;
        break;
      }
    }
    if (capHeight <= 0) {
      for (const auto &[codepoint, glyph] : font.glyphs) {
        (void)codepoint;
        capHeight = std::max(capHeight, glyph.region.h);
      }
    }
    font.base -= capHeight;
  }
  font.auxiliaryMetricsComplete = auxiliaryMetricsComplete;
  return {.font = std::move(font)};
}

std::vector<std::string_view> splitCsv(std::string_view line) {
  std::vector<std::string_view> result;
  std::size_t offset = 0;
  while (offset <= line.size()) {
    const auto comma = line.find(',', offset);
    result.push_back(line.substr(offset, comma == std::string_view::npos
                                            ? line.size() - offset
                                            : comma - offset));
    if (comma == std::string_view::npos) break;
    offset = comma + 1;
  }
  return result;
}

int lr2Integer(std::string_view value) noexcept {
  std::string normalized;
  normalized.reserve(value.size());
  for (char character : value) {
    if (character == ' ') continue;
    normalized.push_back(character == '!' ? '-' : character);
  }
  int result = 0;
  parseInteger(normalized, result);
  return result;
}

std::vector<char32_t> lr2Codepoints(int code) {
  if (code == 288) return {U'\u301c', U'\uff5e'};
  std::string encoded;
  if (code >= 8127) {
    const auto shifted = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(code) + 49281U);
    encoded.push_back(static_cast<char>((shifted >> 8U) & 0xffU));
    encoded.push_back(static_cast<char>(shifted & 0xffU));
  } else if (code >= 256) {
    const auto shifted = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(code) + 32832U);
    encoded.push_back(static_cast<char>((shifted >> 8U) & 0xffU));
    encoded.push_back(static_cast<char>(shifted & 0xffU));
  } else {
    encoded.push_back(static_cast<char>(code & 0xff));
  }
  const auto utf8 = cp932_to_utf8(encoded);
  if (!utf8 || utf8->empty()) return {};
  utf8proc_int32_t scalar = 0;
  const auto consumed = utf8proc_iterate(
      reinterpret_cast<const utf8proc_uint8_t *>(utf8->data()),
      static_cast<utf8proc_ssize_t>(utf8->size()), &scalar);
  return consumed > 0 && validCodepoint(scalar)
             ? std::vector<char32_t>{static_cast<char32_t>(scalar)}
             : std::vector<char32_t>{};
}

SkinBitmapFontParseResult parseLr2Font(SkinBitmapFontResource resource,
                                      std::string_view encoded,
                                      const SkinSafetyPolicy &policy) {
  const auto utf8 = cp932_to_utf8(encoded);
  if (!utf8) return fail("LR2FONT descriptor is not valid MS932");
  SkinParsedBitmapFont font{.resource = std::move(resource), .lr2Font = true};
  font.auxiliaryMetricsComplete = true;
  for (const auto line : lines(*utf8)) {
    if (!line.starts_with('#')) continue;
    const auto fields = splitCsv(line);
    if (fields.empty()) continue;
    std::string command(fields.front().substr(1));
    std::ranges::transform(command, command.begin(), [](unsigned char value) {
      return static_cast<char>(std::toupper(value));
    });
    if (command == "S" && fields.size() > 1) {
      font.resource.originalSize = lr2Integer(fields[1]);
      font.lineHeight = font.resource.originalSize;
      font.base = font.resource.originalSize;
    } else if (command == "M" && fields.size() > 1) {
      font.margin = lr2Integer(fields[1]);
    } else if (command == "T" && fields.size() > 2) {
      const int page = lr2Integer(fields[1]);
      if (page < 0 || static_cast<std::size_t>(page) >=
                          limit(policy, maximumPages)) {
        return fail("LR2FONT page declaration exceeds preparation limits");
      }
      if (font.pagePaths.size() <= static_cast<std::size_t>(page)) {
        font.pagePaths.resize(static_cast<std::size_t>(page) + 1U);
      }
      font.pagePaths[static_cast<std::size_t>(page)] = std::string(fields[2]);
    } else if (command == "R" && fields.size() > 6) {
      const int page = lr2Integer(fields[2]);
      if (page < 0 || static_cast<std::size_t>(page) >= font.pagePaths.size() ||
          font.pagePaths[static_cast<std::size_t>(page)].empty()) {
        continue;
      }
      SkinBitmapGlyph glyph{
          .page = page,
          .region = {.x = lr2Integer(fields[3]),
                     .y = lr2Integer(fields[4]),
                     .w = lr2Integer(fields[5]),
                     .h = lr2Integer(fields[6])}};
      glyph.xAdvance = glyph.region.w;
      if (glyph.region.w < 0 || glyph.region.h < 0) continue;
      for (const char32_t codepoint : lr2Codepoints(lr2Integer(fields[1]))) {
        glyph.codepoint = codepoint;
        font.glyphs.insert_or_assign(codepoint, glyph);
        if (font.glyphs.size() > limit(policy, maximumGlyphs)) {
          return fail("LR2FONT glyph count exceeds preparation limits");
        }
      }
    }
  }
  if (font.resource.originalSize <= 0 || font.pagePaths.empty() ||
      font.glyphs.empty()) {
    return fail("LR2FONT descriptor is incomplete");
  }
  return {.font = std::move(font)};
}

} // namespace

SkinBitmapFontParseResult parseSkinBitmapFont(
    SkinBitmapFontResource resource, std::span<const std::byte> bytes,
    SkinBitmapFontSourceFormat format, SkinSafetyPolicy safetyPolicy) {
  if (resource.id == 0 || resource.virtualPath.empty() || bytes.empty() ||
      bytes.size() > limit(safetyPolicy, maximumDescriptorBytes)) {
    return fail("bitmap-font resource or descriptor size is invalid");
  }
  const std::string_view text(reinterpret_cast<const char *>(bytes.data()),
                              bytes.size());
  return format == SkinBitmapFontSourceFormat::BmFont
             ? parseBmFont(std::move(resource), text, safetyPolicy)
             : parseLr2Font(std::move(resource), text, safetyPolicy);
}

} // namespace skin
