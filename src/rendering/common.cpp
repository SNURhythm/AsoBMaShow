#include <SDL2/SDL.h>
#include "bx/allocator.h"
#include "bx/file.h"

#include "common.h"
#include "ShaderManager.h"
#include "UniformCache.h"
#include <limits>
namespace rendering {
float near_clip = 0.1f;
float far_clip = 100.0f;
void createRect(bgfx::TransientVertexBuffer &tvb,
                bgfx::TransientIndexBuffer &tib, int x, int y, int width,
                int height, uint32_t color) {
  PosColorVertex vertices[] = {
      {float(x), float(y), 0.0f, color},
      {float(x + width), float(y), 0.0f, color},
      {float(x + width), float(y + height), 0.0f, color},
      {float(x), float(y + height), 0.0f, color},
  };

  const uint16_t indices[] = {0, 1, 2, 0, 2, 3};

  bgfx::allocTransientVertexBuffer(&tvb, 4, PosColorVertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);

  bx::memCopy(tvb.data, vertices, sizeof(vertices));
  bx::memCopy(tib.data, indices, sizeof(indices));
}

bgfx::TextureHandle sdlSurfaceToBgfxTexture(SDL_Surface *surface) {
  if (surface == nullptr || surface->pixels == nullptr || surface->w <= 0 ||
      surface->h <= 0 ||
      surface->w > std::numeric_limits<uint16_t>::max() ||
      surface->h > std::numeric_limits<uint16_t>::max()) {
    SDL_Log("Invalid SDL surface for bgfx texture");
    return BGFX_INVALID_HANDLE;
  }

  // Calculate the total size needed for the copy considering pitch
  const auto rowBytes = static_cast<size_t>(surface->w) * sizeof(Uint32);
  const auto totalSize = rowBytes * static_cast<size_t>(surface->h);
  if (totalSize > std::numeric_limits<uint32_t>::max()) {
    SDL_Log("SDL surface is too large for bgfx texture");
    return BGFX_INVALID_HANDLE;
  }
  const bgfx::Memory *mem = bgfx::alloc(static_cast<uint32_t>(totalSize));
  if (mem == nullptr) {
    SDL_Log("Failed to allocate bgfx texture memory");
    return BGFX_INVALID_HANDLE;
  }

  // Copy row by row considering the pitch
  uint8_t *dst = (uint8_t *)mem->data;
  uint8_t *src = (uint8_t *)surface->pixels;
  for (int i = 0; i < surface->h; ++i) {
    bx::memCopy(dst, src, rowBytes);
    src += surface->pitch;
    dst += rowBytes;
  }

  return bgfx::createTexture2D(static_cast<uint16_t>(surface->w),
                               static_cast<uint16_t>(surface->h), false, 1,
                               bgfx::TextureFormat::BGRA8, 0, mem);
}

void renderFullscreenTexture(bgfx::TextureHandle texture, bgfx::ViewId viewId) {
  renderTextureRegion(texture, viewId, 0.0f, 0.0f,
                      static_cast<float>(rendering::window_width),
                      static_cast<float>(rendering::window_height));
}

void renderFullscreenTextureTint(bgfx::TextureHandle texture,
                                 bgfx::ViewId viewId, float brightness) {
  renderTextureRegionTint(
      texture, viewId, 0.0f, 0.0f, static_cast<float>(rendering::window_width),
      static_cast<float>(rendering::window_height), brightness);
}

void renderTextureRegion(bgfx::TextureHandle texture, bgfx::ViewId viewId,
                         float x, float y, float width, float height) {
  if (!bgfx::isValid(texture)) {
    return;
  }
  auto s_texColor =
      rendering::UniformCache::getInstance().getSampler("s_texColor");
  if (width <= 0.0f || height <= 0.0f) {
    return;
  }
  const float invW = 1.0f / static_cast<float>(rendering::window_width);
  const float invH = 1.0f / static_cast<float>(rendering::window_height);
  const float u0 = x * invW;
  const float v0 = y * invH;
  const float u1 = (x + width) * invW;
  const float v1 = (y + height) * invH;

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::VertexLayout &layout = rendering::PosTexCoord0Vertex::ms_decl;
  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  vertex[0] = {x, y + height, 0.0f, u0, v1};
  vertex[1] = {x + width, y + height, 0.0f, u1, v1};
  vertex[2] = {x, y, 0.0f, u0, v0};
  vertex[3] = {x + width, y, 0.0f, u1, v0};

  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texColor, texture);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  bgfx::submit(viewId, kProgram);
}

void renderTextureRegionTint(bgfx::TextureHandle texture, bgfx::ViewId viewId,
                             float x, float y, float width, float height,
                             float brightness) {
  if (!bgfx::isValid(texture) || width <= 0.0f || height <= 0.0f) {
    return;
  }

  auto s_texColor =
      rendering::UniformCache::getInstance().getSampler("s_texColor");
  const float invW = 1.0f / static_cast<float>(rendering::window_width);
  const float invH = 1.0f / static_cast<float>(rendering::window_height);
  const float u0 = x * invW;
  const float v0 = y * invH;
  const float u1 = (x + width) * invW;
  const float v1 = (y + height) * invH;

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::VertexLayout &layout = rendering::PosTexCoord0Vertex::ms_decl;
  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  vertex[0] = {x, y + height, 0.0f, u0, v1};
  vertex[1] = {x + width, y + height, 0.0f, u1, v1};
  vertex[2] = {x, y, 0.0f, u0, v0};
  vertex[3] = {x + width, y, 0.0f, u1, v0};

  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texColor, texture);
  const float clampedBrightness = std::clamp(brightness, 0.0f, 1.0f);
  const float tintColor[4] = {clampedBrightness, clampedBrightness,
                              clampedBrightness, 1.0f};
  const bgfx::UniformHandle u_tintColor =
      rendering::UniformCache::getInstance().getVec4("u_tintColor");
  bgfx::setUniform(u_tintColor, tintColor);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram("vs_text.bin",
                                                         "fs_rect_tint.bin");
  bgfx::submit(viewId, kProgram);
}

void renderTextureRegionScissor(bgfx::TextureHandle texture,
                                bgfx::ViewId viewId, int x, int y, int width,
                                int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  rendering::setScissorUI(x, y, width, height);
  renderFullscreenTexture(texture, viewId);
  bgfx::setScissor();
}
} // namespace rendering
