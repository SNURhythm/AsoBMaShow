#pragma once
class Camera;
#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include "RenderPlan.h"
#include <string>
#include <algorithm>
#include <cmath>
#define SHADER_SIMPLE "vs_simple.bin", "fs_simple.bin"
#define SHADER_TEXT "vs_text.bin", "fs_text.bin"
#define SHADER_UI_SHADOW "vs_text.bin", "fs_shadow.bin"
#define SHADER_YUVRGB "vs_yuvrgb.bin", "fs_yuvrgb.bin"
#define SHADER_BGALAYER "vs_text.bin", "fs_bgalayer.bin"
namespace rendering {
// Coordinate cheat-sheet:
// - UI logical units: used by Yoga/layout, View positions/sizes, TextView, etc.
// - Drawable pixels: actual backbuffer size used by bgfx.
// - Normalized screen coords: SDL touch (0..1) in drawable space.
// Conversions:
// - screenToUi*: drawable pixels -> UI logical units.
// - normalizedToUi*: normalized screen -> UI logical/normalized UI.
// - setScissorUI: UI logical -> drawable pixels.
struct PosTexCoord0Vertex {
  static bgfx::VertexLayout ms_decl;
  float x, y, z;
  float u, v;
  static void init() {
    ms_decl.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
  }
};
struct PosColorVertex {
  static bgfx::VertexLayout ms_decl;
  float x;
  float y;
  float z;
  uint32_t abgr;

  static void init() {
    ms_decl.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
  }
};

struct PosTexVertex {
  float x, y, z;
  float u, v;

  static void init() {
    ms_decl.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
  }

