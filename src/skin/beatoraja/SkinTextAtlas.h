#pragma once

#include "SkinBitmapFontParser.h"
#include "SkinResourceCatalog.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace skin {

// TTF style fields are a bounded enhancement over Beatoraja's incremental
// renderer: finite values are quantized before they become map keys.
[[nodiscard]] bool canonicalizeSkinTextAtlasKey(
    SkinTextAtlasKey &, SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{}) noexcept;
[[nodiscard]] std::string stableFallbackChainDigest(
    SkinResourceId primary, int primaryType,
    const std::vector<SkinFontFallbackResource> &fallbacks);
[[nodiscard]] bool appendStableFallbackChainEntry(
    std::string &digest, std::string_view normalizedVirtualPath, int type,
    SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});

struct SkinTextAtlasFontBytes {
  std::vector<std::byte> encoded;
};

struct SkinTextAtlasBitmapPage {
  std::string physicalKey;
  std::optional<image_decode::DecodedImageData> pixels;
};

struct SkinTextAtlasBitmapFace {
  SkinParsedBitmapFont font;
  std::vector<SkinTextAtlasBitmapPage> pages;
};

struct SkinTextAtlasBuildResult {
  std::optional<SkinPreparedGlyphAtlas> atlas;
  std::string error;
};

// All font bytes are package-owned values supplied by the caller. The builder
// opens/uses/closes SDL_ttf fonts within this call and returns only value-owned
// pixels and metrics, so render code never needs a font handle or path. A
// reservation callback is invoked once, after complete validation and before
// the first effect blend; callers may retain that reservation if later work is
// cancelled or its prepared atlas is rejected by session accounting.
[[nodiscard]] SkinTextAtlasBuildResult buildSkinTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasFontBytes> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs,
    SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{},
    std::size_t maximumPaintBlendOperations =
        SkinResourcePolicy::maximumScalableFontPaintBlendOperations,
    const std::function<bool()> &cancellationRequested = {},
    const std::function<bool(std::size_t)> &reservePaintBlendOperations = {});

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
void resetSkinTextAtlasPaintBlendOperationsForTesting() noexcept;
[[nodiscard]] std::size_t
skinTextAtlasPaintBlendOperationsForTesting() noexcept;
#endif

[[nodiscard]] SkinTextAtlasBuildResult buildSkinBitmapTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasBitmapFace> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs,
    SkinSafetyPolicy safetyPolicy = SkinSafetyPolicy{});

} // namespace skin
