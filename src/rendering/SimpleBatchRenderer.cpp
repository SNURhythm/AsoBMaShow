#include "SimpleBatchRenderer.h"
#include "ShaderManager.h"
#include "bx/bx.h"
#include <algorithm>
#include <cmath>

namespace rendering {

namespace {
constexpr size_t kMaxBatchVertices = 65532;
constexpr size_t kMaxBatchIndices = 65532;
constexpr float kPi = 3.14159265358979323846f;
} // namespace

SimpleBatchRenderer::SimpleBatchRenderer() {
  vertices.reserve(4096);
  indices.reserve(6144);
}

void SimpleBatchRenderer::begin() {
  vertices.clear();
  indices.clear();
}

void SimpleBatchRenderer::end() { flush(); }

void SimpleBatchRenderer::addRect(float x, float y, float width, float height,
                                  uint32_t color) {
  addRectColors(x, y, width, height, color, color, color, color);
}

void SimpleBatchRenderer::addRectVerticalGradient(float x, float y, float width,
                                                  float height,
                                                  uint32_t topColor,
                                                  uint32_t bottomColor) {
  addRectColors(x, y, width, height, topColor, topColor, bottomColor,
                bottomColor);
}

void SimpleBatchRenderer::addRoundedRect(float x, float y, float width,
                                         float height, float radius,
                                         uint32_t color) {
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }

  radius = std::clamp(radius, 0.0f, std::min(width, height) * 0.5f);
  if (radius <= 0.5f) {
    addRect(x, y, width, height, color);
    return;
  }

  const int segments = std::clamp(static_cast<int>(std::ceil(radius / 4.0f)),
                                  4, 12);
  const uint16_t ringVertexCount =
      static_cast<uint16_t>((segments + 1) * 4);
  const uint16_t totalVertexCount =
      static_cast<uint16_t>(ringVertexCount + 1);
  const uint16_t totalIndexCount =
      static_cast<uint16_t>(ringVertexCount * 3);
  if (vertices.size() + totalVertexCount > kMaxBatchVertices ||
      indices.size() + totalIndexCount > kMaxBatchIndices) {
    flush();
  }

  const uint16_t centerIndex = static_cast<uint16_t>(vertices.size());
  vertices.push_back({x + width * 0.5f, y + height * 0.5f, 0.0f, color});

  const auto appendCorner = [&](float cx, float cy, float startAngle) {
    for (int i = 0; i <= segments; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(segments);
      const float angle = startAngle + t * (kPi * 0.5f);
      vertices.push_back(
          {cx + std::cos(angle) * radius, cy + std::sin(angle) * radius,
           0.0f, color});
    }
  };

  appendCorner(x + width - radius, y + radius, -kPi * 0.5f);
  appendCorner(x + width - radius, y + height - radius, 0.0f);
  appendCorner(x + radius, y + height - radius, kPi * 0.5f);
  appendCorner(x + radius, y + radius, kPi);

  for (uint16_t i = 0; i < ringVertexCount; ++i) {
    const uint16_t current = static_cast<uint16_t>(centerIndex + 1 + i);
    const uint16_t next = static_cast<uint16_t>(
        centerIndex + 1 + ((i + 1) % ringVertexCount));
    indices.push_back(centerIndex);
    indices.push_back(current);
    indices.push_back(next);
  }
}

void SimpleBatchRenderer::addRectColors(float x, float y, float width,
                                        float height, uint32_t topLeftColor,
                                        uint32_t topRightColor,
                                        uint32_t bottomRightColor,
                                        uint32_t bottomLeftColor) {
  // 4 vertices and 6 indices per rect.
  if (vertices.size() + 4 > kMaxBatchVertices ||
      indices.size() + 6 > kMaxBatchIndices) {
    flush();
  }

  uint16_t baseIndex = static_cast<uint16_t>(vertices.size());

  vertices.push_back({x, y, 0.0f, topLeftColor});
  vertices.push_back({x + width, y, 0.0f, topRightColor});
  vertices.push_back({x + width, y + height, 0.0f, bottomRightColor});
  vertices.push_back({x, y + height, 0.0f, bottomLeftColor});

  indices.push_back(baseIndex + 0);
  indices.push_back(baseIndex + 1);
  indices.push_back(baseIndex + 2);
  indices.push_back(baseIndex + 2);
  indices.push_back(baseIndex + 3);
  indices.push_back(baseIndex + 0);
}

void SimpleBatchRenderer::flush() {
  if (vertices.empty()) {
    return;
  }

  bgfx::TransientVertexBuffer tvb;
  bgfx::TransientIndexBuffer tib;

  uint32_t numVertices = static_cast<uint32_t>(vertices.size());
  uint32_t numIndices = static_cast<uint32_t>(indices.size());

  if (bgfx::getAvailTransientVertexBuffer(
          numVertices, PosColorVertex::ms_decl) < numVertices ||
      bgfx::getAvailTransientIndexBuffer(numIndices) < numIndices) {
    ++transientBufferMissCount;
    if (transientBufferMissCount <= 3 ||
        (transientBufferMissCount % 300) == 0) {
      SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                  "SimpleBatchRenderer: Not enough transient buffer space. "
                  "Vertices: %d, Indices: %d (misses: %u)",
                  numVertices, numIndices, transientBufferMissCount);
    }
    vertices.clear();
    indices.clear();
    return;
  }

  bgfx::allocTransientVertexBuffer(&tvb, numVertices, PosColorVertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, numIndices);

  bx::memCopy(tvb.data, vertices.data(), numVertices * sizeof(PosColorVertex));
  bx::memCopy(tib.data, indices.data(), numIndices * sizeof(uint16_t));

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
  bgfx::setState(state);

  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::submit(submitView, kProgram, submitDepth);

  vertices.clear();
  indices.clear();
}

} // namespace rendering
