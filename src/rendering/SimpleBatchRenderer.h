#pragma once

#include "common.h"
#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>

namespace rendering {
class SimpleBatchRenderer {
public:
  SimpleBatchRenderer();
  ~SimpleBatchRenderer() = default;

  void begin();
  void end();
  void addRect(float x, float y, float width, float height, uint32_t color);
  void addRectVerticalGradient(float x, float y, float width, float height,
                               uint32_t topColor, uint32_t bottomColor);
  void addRectColors(float x, float y, float width, float height,
                     uint32_t topLeftColor, uint32_t topRightColor,
                     uint32_t bottomRightColor, uint32_t bottomLeftColor);
  void setSubmitDepth(uint32_t depth) { submitDepth = depth; }
  void setSubmitView(bgfx::ViewId viewId) { submitView = viewId; }
  void flush();

private:
  std::vector<PosColorVertex> vertices;
  std::vector<uint16_t> indices;
  uint32_t submitDepth = 0;
  bgfx::ViewId submitView = rendering::main_view;
  uint32_t transientBufferMissCount = 0;
};
} // namespace rendering
