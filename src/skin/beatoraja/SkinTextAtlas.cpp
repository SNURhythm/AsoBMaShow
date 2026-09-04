#include "SkinTextAtlas.h"
#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "../../view/SdlTtfRuntime.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace skin {
namespace {
bool quantize(double &value, const SkinSafetyPolicy &safetyPolicy) noexcept {
  if (!std::isfinite(value) ||
      (safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit) &&
       std::abs(value) > 4096.0)) {
    return false;
  }
  value = std::round(value * 64.0) / 64.0;
  return true;
}
}
bool canonicalizeSkinTextAtlasKey(SkinTextAtlasKey &key,
                                  SkinSafetyPolicy safetyPolicy) noexcept {
  return key.font != 0 && key.pointSize > 0 &&
         key.pointSize <= skinResourceDimensionLimit(safetyPolicy) &&
         !key.fallbackChainDigest.empty() &&
         key.fallbackChainDigest.size() <=
             skinResourceLimit(safetyPolicy,
                               SkinResourcePolicy::maximumFallbackChainDigestBytes) &&
         key.outlineWidth >= 0.0 &&
         (!safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit) ||
          key.outlineWidth <=
              SkinResourcePolicy::maximumScalableFontOutlineWidth) &&
         key.shadowSmoothness >= 0.0 &&
         quantize(key.outlineWidth, safetyPolicy) &&
         quantize(key.shadowOffsetX, safetyPolicy) &&
         quantize(key.shadowOffsetY, safetyPolicy) &&
         quantize(key.shadowSmoothness, safetyPolicy);
}
std::string stableFallbackChainDigest(SkinResourceId primary, int primaryType,
                                      const std::vector<SkinFontFallbackResource> &fallbacks) {
  std::string result = std::to_string(primary) + ':' +
                       std::to_string(primaryType);
  for (const auto &fallback : fallbacks) {
    if (fallback.virtualPath.empty()) continue;
    if (!appendStableFallbackChainEntry(result, fallback.virtualPath,
                                        fallback.type)) {
      return {};
    }
  }
  return result;
}

