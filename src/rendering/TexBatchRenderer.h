#pragma once

#include "common.h"
#include <bgfx/bgfx.h>
#include <vector>

namespace rendering {
class TexBatchRenderer {
public:
  TexBatchRenderer();
  ~TexBatchRenderer() = default;

  void begin();
  void end();

  // Adds a quad with texture coordinates
  // (x, y) is bottom-left, (x+width, y+height) is top-right
  // tileU, tileV control tiling (texture repeating)
  // texture is the bugfx texture handle
  void addRect(float x, float y, float width, float height, float tileU,
               float tileV, bgfx::TextureHandle texture);

  void flush();

private:
  std::vector<PosTexCoord0Vertex> vertices;
  std::vector<uint16_t> indices;
  bgfx::TextureHandle currentTexture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
};
} // namespace rendering
