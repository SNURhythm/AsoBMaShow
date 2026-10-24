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
  std::vector<unsigned char> rgba;
};

// Preparation-thread local. Construct outside an SDL_ttf OperationGuard.
// Opens native fonts lazily and returns owned,
// straight-alpha RGBA pixels; no font handle survives atlas preparation.
class SystemTextRasterizer {
public:
  explicit SystemTextRasterizer(int pointSize);
  ~SystemTextRasterizer();
  SystemTextRasterizer(const SystemTextRasterizer &) = delete;
  SystemTextRasterizer &operator=(const SystemTextRasterizer &) = delete;
  [[nodiscard]] std::optional<SystemTextRaster> colorGlyph(
      char32_t codepoint, std::size_t maximumBytes);
#if defined(__APPLE__)
  [[nodiscard]] std::optional<SystemTextRaster> text(
      std::string_view utf8, std::array<unsigned char, 4> color,
      std::size_t maximumBytes);
#endif
private:
  struct Impl;
  int pointSize_;
  bool ownsTtfRuntime_ = false;
  std::unique_ptr<Impl> impl_;
};
} // namespace text_runtime
