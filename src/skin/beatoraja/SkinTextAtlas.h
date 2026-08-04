#pragma once

#include "SkinResourceCatalog.h"

#include <map>
#include <set>
#include <string>

namespace skin {

// TTF style fields are a bounded enhancement over Beatoraja's incremental
// renderer: finite values are quantized before they become map keys.
[[nodiscard]] bool canonicalizeSkinTextAtlasKey(SkinTextAtlasKey &) noexcept;
[[nodiscard]] std::string stableFallbackChainDigest(
    SkinResourceId primary, int primaryType,
    const std::vector<SkinFontFallbackResource> &fallbacks);

struct SkinTextAtlasFontBytes {
  std::vector<std::byte> encoded;
};

struct SkinTextAtlasBuildResult {
  std::optional<SkinPreparedGlyphAtlas> atlas;
  std::string error;
};

// All font bytes are package-owned values supplied by the caller. The builder
// opens/uses/closes SDL_ttf fonts within this call and returns only value-owned
// pixels and metrics, so render code never needs a font handle or path.
[[nodiscard]] SkinTextAtlasBuildResult buildSkinTextAtlas(
    SkinTextAtlasId id, SkinTextAtlasKey key,
    const std::vector<SkinTextAtlasFontBytes> &faces,
    const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs);

} // namespace skin
