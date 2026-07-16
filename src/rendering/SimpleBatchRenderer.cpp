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

void SimpleBatchRenderer::begin(const float *transform) {
  vertices.clear();
  indices.clear();
  hasModelTransform = transform != nullptr;
  if (hasModelTransform) {
    std::copy_n(transform, modelTransform.size(), modelTransform.begin());
  }
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

void SimpleBatchRenderer::addLine(float x0, float y0, float x1, float y1,
                                  float thickness, uint32_t color) {
  if (thickness <= 0.0f) {
    return;
  }

  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.001f) {
    addCircle(x0, y0, thickness * 0.5f, color);
    return;
  }

  if (vertices.size() + 4 > kMaxBatchVertices ||
      indices.size() + 6 > kMaxBatchIndices) {
    flush();
  }

  const float halfThickness = thickness * 0.5f;
  const float nx = (-dy / length) * halfThickness;
  const float ny = (dx / length) * halfThickness;
  const uint16_t baseIndex = static_cast<uint16_t>(vertices.size());

  vertices.push_back({x0 + nx, y0 + ny, 0.0f, color});
  vertices.push_back({x1 + nx, y1 + ny, 0.0f, color});
  vertices.push_back({x1 - nx, y1 - ny, 0.0f, color});
  vertices.push_back({x0 - nx, y0 - ny, 0.0f, color});

  indices.push_back(baseIndex + 0);
  indices.push_back(baseIndex + 1);
  indices.push_back(baseIndex + 2);
  indices.push_back(baseIndex + 2);
  indices.push_back(baseIndex + 3);
  indices.push_back(baseIndex + 0);
}

void SimpleBatchRenderer::addTriangle(float x0, float y0, float x1, float y1,
                                      float x2, float y2, uint32_t color) {
  if (vertices.size() + 3 > kMaxBatchVertices ||
      indices.size() + 3 > kMaxBatchIndices) {
    flush();
  }

  const uint16_t baseIndex = static_cast<uint16_t>(vertices.size());
  vertices.push_back({x0, y0, 0.0F, color});
  vertices.push_back({x1, y1, 0.0F, color});
  vertices.push_back({x2, y2, 0.0F, color});
  indices.push_back(baseIndex);
  indices.push_back(baseIndex + 1);
  indices.push_back(baseIndex + 2);
}

void SimpleBatchRenderer::addCircle(float cx, float cy, float radius,
                                    uint32_t color) {
  if (radius <= 0.0f) {
    return;
  }
  addRoundedRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f,
                 radius, color);
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
  if (hasModelTransform) {
    bgfx::setTransform(modelTransform.data());
  }

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