bool appendStableFallbackChainEntry(
    std::string &digest, std::string_view normalizedVirtualPath, int type,
    SkinSafetyPolicy safetyPolicy) {
  const std::string pathSize = std::to_string(normalizedVirtualPath.size());
  const std::string typeText = std::to_string(type);
  const auto append = [&](std::string_view value) {
    const std::size_t limit = skinResourceLimit(
        safetyPolicy, SkinResourcePolicy::maximumFallbackChainDigestBytes);
    if (digest.size() > limit || value.size() > limit - digest.size()) {
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
  int layoutOffsetY = 0;
  int contentWidth = 0;
  int contentHeight = 0;
  std::vector<unsigned char> alpha;
};

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
std::atomic_size_t paintBlendOperationsForTesting{0};
#endif

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

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
void resetSkinTextAtlasPaintBlendOperationsForTesting() noexcept {
  paintBlendOperationsForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinTextAtlasPaintBlendOperationsForTesting() noexcept {
  return paintBlendOperationsForTesting.load(std::memory_order_relaxed);
}
#endif

SkinTextAtlasBuildResult buildSkinTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasFontBytes> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs,
    SkinSafetyPolicy safetyPolicy,
    std::size_t maximumPaintBlendOperations,
    const std::function<bool()> &cancellationRequested,
    const std::function<bool(std::size_t)> &reservePaintBlendOperations) {
  SkinTextAtlasBuildResult result;
  if (!safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit)) {
    // Pinned SkinTextFont does not apply the bitmap-font distance-field paint
    // fields while generating its FreeType glyph pages. Its only text effect
    // is a second offset layout drawn by the renderer.
    key.outlineRgba = {255, 255, 255, 0};
    key.outlineWidth = 0.0;
    key.shadowRgba = {255, 255, 255, 0};
    key.shadowOffsetX = 0.0;
    key.shadowOffsetY = 0.0;
    key.shadowSmoothness = 0.0;
  }
  if (safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit)) {
    maximumPaintBlendOperations = std::min(
        maximumPaintBlendOperations,
        SkinResourcePolicy::maximumScalableFontPaintBlendOperations);
  }
  if (safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit) &&
      std::isfinite(key.outlineWidth) &&
      key.outlineWidth >
          SkinResourcePolicy::maximumScalableFontOutlineWidth) {
    result.error = "font outline width exceeds atlas preparation limit";
    return result;
  }
  if (id == 0 || !canonicalizeSkinTextAtlasKey(key, safetyPolicy) ||
      faces.empty() ||
      codepoints.size() >
          skinResourceLimit(safetyPolicy, SkinResourcePolicy::maximumGlyphs) ||
      pairs.size() > skinResourceLimit(
                         safetyPolicy,
                         SkinResourcePolicy::maximumKerningPairs)) {
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

  constexpr std::u32string_view capCharacters =
      U"MNBDCEFKAGHIJLOPQRSTUVWXYZ";
  int capHeight = 0;
  for (const char32_t codepoint : capCharacters) {
    if (!TTF_GlyphIsProvided32(opened.front().font,
                               static_cast<Uint32>(codepoint))) {
      continue;
    }
    int minX = 0;
    int maxX = 0;
    int minY = 0;
    int maxY = 0;
    int advance = 0;
    if (TTF_GlyphMetrics32(opened.front().font,
                           static_cast<Uint32>(codepoint), &minX, &maxX,
                           &minY, &maxY, &advance) == 0) {
      capHeight = maxY - minY;
      break;
    }
  }
  if (capHeight <= 0) {
    capHeight = std::max(1, TTF_FontHeight(opened.front().font));
  }
  if (capHeight > skinResourceDimensionLimit(safetyPolicy)) {
    result.error = "font cap height is unavailable";
    return result;
  }
  const int primaryAscent = TTF_FontAscent(opened.front().font);
  const int layoutAscent = primaryAscent - capHeight;
  const int outline = static_cast<int>(std::ceil(key.outlineWidth));
  const bool outlineActive = key.outlineRgba[3] != 0;
  const bool shadowActive = key.shadowRgba[3] != 0;
  std::vector<std::pair<int, int>> outlineOffsets;
  if (outlineActive) {
    for (int oy = -outline; oy <= outline; ++oy) {
      for (int ox = -outline; ox <= outline; ++ox) {
        if (ox * ox + oy * oy <= outline * outline) {
          outlineOffsets.emplace_back(ox, oy);
        }
      }
    }
  }

  std::vector<GlyphBitmap> glyphs;
  glyphs.reserve(codepoints.size());
  std::size_t temporaryGlyphBytes = 0;
  std::size_t paintBlendOperations = 0;
  const int shadowX = shadowActive
                          ? static_cast<int>(std::round(key.shadowOffsetX))
                          : 0;
  const int shadowY = shadowActive
                          ? static_cast<int>(std::round(key.shadowOffsetY))
                          : 0;
  const int smoothRadius =
      shadowActive
          ? std::min(4, static_cast<int>(std::ceil(key.shadowSmoothness)))
          : 0;
  const std::size_t smoothingOperations =
      smoothRadius == 0
          ? 0
          : static_cast<std::size_t>(2 * smoothRadius + 1) *
                    static_cast<std::size_t>(2 * smoothRadius + 1) -
                1U;
  const std::size_t operationsPerOpaquePixel =
      outlineOffsets.size() +
      (shadowActive ? 1U + smoothingOperations : 0U);
  const auto paintLimitError = [&] {
    return shadowActive ? "font paint work exceeds atlas preparation limit"
                        : "font outline work exceeds atlas preparation limit";
  };
  for (char32_t codepoint : codepoints) {
    if (cancellationRequested && cancellationRequested()) {
      result.error = "font atlas preparation cancelled";
      return result;
    }
    if (codepoint == U'\n' || codepoint == U'\r') continue;
    std::size_t faceIndex = opened.size();
    char32_t sourceCodepoint = codepoint;
    for (std::size_t index = 0; index < opened.size(); ++index)
      if (TTF_GlyphIsProvided32(opened[index].font, static_cast<Uint32>(codepoint))) { faceIndex = index; break; }
    if (faceIndex == opened.size() &&
        !safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit)) {
      constexpr std::u32string_view missing = U"\u25a1\u25a2\u2610\u25a0?";
      for (const char32_t candidate : missing) {
        for (std::size_t index = 0; index < opened.size(); ++index) {
          if (TTF_GlyphIsProvided32(opened[index].font,
                                    static_cast<Uint32>(candidate))) {
            faceIndex = index;
            sourceCodepoint = candidate;
            break;
          }
        }
        if (faceIndex != opened.size()) break;
      }
    }
    if (faceIndex == opened.size() &&
        safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit)) {
      result.error = "font atlas has an unsupported glyph";
      return result;
    }
    const bool syntheticMissing = faceIndex == opened.size();
    if (syntheticMissing) faceIndex = 0;
    int minX = 0, maxX = 0, minY = 0, maxY = 0, advance = 0;
    const int missingHeight = std::max(8, key.pointSize);
    const int missingWidth = std::max(
        6, static_cast<int>(std::lround(missingHeight * 0.75)));
    const int missingStroke = std::max(1, missingHeight / 12);
    if (syntheticMissing) {
      maxX = missingWidth;
      maxY = missingHeight;
      advance = missingWidth + missingStroke;
    } else if (TTF_GlyphMetrics32(
                   opened[faceIndex].font,
                   static_cast<Uint32>(sourceCodepoint), &minX, &maxX, &minY,
                   &maxY, &advance) != 0) {
      result.error = "font glyph metrics failed"; return result;
    }
    const int padding = std::max({2, outline + 1, std::abs(shadowX) + smoothRadius + 1, std::abs(shadowY) + smoothRadius + 1});
    const auto estimatedWidth = static_cast<std::int64_t>(maxX) - minX + 2LL * padding;
    const auto estimatedHeight = static_cast<std::int64_t>(maxY) - minY + 2LL * padding;
    const std::size_t maximumAtlasBytes = skinResourceLimit(
        safetyPolicy, SkinResourcePolicy::maximumAtlasBytes);
    if (estimatedWidth <= 0 || estimatedHeight <= 0 ||
        estimatedWidth > skinResourceDimensionLimit(safetyPolicy) ||
        estimatedHeight > skinResourceDimensionLimit(safetyPolicy) ||
        static_cast<std::uint64_t>(estimatedWidth) *
                static_cast<std::uint64_t>(estimatedHeight) *
                4U >
            maximumAtlasBytes - temporaryGlyphBytes) {
      result.error = "font glyph metrics exceed atlas preparation limits"; return result;
    }
    SDL_Surface *surface = nullptr;
    if (syntheticMissing) {
      surface = SDL_CreateRGBSurfaceWithFormat(
          0, missingWidth, missingHeight, 32, SDL_PIXELFORMAT_RGBA32);
      if (surface != nullptr) {
        SDL_FillRect(surface, nullptr,
                     SDL_MapRGBA(surface->format, 0, 0, 0, 0));
        if (SDL_LockSurface(surface) == 0) {
          const Uint32 white =
              SDL_MapRGBA(surface->format, 255, 255, 255, 255);
          for (int y = 0; y < missingHeight; ++y) {
            auto *row = reinterpret_cast<Uint32 *>(
                static_cast<unsigned char *>(surface->pixels) +
                static_cast<std::size_t>(y) * surface->pitch);
            for (int x = 0; x < missingWidth; ++x) {
              if (x < missingStroke || y < missingStroke ||
                  x >= missingWidth - missingStroke ||
                  y >= missingHeight - missingStroke) {
                row[x] = white;
              }
            }
          }
          SDL_UnlockSurface(surface);
        }
      }
    } else {
      SDL_Surface *raw = TTF_RenderGlyph32_Blended(
          opened[faceIndex].font, static_cast<Uint32>(sourceCodepoint),
          SDL_Color{255,255,255,255});
      if (!raw) { result.error = "font glyph rasterization failed"; return result; }
      surface = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
      SDL_FreeSurface(raw);
    }
    if (!surface) { result.error = "font glyph conversion failed"; return result; }
    if (surface->w <= 0 || surface->h <= 0 ||
        surface->w > skinResourceDimensionLimit(safetyPolicy) ||
        surface->h > skinResourceDimensionLimit(safetyPolicy)) {
      SDL_FreeSurface(surface); result.error = "font glyph dimensions exceed limits"; return result;
    }
    if (SDL_LockSurface(surface) != 0) { SDL_FreeSurface(surface); result.error = "font glyph surface lock failed"; return result; }
    const auto *pixels = static_cast<const unsigned char *>(surface->pixels);
    int alphaMinX = surface->w;
    int alphaMinY = surface->h;
    int alphaMaxX = -1;
    int alphaMaxY = -1;
    std::size_t opaquePixels = 0;
    for (int y = 0; y < surface->h; ++y) {
      if (cancellationRequested && cancellationRequested()) {
        SDL_UnlockSurface(surface);
        SDL_FreeSurface(surface);
        result.error = "font atlas preparation cancelled";
        return result;
      }
      for (int x = 0; x < surface->w; ++x) {
        const unsigned char alpha =
            pixels[static_cast<std::size_t>(y) * surface->pitch +
                   static_cast<std::size_t>(x) * 4U + 3U];
        if (alpha == 0) {
          continue;
        }
        ++opaquePixels;
        alphaMinX = std::min(alphaMinX, x);
        alphaMinY = std::min(alphaMinY, y);
        alphaMaxX = std::max(alphaMaxX, x);
        alphaMaxY = std::max(alphaMaxY, y);
      }
    }
    if (operationsPerOpaquePixel != 0 &&
        (paintBlendOperations > maximumPaintBlendOperations ||
         opaquePixels >
             (maximumPaintBlendOperations - paintBlendOperations) /
                 operationsPerOpaquePixel)) {
      SDL_UnlockSurface(surface);
      SDL_FreeSurface(surface);
      result.error = paintLimitError();
      return result;
    }
    paintBlendOperations += opaquePixels * operationsPerOpaquePixel;
    const bool hasPixels = alphaMaxX >= alphaMinX && alphaMaxY >= alphaMinY;
    const int glyphPadding = hasPixels ? padding : 0;
    const int contentWidth =
        hasPixels ? alphaMaxX - alphaMinX + 1 : std::max(1, advance);
    const int contentHeight = hasPixels ? alphaMaxY - alphaMinY + 1 : 1;
    const int glyphWidth = contentWidth + 2 * glyphPadding;
    const int glyphHeight = contentHeight + 2 * glyphPadding;
    if (glyphWidth <= 0 || glyphHeight <= 0 ||
        glyphWidth > skinResourceDimensionLimit(safetyPolicy) ||
        glyphHeight > skinResourceDimensionLimit(safetyPolicy)) {
      SDL_UnlockSurface(surface);
      SDL_FreeSurface(surface);
      result.error = "font cropped glyph dimensions exceed limits";
      return result;
    }
    const std::size_t glyphBytes = static_cast<std::size_t>(glyphWidth) *
                                   static_cast<std::size_t>(glyphHeight) * 4U;
    if (glyphBytes > skinResourceLimit(
                         safetyPolicy,
                         SkinResourcePolicy::maximumAtlasBytes) -
                         temporaryGlyphBytes) {
      SDL_UnlockSurface(surface);
      SDL_FreeSurface(surface); result.error = "font temporary glyph bytes exceed atlas limit"; return result;
    }
    temporaryGlyphBytes += glyphBytes;
    std::vector<unsigned char> alpha(
        static_cast<std::size_t>(contentWidth) * contentHeight, 0);
    for (int y = alphaMinY; hasPixels && y <= alphaMaxY; ++y) {
      if (cancellationRequested && cancellationRequested()) {
        SDL_UnlockSurface(surface);
        SDL_FreeSurface(surface);
        result.error = "font atlas preparation cancelled";
        return result;
      }
      for (int x = alphaMinX; x <= alphaMaxX; ++x) {
        alpha[static_cast<std::size_t>(y - alphaMinY) * contentWidth +
              static_cast<std::size_t>(x - alphaMinX)] =
            pixels[static_cast<std::size_t>(y) * surface->pitch +
                   static_cast<std::size_t>(x) * 4U + 3U];
      }
    }
    SDL_UnlockSurface(surface);
    SDL_FreeSurface(surface);
    const int selectedFaceAscent = TTF_FontAscent(opened[faceIndex].font);
    GlyphBitmap glyph{
        .codepoint = codepoint,
        .face = faceIndex,
        .bearingX = hasPixels ? minX : 0,
        .bearingY = hasPixels ? maxY : 0,
        .advance = advance,
        .width = glyphWidth,
        .height = glyphHeight,
        .padding = glyphPadding,
        .layoutOffsetY =
            hasPixels ? layoutAscent + maxY - contentHeight -
                            selectedFaceAscent - glyphPadding
                      : 0,
        .contentWidth = contentWidth,
        .contentHeight = contentHeight,
        .alpha = std::move(alpha)};
    glyphs.push_back(std::move(glyph));
  }
  // FreeTypeFontGenerator's pinned incremental packer is 1024x1024 and adds
  // pages as required. Retain that page boundary instead of turning a second
  // page into a host-only load failure.
  constexpr int atlasWidth = 1024;
  constexpr int atlasPageHeight = 1024;
  int cursorX = 1, cursorY = 1, rowHeight = 0;
  std::size_t pageIndex = 0;
  std::vector<int> pageHeights(1, 1);
  std::map<char32_t, SkinPreparedGlyphMetrics> metrics;
  for (const auto &glyph : glyphs) {
    if (cancellationRequested && cancellationRequested()) {
      result.error = "font atlas preparation cancelled";
      return result;
    }
    if (glyph.width + 2 > atlasWidth ||
        glyph.height + 2 > atlasPageHeight) {
      result.error = "font glyph exceeds the pinned packer page";
      return result;
    }
    if (cursorX + glyph.width + 1 > atlasWidth) { cursorX = 1; cursorY += rowHeight + 1; rowHeight = 0; }
    if (cursorY + glyph.height + 1 > atlasPageHeight) {
      ++pageIndex;
      pageHeights.push_back(1);
      cursorX = 1;
      cursorY = 1;
      rowHeight = 0;
    }
    metrics.emplace(glyph.codepoint, SkinPreparedGlyphMetrics{
        .region = {.x = cursorX,
                   .y = cursorY,
                   .w = glyph.width,
                   .h = glyph.height},
        .bearingX = glyph.bearingX - glyph.padding,
        .bearingY = glyph.bearingY + glyph.padding,
        .advance = glyph.advance,
        .layoutOffsetY = glyph.layoutOffsetY,
        .page = pageIndex});
    cursorX += glyph.width + 1;
    rowHeight = std::max(rowHeight, glyph.height);
    pageHeights[pageIndex] =
        std::max(pageHeights[pageIndex], cursorY + rowHeight + 1);
  }
  std::size_t byteCount = 0;
  for (const int height : pageHeights) {
    const std::size_t pageBytes =
        static_cast<std::size_t>(atlasWidth) * height * 4U;
    if (pageBytes > skinResourceLimit(
                        safetyPolicy, SkinResourcePolicy::maximumAtlasBytes) -
                        byteCount) {
      result.error = "font atlas exceeds byte limit";
      return result;
    }
    byteCount += pageBytes;
  }
  if (byteCount > skinResourceLimit(safetyPolicy,
                                    SkinResourcePolicy::maximumAtlasBytes)) {
    result.error = "font atlas exceeds byte limit";
    return result;
  }
  std::vector<std::shared_ptr<std::vector<unsigned char>>> pagePixels;
  pagePixels.reserve(pageHeights.size());
  for (const int height : pageHeights) {
    pagePixels.push_back(std::make_shared<std::vector<unsigned char>>(
        static_cast<std::size_t>(atlasWidth) * height * 4U, 0));
  }
  std::map<std::pair<char32_t, char32_t>, int> kerning;
  for (const auto &[left, right] : pairs) {
    if (cancellationRequested && cancellationRequested()) {
      result.error = "font atlas preparation cancelled";
      return result;
    }
    const auto leftGlyph = std::find_if(glyphs.begin(), glyphs.end(), [left] (const GlyphBitmap &item) { return item.codepoint == left; });
    const auto rightGlyph = std::find_if(glyphs.begin(), glyphs.end(), [right] (const GlyphBitmap &item) { return item.codepoint == right; });
    if (leftGlyph == glyphs.end() || rightGlyph == glyphs.end() || leftGlyph->face != rightGlyph->face) {
      kerning.emplace(std::pair{left, right}, 0);
      continue;
    }
    int amount = TTF_GetFontKerningSizeGlyphs32(opened[leftGlyph->face].font, static_cast<Uint32>(left), static_cast<Uint32>(right));
    kerning.emplace(std::pair{left, right}, amount);
  }
  if (paintBlendOperations != 0 && reservePaintBlendOperations &&
      !reservePaintBlendOperations(paintBlendOperations)) {
    result.error = paintLimitError();
    return result;
  }
  for (const auto &glyph : glyphs) {
    const auto &glyphMetrics = metrics.at(glyph.codepoint);
    const auto &region = glyphMetrics.region;
    auto &rgba = *pagePixels[glyphMetrics.page];
    const auto put = [&](int targetX, int targetY,
                         const std::array<std::uint8_t, 4> color,
                         unsigned char alpha, bool charged) {
      if (targetX < 0 || targetY < 0 || targetX >= glyph.width ||
          targetY >= glyph.height) {
        return;
      }
      alphaOver(
          rgba.data() +
              (static_cast<std::size_t>(region.y + targetY) * atlasWidth +
               region.x + targetX) *
                  4U,
          color, alpha);
#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
      if (charged) {
        paintBlendOperationsForTesting.fetch_add(1,
                                                  std::memory_order_relaxed);
      }
#else
      (void)charged;
#endif
    };
    for (int y = 0; y < glyph.contentHeight; ++y) {
      if (cancellationRequested && cancellationRequested()) {
        result.error = "font atlas preparation cancelled";
        return result;
      }
      for (int x = 0; x < glyph.contentWidth; ++x) {
        const unsigned char alpha =
            glyph.alpha[static_cast<std::size_t>(y) * glyph.contentWidth + x];
        if (alpha == 0) continue;
        const int glyphX = x + glyph.padding;
        const int glyphY = y + glyph.padding;
        if (shadowActive) {
          put(glyphX + shadowX, glyphY + shadowY, key.shadowRgba, alpha,
              true);
          if (smoothRadius > 0) {
            const unsigned char smoothAlpha = static_cast<unsigned char>(
                static_cast<double>(alpha) *
                std::min(1.0, key.shadowSmoothness) /
                static_cast<double>((2 * smoothRadius + 1) *
                                    (2 * smoothRadius + 1)));
            for (int oy = -smoothRadius; oy <= smoothRadius; ++oy)
              for (int ox = -smoothRadius; ox <= smoothRadius; ++ox)
                if (ox != 0 || oy != 0)
                  put(glyphX + shadowX + ox, glyphY + shadowY + oy,
                      key.shadowRgba, smoothAlpha, true);
          }
        }
        for (const auto [ox, oy] : outlineOffsets)
          put(glyphX + ox, glyphY + oy, key.outlineRgba, alpha, true);
        put(glyphX, glyphY, {255,255,255,255}, alpha, false);
      }
    }
  }
  SkinPreparedGlyphAtlas atlas{.id=id, .key=std::move(key),
      .glyphs=std::move(metrics), .kerning=std::move(kerning),
      .ascent=primaryAscent, .capHeight=capHeight,
      .descent=TTF_FontDescent(opened.front().font),
      .lineHeight=TTF_FontHeight(opened.front().font),
      .paintBlendOperations=paintBlendOperations};
  if (pagePixels.size() == 1) {
    atlas.pixels = {.width = atlasWidth,
                    .height = pageHeights.front(),
                    .rgba = std::move(pagePixels.front())};
  } else {
    atlas.pages.reserve(pagePixels.size());
    for (std::size_t index = 0; index < pagePixels.size(); ++index) {
      atlas.pages.push_back(
          {.physicalKey = "scalable:" + std::to_string(id) + ":" +
                          std::to_string(index),
           .pixels = image_decode::DecodedImageData{
               .width = atlasWidth,
               .height = pageHeights[index],
               .rgba = std::move(pagePixels[index])}});
    }
  }
  result.atlas = std::move(atlas);
  return result;
}

