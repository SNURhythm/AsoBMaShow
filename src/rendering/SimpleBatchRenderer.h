#pragma once

#include "common.h"
#include <bgfx/bgfx.h>
#include <vector>

namespace rendering {
class SimpleBatchRenderer {
public:
  SimpleBatchRenderer() = default;
  ~SimpleBatchRenderer() = default;

  void begin();
  void end();
  void addRect(float x, float y, float width, float height, uint32_t color);
  void flush();

private:
  std::vector<PosColorVertex> vertices;
  std::vector<uint16_t> indices;
};
} // namespace rendering
