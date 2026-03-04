#include "TexBatchRenderer.h"
#include "ShaderManager.h"
#include "UniformCache.h"
#include "bx/bx.h"

namespace rendering {

namespace {
constexpr size_t kMaxBatchVertices = 65532;
constexpr size_t kMaxBatchIndices = 65532;
} // namespace

TexBatchRenderer::TexBatchRenderer() {
  s_texColor = UniformCache::getInstance().getSampler("s_texColor");
  vertices.reserve(4096);
  indices.reserve(6144);
}

void TexBatchRenderer::begin() {
  vertices.clear();
  indices.clear();
  currentTexture = BGFX_INVALID_HANDLE;
  rectCount = 0;
  flushCount = 0;
  submitCount = 0;
}

void TexBatchRenderer::end() { flush(); }

void TexBatchRenderer::addRect(float x, float y, float width, float height,
                               float tileU, float tileV,
                               bgfx::TextureHandle texture) {
  if (texture.idx != currentTexture.idx) {
    flush();
    currentTexture = texture;
  }

  // 4 vertices and 6 indices per rect.
  if (vertices.size() + 4 > kMaxBatchVertices ||
      indices.size() + 6 > kMaxBatchIndices) {
    flush();
  }

  ++rectCount;
  uint16_t baseIndex = static_cast<uint16_t>(vertices.size());

  // Match SpriteObject's vertex order/UV mapping to keep visual parity.
  vertices.push_back({x, y + height, 0.0f, 0.0f, 0.0f});
  vertices.push_back({x + width, y + height, 0.0f, tileU, 0.0f});
  vertices.push_back({x, y, 0.0f, 0.0f, tileV});
  vertices.push_back({x + width, y, 0.0f, tileU, tileV});

  indices.push_back(baseIndex + 0);
  indices.push_back(baseIndex + 1);
  indices.push_back(baseIndex + 2);
  indices.push_back(baseIndex + 1);
  indices.push_back(baseIndex + 3);
  indices.push_back(baseIndex + 2);
}

void TexBatchRenderer::flush() {
  if (vertices.empty()) {
    return;
  }

  ++flushCount;
  if (!bgfx::isValid(currentTexture)) {
    // Should not happen if addRect logic is correct, but safe to clear
    vertices.clear();
    indices.clear();
    return;
  }

  bgfx::TransientVertexBuffer tvb;
  bgfx::TransientIndexBuffer tib;

  uint32_t numVertices = static_cast<uint32_t>(vertices.size());
  uint32_t numIndices = static_cast<uint32_t>(indices.size());

  if (bgfx::getAvailTransientVertexBuffer(
          numVertices, PosTexCoord0Vertex::ms_decl) < numVertices ||
      bgfx::getAvailTransientIndexBuffer(numIndices) < numIndices) {
    SDL_LogWarn(SDL_LOG_CATEGORY_RENDER,
                "TexBatchRenderer: Not enough transient buffer space.");
    vertices.clear();
    indices.clear();
    return;
  }

  bgfx::allocTransientVertexBuffer(&tvb, numVertices,
                                   PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, numIndices);

  bx::memCopy(tvb.data, vertices.data(),
              numVertices * sizeof(PosTexCoord0Vertex));
  bx::memCopy(tib.data, indices.data(), numIndices * sizeof(uint16_t));

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  // Bind texture
  bgfx::setTexture(0, s_texColor, currentTexture);

  uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA;
  bgfx::setState(state);

  bgfx::submit(rendering::main_view,
               rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT),
               submitDepth);
  ++submitCount;

  vertices.clear();
  indices.clear();
}

} // namespace rendering