SkinTextAtlasBuildResult buildSkinBitmapTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasBitmapFace> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs,
    SkinSafetyPolicy safetyPolicy) {
  SkinTextAtlasBuildResult result;
  if (id == 0 || !canonicalizeSkinTextAtlasKey(key, safetyPolicy) ||
      faces.empty() || codepoints.size() > skinResourceLimit(
                                             safetyPolicy,
                                             SkinResourcePolicy::maximumGlyphs) ||
      pairs.size() > skinResourceLimit(
                         safetyPolicy,
                         SkinResourcePolicy::maximumKerningPairs)) {
    result.error = "bitmap font atlas key or limits are invalid";
    return result;
  }

  std::vector<std::vector<std::size_t>> pageIndices(faces.size());
  std::vector<SkinPreparedGlyphPage> pages;
  for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
    const auto &face = faces[faceIndex];
    if (face.font.pagePaths.size() != face.pages.size() ||
        face.font.resource.originalSize <= 0 || face.font.lineHeight <= 0) {
      result.error = "bitmap font face or page count is invalid";
      return result;
    }
    pageIndices[faceIndex].reserve(face.pages.size());
    for (const auto &page : face.pages) {
      if (page.pixels &&
          (page.physicalKey.empty() ||
           !skinResourceDimensionsAllowed(page.pixels->width,
                                          page.pixels->height,
                                          page.pixels->byteSize(),
                                          safetyPolicy))) {
        result.error = "bitmap font page exceeds image limits";
        return result;
      }
      pageIndices[faceIndex].push_back(pages.size());
      pages.push_back({.physicalKey = page.physicalKey,
                       .pixels = page.pixels,
                       .bitmapFontType = face.font.resource.type});
    }
  }

  const auto pageFor = [&](std::size_t faceIndex,
                           int pageIndex) -> const SkinTextAtlasBitmapPage * {
    if (faceIndex >= faces.size() || pageIndex < 0 ||
        static_cast<std::size_t>(pageIndex) >= faces[faceIndex].pages.size()) {
      return nullptr;
    }
    const auto &page = faces[faceIndex].pages[static_cast<std::size_t>(pageIndex)];
    return page.pixels ? &page : nullptr;
  };
  const auto usableGlyph = [&](std::size_t faceIndex,
                               const SkinBitmapGlyph &glyph) {
    const auto *page = pageFor(faceIndex, glyph.page);
    if (page == nullptr) {
      return false;
    }
    const auto &pixels = *page->pixels;
    return glyph.region.x >= 0 && glyph.region.y >= 0 &&
           glyph.region.w >= 0 && glyph.region.h >= 0 &&
           glyph.region.x <= pixels.width - glyph.region.w &&
           glyph.region.y <= pixels.height - glyph.region.h;
  };

  const auto representativeYOffset = [](const SkinParsedBitmapFont &font) {
    constexpr std::u32string_view preferred = U"SPANOTHER[]M0";
    for (const char32_t codepoint : preferred) {
      const auto found = font.glyphs.find(codepoint);
      if (found != font.glyphs.end() && found->second.region.h > 0) {
        return found->second.yOffset;
      }
    }
    for (const auto &[codepoint, glyph] : font.glyphs) {
      (void)codepoint;
      if (glyph.region.h > 0) return glyph.yOffset;
    }
    return 0;
  };
  const int primaryYOffset = representativeYOffset(faces.front().font);
  struct SelectedGlyph {
    const SkinBitmapGlyph *glyph = nullptr;
    std::size_t face = 0;
    char32_t sourceCodepoint = 0;
  };
  const auto exact = [&](char32_t codepoint) -> SelectedGlyph {
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
      const auto found = faces[faceIndex].font.glyphs.find(codepoint);
      if (found != faces[faceIndex].font.glyphs.end() &&
          usableGlyph(faceIndex, found->second)) {
        return {.glyph = &found->second,
                .face = faceIndex,
                .sourceCodepoint = codepoint};
      }
    }
    return {};
  };
  const auto select = [&](char32_t codepoint) {
    auto selected = exact(codepoint);
    if (selected.glyph) return selected;
    if (codepoint == U'\u301c') selected = exact(U'\uff5e');
    else if (codepoint == U'\uff5e') selected = exact(U'\u301c');
    if (selected.glyph) return selected;
    constexpr std::u32string_view missing = U"\u25a1\u25a2\u2610\u25a0?";
    for (std::size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
      for (const char32_t candidate : missing) {
        const auto found = faces[faceIndex].font.glyphs.find(candidate);
        if (found != faces[faceIndex].font.glyphs.end() &&
            usableGlyph(faceIndex, found->second)) {
          return SelectedGlyph{.glyph = &found->second,
                               .face = faceIndex,
                               .sourceCodepoint = candidate};
        }
      }
    }
    return SelectedGlyph{};
  };

  std::map<char32_t, SkinPreparedGlyphMetrics> metrics;
  std::map<char32_t, SelectedGlyph> selections;
  for (const char32_t codepoint : codepoints) {
    if (codepoint == U'\r' || codepoint == U'\n') continue;
    const auto selected = select(codepoint);
    if (!selected.glyph) {
      continue;
    }
    const auto &glyph = *selected.glyph;
    const int yOffsetAdjust =
        representativeYOffset(faces[selected.face].font) - primaryYOffset;
    // The renderer anchors the baseline at the destination top edge and
    // treats the authored rect as bottom-origin. A BMFont glyph's ink top
    // sits `yOffset` below that baseline, so its bottom edge is
    // `yOffset + region.h` below the baseline. Placing the rect bottom there
    // mirrors Beatoraja's BitmapFont layout exactly.
    metrics.emplace(
        codepoint,
        SkinPreparedGlyphMetrics{
            .region = glyph.region,
            .bearingX = glyph.xOffset,
            .bearingY = -glyph.yOffset,
            .advance = glyph.xAdvance,
            .layoutOffsetY = -glyph.yOffset - yOffsetAdjust -
                             glyph.region.h,
            .page = pageIndices[selected.face]
                               [static_cast<std::size_t>(glyph.page)],
            .bitmapFontType = faces[selected.face].font.resource.type});
    selections.emplace(codepoint, selected);
  }

  std::map<std::pair<char32_t, char32_t>, int> kerning;
  for (const auto &[left, right] : pairs) {
    const auto leftSelection = selections.find(left);
    const auto rightSelection = selections.find(right);
    if (leftSelection == selections.end() ||
        rightSelection == selections.end()) {
      continue;
    }
    int amount = 0;
    if (leftSelection->second.face == rightSelection->second.face &&
        leftSelection->second.sourceCodepoint == left &&
        rightSelection->second.sourceCodepoint == right) {
      const auto found = faces[leftSelection->second.face].font.kerning.find(
          {left, right});
      if (found != faces[leftSelection->second.face].font.kerning.end()) {
        amount = found->second;
      }
    }
    kerning.emplace(std::pair{left, right}, amount);
  }

  int capHeight = 0;
  constexpr std::u32string_view caps = U"MNBDCEFKAGHIJLOPQRSTUVWXYZ";
  for (const char32_t codepoint : caps) {
    const auto found = faces.front().font.glyphs.find(codepoint);
    if (found != faces.front().font.glyphs.end()) {
      capHeight = found->second.region.h;
      break;
    }
  }
  if (capHeight <= 0) capHeight = faces.front().font.lineHeight;
  int pageWidth = faces.front().font.pageWidth;
  int pageHeight = faces.front().font.pageHeight;
  if (!faces.front().font.auxiliaryMetricsComplete) {
    const auto available = std::ranges::find_if(
        faces.front().pages,
        [](const SkinTextAtlasBitmapPage &page) { return page.pixels.has_value(); });
    if (available != faces.front().pages.end()) {
      pageWidth = available->pixels->width;
      pageHeight = available->pixels->height;
    }
  }
  pageWidth = std::max(1, pageWidth);
  pageHeight = std::max(1, pageHeight);
  result.atlas = SkinPreparedGlyphAtlas{
      .id = id,
      .key = std::move(key),
      .pages = std::move(pages),
      .glyphs = std::move(metrics),
      .kerning = std::move(kerning),
      .ascent = faces.front().font.base - faces.front().font.lineHeight,
      .capHeight = capHeight,
      .descent = faces.front().font.base - faces.front().font.lineHeight,
      .lineHeight = faces.front().font.lineHeight,
      .bitmapFont = true,
      .bitmapFontType = faces.front().font.resource.type,
      .originalSize = faces.front().font.resource.originalSize,
      .pageWidth = pageWidth,
      .pageHeight = pageHeight,
      .layoutKind = faces.front().font.lr2Font
                        ? SkinTextLayoutKind::Lr2Image
                        : SkinTextLayoutKind::Bitmap,
      .margin = faces.front().font.margin};
  return result;
}
} // namespace skin
#endif
