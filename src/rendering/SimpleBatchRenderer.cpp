#include "SimpleBatchRenderer.h"
#include "FrameSubmission.h"
#include "ShaderManager.h"
#include "UnitCircleSamples.h"
#include "bx/bx.h"
#include <algorithm>
#include <cmath>

namespace rendering {

namespace {
constexpr size_t kMaxBatchVertices = 65532;
constexpr size_t kMaxBatchIndices = 65532;
constexpr float kPi = 3.14159265358979323846f;
constexpr int kMinCornerSegments = 4;
constexpr int kMaxCornerSegments = 12;

struct CornerSample {
  float x;
  float y;
};
using RoundedRectSamples =
    std::array<std::array<std::array<CornerSample, kMaxCornerSegments + 1>, 4>,
               kMaxCornerSegments - kMinCornerSegments + 1>;

const RoundedRectSamples &roundedRectSamples() {
  static const RoundedRectSamples samples = [] {
    RoundedRectSamples result{};
    constexpr std::array<float, 4> startAngles{
        -kPi * 0.5f, 0.0f, kPi * 0.5f, kPi};
    for (int segments = kMinCornerSegments; segments <= kMaxCornerSegments;
         ++segments) {
      auto &corners = result[segments - kMinCornerSegments];
      for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        for (int i = 0; i <= segments; ++i) {
          // Keep the original float expressions, including corner endpoints.
          const float t = static_cast<float>(i) / static_cast<float>(segments);
          const float angle = startAngles[corner] + t * (kPi * 0.5f);
          corners[corner][i] = {std::cos(angle), std::sin(angle)};
        }
      }
    }
    return result;
  }();
  return samples;
}
} // namespace

SimpleBatchRenderer::SimpleBatchRenderer() {
  // Initialize once before drawing; later instances share immutable samples.
  (void)roundedRectSamples();
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
  addTriangleColors(x0, y0, x1, y1, x2, y2, color, color, color);
}

void SimpleBatchRenderer::addTriangleColors(float x0, float y0, float x1, float y1,
                                            float x2, float y2, uint32_t color0,
                                            uint32_t color1, uint32_t color2) {
  if (vertices.size() + 3 > kMaxBatchVertices ||
      indices.size() + 3 > kMaxBatchIndices) {
    flush();
  }
  const uint16_t baseIndex = static_cast<uint16_t>(vertices.size());
  vertices.push_back({x0, y0, 0.0F, color0});
  vertices.push_back({x1, y1, 0.0F, color1});
  vertices.push_back({x2, y2, 0.0F, color2});
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

void SimpleBatchRenderer::addRadialGradientCircle(
    float cx, float cy, float radius, uint32_t centerColor, uint32_t edgeColor) {
  if (radius <= 0.0F || ((centerColor | edgeColor) >> 24) == 0) {
    return;
  }
  constexpr int segments = 32;
  constexpr int vertexCount = segments + 2;
  constexpr int indexCount = segments * 3;
  if (vertices.size() + vertexCount > kMaxBatchVertices ||
      indices.size() + indexCount > kMaxBatchIndices) {
    flush();
  }
  const auto base = static_cast<uint16_t>(vertices.size());
  vertices.push_back({cx, cy, 0.0F, centerColor});
  for (const auto sample : unitCircleSamples<segments>()) {
    vertices.push_back({cx + sample.x * radius, cy + sample.y * radius,
                        0.0F, edgeColor});
  }
  for (int i = 0; i < segments; ++i) {
    indices.push_back(base);
    indices.push_back(static_cast<uint16_t>(base + i + 1));
    indices.push_back(static_cast<uint16_t>(base + i + 2));
  }
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
                                  kMinCornerSegments, kMaxCornerSegments);
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

  const auto &corners = roundedRectSamples()[segments - kMinCornerSegments];
  const auto appendCorner = [&](float cx, float cy, std::size_t corner) {
    for (int i = 0; i <= segments; ++i) {
      const auto sample = corners[corner][i];
      vertices.push_back(
          {cx + sample.x * radius, cy + sample.y * radius,
           0.0f, color});
    }
  };

  appendCorner(x + width - radius, y + radius, 0);
  appendCorner(x + width - radius, y + height - radius, 1);
  appendCorner(x + radius, y + height - radius, 2);
  appendCorner(x + radius, y + radius, 3);

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
    rejectFrameSubmission();
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

  if (tvb.data == nullptr || tib.data == nullptr ||
      tvb.size < numVertices * sizeof(PosColorVertex) ||
      tib.size < numIndices * sizeof(uint16_t)) {
    rejectFrameSubmission();
    vertices.clear();
    indices.clear();
    return;
  }

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
