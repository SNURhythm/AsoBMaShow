#pragma once

namespace text_runtime {
// Shared by built-in text and custom skin missing-glyph fallback.
inline constexpr const char *bundledFontFallbackPaths[]{
    "assets/fonts/notosanscjkjp.ttf",
    "assets/fonts/notosanssymbols2.ttf",
    "assets/fonts/arial.ttf"};
} // namespace text_runtime
