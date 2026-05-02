#pragma once

#include "common.h"
#include <bgfx/bgfx.h>
#include <cstdint>
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
  void addRectUV(float x, float y, float width, float height, float u0,
                 float v0, float u1, float v1,
                 bgfx::TextureHandle texture);
  void setSubmitDepth(uint32_t depth) { submitDepth = depth; }

  void flush();
  [[nodiscard]] uint32_t getRectCount() const { return rectCount; }
  [[nodiscard]] uint32_t getFlushCount() const { return flushCount; }
  [[nodiscard]] uint32_t getSubmitCount() const { return submitCount; }

private:
  std::vector<PosTexCoord0Vertex> vertices;
  std::vector<uint16_t> indices;
  bgfx::TextureHandle currentTexture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
  uint32_t submitDepth = 0;
  uint32_t rectCount = 0;
  uint32_t flushCount = 0;
  uint32_t submitCount = 0;
  uint32_t transientBufferMissCount = 0;
};
} // namespace rendering
