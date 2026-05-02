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
  void setSubmitDepth(uint32_t depth) { submitDepth = depth; }
  void flush();

private:
  std::vector<PosColorVertex> vertices;
  std::vector<uint16_t> indices;
  uint32_t submitDepth = 0;
  uint32_t transientBufferMissCount = 0;
};
} // namespace rendering
