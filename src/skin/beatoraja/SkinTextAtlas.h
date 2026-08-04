#pragma once

#include "SkinResourceCatalog.h"

#include <string>

namespace skin {

// TTF style fields are a bounded enhancement over Beatoraja's incremental
// renderer: finite values are quantized before they become map keys.
[[nodiscard]] bool canonicalizeSkinTextAtlasKey(SkinTextAtlasKey &) noexcept;
[[nodiscard]] std::string stableFallbackChainDigest(
    SkinResourceId primary, int primaryType,
    const std::vector<SkinFontFallbackResource> &fallbacks);

} // namespace skin
