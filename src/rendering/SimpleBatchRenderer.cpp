#include "SimpleBatchRenderer.h"
#include "ShaderManager.h"
#include "bx/bx.h"

namespace rendering {

void SimpleBatchRenderer::begin() {
  vertices.clear();
  indices.clear();
}

void SimpleBatchRenderer::end() { flush(); }

void SimpleBatchRenderer::addRect(float x, float y, float width, float height,
                                  uint32_t color) {
  // Check if adding this rect would exceed transient buffer limits
  // Max vertices per transient buffer is typically 65536, but let's be safe
  // with a smaller batch limit or handle overflow by flushing.
  // 4 vertices per rect.
  if (vertices.size() + 4 > 65536) {
    flush();
  }

  uint16_t baseIndex = static_cast<uint16_t>(vertices.size());

  vertices.push_back({x, y, 0.0f, color});
  vertices.push_back({x + width, y, 0.0f, color});
  vertices.push_back({x + width, y + height, 0.0f, color});
  vertices.push_back({x, y + height, 0.0f, color});

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
    // Not enough space in transient buffers for this frame/draw call.
    // Ideally we should split the batch, but for now just logging.
    // In a real scenario, we might need a dynamic buffer or multiple submit
    // calls.
    SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "SimpleBatchRenderer: Not enough transient buffer space. "
                "Vertices: %d, Indices: %d",
                numVertices, numIndices);
    // Try to alloc anyway, bgfx might handle it or return empty
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

  bgfx::submit(
      rendering::main_view,
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE));

  vertices.clear();
  indices.clear();
}

} // namespace rendering
