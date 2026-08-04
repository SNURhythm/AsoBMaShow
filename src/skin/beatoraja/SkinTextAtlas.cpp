#include "SkinTextAtlas.h"
#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "view/SdlTtfRuntime.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace skin {
namespace {
bool quantize(double &value) noexcept {
  if (!std::isfinite(value) || std::abs(value) > 4096.0) return false;
  value = std::round(value * 64.0) / 64.0;
  return true;
}
}
bool canonicalizeSkinTextAtlasKey(SkinTextAtlasKey &key) noexcept {
  return key.font != 0 && key.pointSize > 0 && key.pointSize <= 512 &&
         !key.fallbackChainDigest.empty() &&
         key.fallbackChainDigest.size() <=
             SkinResourcePolicy::maximumFallbackChainDigestBytes &&
         key.outlineWidth >= 0.0 && key.shadowSmoothness >= 0.0 &&
         quantize(key.outlineWidth) && quantize(key.shadowOffsetX) &&
         quantize(key.shadowOffsetY) && quantize(key.shadowSmoothness);
}
std::string stableFallbackChainDigest(SkinResourceId primary, int primaryType,
                                      const std::vector<SkinFontFallbackResource> &fallbacks) {
  std::string result = std::to_string(primary) + ':' +
                       std::to_string(primaryType);
  for (const auto &fallback : fallbacks) {
    if (!appendStableFallbackChainEntry(result, fallback.virtualPath,
                                        fallback.type)) {
      return {};
    }
  }
  return result;
}

bool appendStableFallbackChainEntry(
    std::string &digest, std::string_view normalizedVirtualPath, int type) {
  const std::string pathSize = std::to_string(normalizedVirtualPath.size());
  const std::string typeText = std::to_string(type);
  const auto append = [&](std::string_view value) {
    if (digest.size() > SkinResourcePolicy::maximumFallbackChainDigestBytes ||
        value.size() > SkinResourcePolicy::maximumFallbackChainDigestBytes -
                           digest.size()) {
      return false;
    }
    digest.append(value);
    return true;
  };
  return append("|") && append(pathSize) && append(":") &&
         append(normalizedVirtualPath) && append(":") && append(typeText);
}

namespace {
struct OpenFace {
  TTF_Font *font = nullptr;
  ~OpenFace() { if (font) TTF_CloseFont(font); }
  OpenFace() = default;
  OpenFace(const OpenFace &) = delete;
  OpenFace &operator=(const OpenFace &) = delete;
  OpenFace(OpenFace &&other) noexcept : font(std::exchange(other.font, nullptr)) {}
  OpenFace &operator=(OpenFace &&other) noexcept { std::swap(font, other.font); return *this; }
};
struct GlyphBitmap {
  char32_t codepoint = 0;
  std::size_t face = 0;
  int bearingX = 0;
  int bearingY = 0;
  int advance = 0;
  int width = 0;
  int height = 0;
  int padding = 0;
  std::vector<unsigned char> rgba;
};

void alphaOver(unsigned char *destination, const std::array<std::uint8_t, 4> color,
               unsigned char alpha) {
  const unsigned int sourceAlpha = static_cast<unsigned int>(color[3]) * alpha / 255U;
  const unsigned int destinationAlpha = destination[3];
  const unsigned int outputAlpha = sourceAlpha + destinationAlpha * (255U - sourceAlpha) / 255U;
  if (outputAlpha == 0) return;
  for (int channel = 0; channel != 3; ++channel) {
    const unsigned int source = color[static_cast<std::size_t>(channel)] * sourceAlpha;
    const unsigned int destinationValue = static_cast<unsigned int>(destination[channel]) * destinationAlpha * (255U - sourceAlpha) / 255U;
    destination[channel] = static_cast<unsigned char>((source + destinationValue) / outputAlpha);
  }
  destination[3] = static_cast<unsigned char>(outputAlpha);
}

std::string utf8(char32_t codepoint) {
  if (codepoint <= 0x7f) return {static_cast<char>(codepoint)};
  if (codepoint <= 0x7ff) return {static_cast<char>(0xc0 | (codepoint >> 6)), static_cast<char>(0x80 | (codepoint & 0x3f))};
  if (codepoint <= 0xffff) return {static_cast<char>(0xe0 | (codepoint >> 12)), static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)), static_cast<char>(0x80 | (codepoint & 0x3f))};
  return {static_cast<char>(0xf0 | (codepoint >> 18)), static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)), static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)), static_cast<char>(0x80 | (codepoint & 0x3f))};
}
}

