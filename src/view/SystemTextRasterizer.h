#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace text_runtime {
struct SystemTextRaster {
  int width = 0;
  int height = 0;
  int ascent = 0;
  int advance = 0;
  int offsetX = 0; // Raster left edge relative to the pen; advance stays unchanged.
  std::vector<unsigned char> rgba;
};

// Preparation-thread local. Construct outside an SDL_ttf OperationGuard.
// Opens native fonts lazily and returns owned,
// straight-alpha RGBA pixels; no font handle survives atlas preparation.
class SystemTextRasterizer {
public:
  enum class GlyphFallback { SystemOnly, BundledThenSystem };
  explicit SystemTextRasterizer(int pointSize, GlyphFallback fallback = GlyphFallback::SystemOnly);
  ~SystemTextRasterizer();
  SystemTextRasterizer(const SystemTextRasterizer &) = delete;
  SystemTextRasterizer &operator=(const SystemTextRasterizer &) = delete;
  // Return a supported glyph in the selected fallback order, including ordinary
  // Unicode text and intrinsic-color emoji. Missing glyphs return nullopt.
  [[nodiscard]] std::optional<SystemTextRaster> glyph(
      char32_t codepoint, std::size_t maximumBytes);
#if defined(__APPLE__) || defined(__ANDROID__)
  [[nodiscard]] std::optional<SystemTextRaster> text(
      std::string_view utf8, std::array<unsigned char, 4> color,
      std::size_t maximumBytes);
#endif
#if defined(__ANDROID__)
  // Same dimensions/baseline as text(), without allocating a bitmap.
  [[nodiscard]] std::optional<SystemTextRaster> measure(
      std::string_view utf8, std::size_t maximumBytes);
#endif
private:
  struct Impl;
  struct FontChain;
  int pointSize_;
  bool ownsTtfRuntime_ = false;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<FontChain> bundledFonts_;
};
} // namespace text_runtime