  static bgfx::VertexLayout ms_decl;
};
extern Camera *main_camera;
extern Camera game_camera;
constexpr int design_width = 1920;
constexpr int design_height = 1080;
// UI logical size (design units). Used by Yoga/layout and UI positions/sizes.
extern int window_width;
extern int window_height;
// Drawable size (actual backbuffer in pixels). Use for bgfx resolution.
extern int render_width;
extern int render_height;
// SDL renderer scale: logical window -> drawable pixels (HiDPI factor).
extern float widthScale;
extern float heightScale;
// UI scale from logical units -> drawable pixels (no letterbox here).
extern float ui_scale_x;
extern float ui_scale_y;
// UI viewport offset in drawable pixels (0 when no letterbox).
extern int ui_offset_x;
extern int ui_offset_y;
// UI viewport size in drawable pixels (usually render size).
extern int ui_view_width;
extern int ui_view_height;
extern float near_clip;
extern float far_clip;

void updateUIScale(int renderW, int renderH);

// Convert drawable pixel coords -> UI logical units.
inline void screenToUi(float screenX, float screenY, float &outX, float &outY) {
  outX = (screenX - static_cast<float>(ui_offset_x)) / ui_scale_x;
  outY = (screenY - static_cast<float>(ui_offset_y)) / ui_scale_y;
}

inline void screenToUi(int screenX, int screenY, int &outX, int &outY) {
  float fx = 0.0f;
  float fy = 0.0f;
  screenToUi(static_cast<float>(screenX), static_cast<float>(screenY), fx, fy);
  outX = static_cast<int>(fx);
  outY = static_cast<int>(fy);
}

// Convert drawable pixel coords -> UI normalized (0..1 in UI logical space).
inline void screenToUiNormalized(float screenX, float screenY, float &outX,
                                 float &outY) {
  float uiX = 0.0f;
  float uiY = 0.0f;
  screenToUi(screenX, screenY, uiX, uiY);
  outX = uiX / static_cast<float>(window_width);
  outY = uiY / static_cast<float>(window_height);
}

// Convert normalized screen coords -> UI logical units.
inline void normalizedToUi(float normX, float normY, float &outX, float &outY) {
  screenToUi(normX * static_cast<float>(render_width),
             normY * static_cast<float>(render_height), outX, outY);
}

// Convert normalized screen coords -> UI normalized (0..1 in UI logical space).
inline void normalizedToUiNormalized(float normX, float normY, float &outX,
                                     float &outY) {
  screenToUiNormalized(normX * static_cast<float>(render_width),
                       normY * static_cast<float>(render_height), outX, outY);
}

struct DrawableScissor {
  bool enabled = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

// Convert edge coordinates instead of truncating x/y/width/height
// independently. Skin destinations can land on fractional UI-logical edges.
inline DrawableScissor toDrawableScissor(double x, double y, double width,
                                         double height) {
  if (width < 0 || height < 0) {
    return {};
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
      !std::isfinite(height) || !std::isfinite(ui_scale_x) ||
      !std::isfinite(ui_scale_y) || render_width <= 0 || render_height <= 0) {
    return {.enabled = true};
  }
  const double sx0 = static_cast<double>(ui_offset_x) +
                     std::floor(x * static_cast<double>(ui_scale_x));
  const double sy0 = static_cast<double>(ui_offset_y) +
                     std::floor(y * static_cast<double>(ui_scale_y));
  const double sx1 = static_cast<double>(ui_offset_x) +
                     std::ceil((x + width) * static_cast<double>(ui_scale_x));
  const double sy1 = static_cast<double>(ui_offset_y) +
                     std::ceil((y + height) * static_cast<double>(ui_scale_y));
  if (!std::isfinite(sx0) || !std::isfinite(sy0) || !std::isfinite(sx1) ||
      !std::isfinite(sy1)) {
    return {.enabled = true};
  }
  const int sx =
      static_cast<int>(std::clamp(sx0, 0.0, static_cast<double>(render_width)));
  const int sy = static_cast<int>(
      std::clamp(sy0, 0.0, static_cast<double>(render_height)));
  const int right =
      static_cast<int>(std::clamp(sx1, 0.0, static_cast<double>(render_width)));
  const int bottom = static_cast<int>(
      std::clamp(sy1, 0.0, static_cast<double>(render_height)));
  const int sw = right - sx;
  const int sh = bottom - sy;
  if (sw <= 0 || sh <= 0) {
    return {.enabled = true};
  }
  return {.enabled = true, .x = sx, .y = sy, .width = sw, .height = sh};
}

// Set scissor using UI logical units; converts to drawable pixels internally.
inline void setScissorUI(double x, double y, double width, double height) {
  const auto scissor = toDrawableScissor(x, y, width, height);
  if (!scissor.enabled) {
    bgfx::setScissor();
    return;
  }
  bgfx::setScissor(static_cast<uint16_t>(scissor.x),
                   static_cast<uint16_t>(scissor.y),
                   static_cast<uint16_t>(scissor.width),
                   static_cast<uint16_t>(scissor.height));
}

bgfx::TextureHandle sdlSurfaceToBgfxTexture(SDL_Surface *surface);

void createRect(bgfx::TransientVertexBuffer &tvb,
                bgfx::TransientIndexBuffer &tib, int x, int y, int width,
                int height, uint32_t color);

void renderTextureRegion(bgfx::TextureHandle texture, bgfx::ViewId viewId,
                         float x, float y, float width, float height);
void renderTextureRegionTint(bgfx::TextureHandle texture, bgfx::ViewId viewId,
                             float x, float y, float width, float height,
                             float brightness);
void renderTextureRegionScissor(bgfx::TextureHandle texture,
                                bgfx::ViewId viewId, int x, int y, int width,
                                int height);
void renderFullscreenTexture(bgfx::TextureHandle texture, bgfx::ViewId viewId);
void renderFullscreenTextureTint(bgfx::TextureHandle texture,
                                 bgfx::ViewId viewId, float brightness);

static PosTexCoord0Vertex s_quadVertices[] = {
    {-1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 1.0f, 0.0f, 1.0f, 0.0f},
    {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
    {1.0f, -1.0f, 0.0f, 1.0f, 1.0f},
};
static const uint16_t s_quadIndices[] = {0, 1, 2, 1, 3, 2};
inline void screenSpaceQuad() {
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  //  SDL_Log("Rendering video texture frame %d; time: %f", currentFrame,
  //  currentFrame / 30.0f);

  bgfx::VertexLayout &layout = rendering::PosTexCoord0Vertex::ms_decl;
  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  // Define quad vertices
  vertex[0].x = -1.0f;
  vertex[0].y = -1.0f;
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;

  vertex[1].x = 1.0f;
  vertex[1].y = -1.0f;
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;

  vertex[2].x = -1.0f;
  vertex[2].y = 1.0f;
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;

  vertex[3].x = 1.0f;
  vertex[3].y = 1.0f;
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
}
} // namespace rendering