SkinTextAtlasBuildResult buildSkinTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasFontBytes> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs) {
  SkinTextAtlasBuildResult result;
  if (id == 0 || !canonicalizeSkinTextAtlasKey(key) || faces.empty() ||
      codepoints.size() > SkinResourcePolicy::maximumGlyphs ||
      pairs.size() > SkinResourcePolicy::maximumKerningPairs) {
    result.error = "font atlas key or limits are invalid";
    return result;
  }
  if (!text_runtime::acquire()) {
    result.error = "SDL_ttf initialization failed";
    return result;
  }
  struct RuntimeRelease { ~RuntimeRelease() { text_runtime::release(); } } release;
  text_runtime::OperationGuard operation;
  std::vector<OpenFace> opened;
  opened.reserve(faces.size());
  for (const auto &face : faces) {
    if (face.encoded.empty() || face.encoded.size() > INT_MAX) {
      result.error = "font bytes are invalid";
      return result;
    }
    SDL_RWops *rw = SDL_RWFromConstMem(face.encoded.data(), static_cast<int>(face.encoded.size()));
    if (!rw) { result.error = "font stream could not be opened"; return result; }
    OpenFace item;
    item.font = TTF_OpenFontRW(rw, 1, key.pointSize);
    if (!item.font) { result.error = "font bytes are not a supported TTF or OTF"; return result; }
    opened.push_back(std::move(item));
  }

  std::vector<GlyphBitmap> glyphs;
  glyphs.reserve(codepoints.size());
  std::size_t temporaryGlyphBytes = 0;
  for (char32_t codepoint : codepoints) {
    if (codepoint == U'\n' || codepoint == U'\r') continue;
    std::size_t faceIndex = opened.size();
    for (std::size_t index = 0; index < opened.size(); ++index)
      if (TTF_GlyphIsProvided32(opened[index].font, static_cast<Uint32>(codepoint))) { faceIndex = index; break; }
    if (faceIndex == opened.size()) { result.error = "font atlas has an unsupported glyph"; return result; }
    int minX = 0, maxX = 0, minY = 0, maxY = 0, advance = 0;
    if (TTF_GlyphMetrics32(opened[faceIndex].font, static_cast<Uint32>(codepoint), &minX, &maxX, &minY, &maxY, &advance) != 0) {
      result.error = "font glyph metrics failed"; return result;
    }
    const int outline = static_cast<int>(std::ceil(key.outlineWidth));
    const int shadowX = static_cast<int>(std::round(key.shadowOffsetX));
    const int shadowY = static_cast<int>(std::round(key.shadowOffsetY));
    const int smoothRadius = std::min(4, static_cast<int>(std::ceil(key.shadowSmoothness)));
    const int padding = std::max({2, outline + 1, std::abs(shadowX) + smoothRadius + 1, std::abs(shadowY) + smoothRadius + 1});
    const auto estimatedWidth = static_cast<std::int64_t>(maxX) - minX + 2LL * padding;
    const auto estimatedHeight = static_cast<std::int64_t>(maxY) - minY + 2LL * padding;
    if (estimatedWidth <= 0 || estimatedHeight <= 0 || estimatedWidth > SkinResourcePolicy::maximumDimension ||
        estimatedHeight > SkinResourcePolicy::maximumDimension ||
        static_cast<std::uint64_t>(estimatedWidth) * static_cast<std::uint64_t>(estimatedHeight) * 4U > SkinResourcePolicy::maximumAtlasBytes - temporaryGlyphBytes) {
      result.error = "font glyph metrics exceed atlas preparation limits"; return result;
    }
    SDL_Surface *raw = TTF_RenderGlyph32_Blended(opened[faceIndex].font, static_cast<Uint32>(codepoint), SDL_Color{255,255,255,255});
    if (!raw) { result.error = "font glyph rasterization failed"; return result; }
    SDL_Surface *surface = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!surface) { result.error = "font glyph conversion failed"; return result; }
    if (surface->w <= 0 || surface->h <= 0 || surface->w > SkinResourcePolicy::maximumDimension - 2 * padding ||
        surface->h > SkinResourcePolicy::maximumDimension - 2 * padding) {
      SDL_FreeSurface(surface); result.error = "font glyph dimensions exceed limits"; return result;
    }
    const std::size_t glyphBytes = static_cast<std::size_t>(surface->w + 2 * padding) *
                                   static_cast<std::size_t>(surface->h + 2 * padding) * 4U;
    if (glyphBytes > SkinResourcePolicy::maximumAtlasBytes - temporaryGlyphBytes) {
      SDL_FreeSurface(surface); result.error = "font temporary glyph bytes exceed atlas limit"; return result;
    }
    temporaryGlyphBytes += glyphBytes;
    GlyphBitmap glyph{.codepoint=codepoint, .face=faceIndex, .bearingX=minX, .bearingY=maxY, .advance=advance,
                      .width=surface->w + 2 * padding, .height=surface->h + 2 * padding, .padding=padding,
                      .rgba=std::vector<unsigned char>(static_cast<std::size_t>(surface->w + 2 * padding) * static_cast<std::size_t>(surface->h + 2 * padding) * 4U, 0)};
    if (SDL_LockSurface(surface) != 0) { SDL_FreeSurface(surface); result.error = "font glyph surface lock failed"; return result; }
    const auto *pixels = static_cast<const unsigned char *>(surface->pixels);
    const auto put = [&](int targetX, int targetY, const std::array<std::uint8_t,4> color, unsigned char alpha) {
      if (targetX < 0 || targetY < 0 || targetX >= glyph.width || targetY >= glyph.height) return;
      alphaOver(glyph.rgba.data() + (static_cast<std::size_t>(targetY) * glyph.width + targetX) * 4U, color, alpha);
    };
    for (int y = 0; y < surface->h; ++y) for (int x = 0; x < surface->w; ++x) {
      const unsigned char alpha = pixels[static_cast<std::size_t>(y) * surface->pitch + static_cast<std::size_t>(x) * 4U + 3U];
      if (alpha == 0) continue;
      put(x + padding + shadowX, y + padding + shadowY, key.shadowRgba, alpha);
      if (smoothRadius > 0) {
        const unsigned char smoothAlpha = static_cast<unsigned char>(
            static_cast<double>(alpha) * std::min(1.0, key.shadowSmoothness) /
            static_cast<double>((2 * smoothRadius + 1) * (2 * smoothRadius + 1)));
        for (int oy = -smoothRadius; oy <= smoothRadius; ++oy)
          for (int ox = -smoothRadius; ox <= smoothRadius; ++ox)
            if (ox != 0 || oy != 0) put(x + padding + shadowX + ox, y + padding + shadowY + oy, key.shadowRgba, smoothAlpha);
      }
      for (int oy = -outline; oy <= outline; ++oy) for (int ox = -outline; ox <= outline; ++ox)
        if (ox * ox + oy * oy <= outline * outline) put(x + padding + ox, y + padding + oy, key.outlineRgba, alpha);
      put(x + padding, y + padding, {255,255,255,255}, alpha);
    }
    SDL_UnlockSurface(surface);
    SDL_FreeSurface(surface);
    glyphs.push_back(std::move(glyph));
  }
  constexpr int atlasWidth = 1024;
  int cursorX = 1, cursorY = 1, rowHeight = 0;
  std::map<char32_t, SkinPreparedGlyphMetrics> metrics;
  for (const auto &glyph : glyphs) {
    if (glyph.width + 2 > atlasWidth) { result.error = "font glyph exceeds atlas width"; return result; }
    if (cursorX + glyph.width + 1 > atlasWidth) { cursorX = 1; cursorY += rowHeight + 1; rowHeight = 0; }
    if (cursorY + glyph.height + 1 > SkinResourcePolicy::maximumDimension) { result.error = "font atlas exceeds one-page limit"; return result; }
    metrics.emplace(glyph.codepoint, SkinPreparedGlyphMetrics{.region={.x=cursorX,.y=cursorY,.w=glyph.width,.h=glyph.height}, .bearingX=glyph.bearingX - glyph.padding, .bearingY=glyph.bearingY + glyph.padding, .advance=glyph.advance});
    cursorX += glyph.width + 1;
    rowHeight = std::max(rowHeight, glyph.height);
  }
  const int atlasHeight = std::max(1, cursorY + rowHeight + 1);
  const std::size_t byteCount = static_cast<std::size_t>(atlasWidth) * atlasHeight * 4U;
  if (byteCount > SkinResourcePolicy::maximumAtlasBytes) { result.error = "font atlas exceeds byte limit"; return result; }
  auto rgba = std::make_shared<std::vector<unsigned char>>(byteCount, 0);
  for (const auto &glyph : glyphs) {
    const auto &region = metrics.at(glyph.codepoint).region;
    for (int y = 0; y < glyph.height; ++y)
      std::copy_n(glyph.rgba.data() + static_cast<std::size_t>(y) * glyph.width * 4U,
                  static_cast<std::size_t>(glyph.width) * 4U,
                  rgba->data() + (static_cast<std::size_t>(region.y + y) * atlasWidth + region.x) * 4U);
  }
  std::map<std::pair<char32_t, char32_t>, int> kerning;
  for (const auto &[left, right] : pairs) {
    const auto leftGlyph = std::find_if(glyphs.begin(), glyphs.end(), [left] (const GlyphBitmap &item) { return item.codepoint == left; });
    const auto rightGlyph = std::find_if(glyphs.begin(), glyphs.end(), [right] (const GlyphBitmap &item) { return item.codepoint == right; });
    if (leftGlyph == glyphs.end() || rightGlyph == glyphs.end() || leftGlyph->face != rightGlyph->face) {
      kerning.emplace(std::pair{left, right}, 0);
      continue;
    }
    int amount = TTF_GetFontKerningSizeGlyphs32(opened[leftGlyph->face].font, static_cast<Uint32>(left), static_cast<Uint32>(right));
    kerning.emplace(std::pair{left, right}, amount);
  }
  result.atlas = SkinPreparedGlyphAtlas{.id=id, .key=std::move(key),
      .pixels={.width=atlasWidth,.height=atlasHeight,.rgba=std::move(rgba)},
      .glyphs=std::move(metrics), .kerning=std::move(kerning),
      .ascent=TTF_FontAscent(opened.front().font), .descent=TTF_FontDescent(opened.front().font),
      .lineHeight=TTF_FontHeight(opened.front().font)};
  return result;
}
} // namespace skin
#endif
