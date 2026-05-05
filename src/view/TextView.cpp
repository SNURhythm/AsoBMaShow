#include "TextView.h"
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <cstring>
#include <cmath>
#include <mutex>
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include "bgfx/defines.h"
#include "bx/math.h"
#include <algorithm>

namespace {
std::mutex g_ttfMutex;
int g_ttfRefCount = 0;

bool acquireTtf() {
  std::lock_guard<std::mutex> lock(g_ttfMutex);
  if (g_ttfRefCount == 0 && TTF_Init() != 0) {
    SDL_Log("Failed to initialize SDL_ttf: %s", TTF_GetError());
    return false;
  }
  ++g_ttfRefCount;
  return true;
}

void releaseTtf() {
  std::lock_guard<std::mutex> lock(g_ttfMutex);
  if (g_ttfRefCount <= 0) {
    return;
  }
  --g_ttfRefCount;
  if (g_ttfRefCount == 0) {
    TTF_Quit();
  }
}
} // namespace

TextView::TextView(const std::string &fontPath, int fontSize)
    : View(), texture(BGFX_INVALID_HANDLE) {
  ttfInitialized = acquireTtf();
  if (ttfInitialized) {
    font = TTF_OpenFont(fontPath.c_str(), fontSize);
    if (!font) {
      SDL_Log("Failed to load font: %s", TTF_GetError());
      font =
          TTF_OpenFont("assets/fonts/arial.ttf", fontSize); // Fallback font
    }
  }
  color = {255, 255, 255, 255}; // Default color: white
  rect = {0, 0, 0, 0};
  s_texColor =
      rendering::UniformCache::getInstance().getSampler("s_texColor");
  YGNodeSetMeasureFunc(getNode(), measureFunc);
}

TextView::~TextView() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }

  if (font) {
    TTF_CloseFont(font);
  }
  if (ttfInitialized) {
    releaseTtf();
  }
}

void TextView::setText(const std::string &newText) {
  if (newText == text) {
    return;
  }
  this->text = newText;
  createTexture();
}

void TextView::renderImpl(RenderContext &context) {
  if (bgfx::isValid(texture)) {

    rect.x = this->getX();
    rect.y = this->getY();
    auto width = this->getWidth();
    auto height = this->getHeight();
    switch (align) {
    case TextAlign::LEFT:
      break;
    case TextAlign::CENTER:
      rect.x += (width - rect.w) / 2; // center horizontally
      break;
    case TextAlign::RIGHT:
      rect.x += width - rect.w; // align right
      break;
    }
    switch (valign) {
    case TextVAlign::TOP:
      break;
    case TextVAlign::MIDDLE:
      rect.y += (height - rect.h) / 2; // center vertically
      break;
    case TextVAlign::BOTTOM:
      rect.y += height - rect.h; // align bottom
      break;
    }

    rendering::PosTexVertex vertices[] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},                   // Top-left
        {(float)rect.w, 0.0f, 0.0f, 1.0f, 0.0f},          // Top-right
        {(float)rect.w, (float)rect.h, 0.0f, 1.0f, 1.0f}, // Bottom-right
        {0.0f, (float)rect.h, 0.0f, 0.0f, 1.0f}           // Bottom-left
    };

    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, rendering::PosTexVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tvb.data, vertices, sizeof(vertices));
    bx::memCopy(tib.data, indices, sizeof(indices));

    float translate[16];
    bx::mtxTranslate(translate, rect.x, rect.y, 0.0f);
    bgfx::setTransform(translate);
    bgfx::setTexture(0, s_texColor, texture);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);

    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
    bgfx::submit(rendering::ui_view, kProgram);
  }
}

void TextView::setColor(SDL_Color newColor) {
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  this->color = newColor;
  createTexture(); // Update the texture since newColor has changed
}

void TextView::createTexture(bool markDirty, bool force, int requestedWrapWidth) {
  const int effectiveWrapWidth =
      wrapEnabled ? std::max(0, requestedWrapWidth >= 0 ? requestedWrapWidth
                                                        : currentWrapWidth)
                  : 0;
  if (!force && effectiveWrapWidth == currentWrapWidth && bgfx::isValid(texture)) {
    return;
  }

  currentWrapWidth = effectiveWrapWidth;
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
    texture = BGFX_INVALID_HANDLE;
  }

  if (text.empty() || font == nullptr) {
    rect.w = 0;
    rect.h = 0;
    if (markDirty) {
      YGNodeMarkDirty(getNode());
    }
    return;
  }
  SDL_Surface *surface = nullptr;
  if (wrapEnabled && effectiveWrapWidth > 0) {
    surface = TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color,
                                             effectiveWrapWidth);
  } else {
    surface = TTF_RenderUTF8_Blended(font, text.c_str(), color);
  }
  if (!surface) {
    SDL_Log("Failed to render text: %s", TTF_GetError());
    return;
  }
  rect.w = surface->w;
  rect.h = surface->h;
  if (markDirty) {
    YGNodeMarkDirty(getNode());
  }
  texture = rendering::sdlSurfaceToBgfxTexture(surface);
  SDL_FreeSurface(surface);
}

YGSize TextView::measureFunc(YGNodeConstRef node, float width,
                             YGMeasureMode widthMode, float height,
                             YGMeasureMode heightMode) {
  auto *view = static_cast<TextView *>(YGNodeGetContext(node));
  (void)height;
  (void)heightMode;
  if (view->wrapEnabled && widthMode != YGMeasureModeUndefined && width > 0.0f) {
    view->createTexture(false, false, static_cast<int>(std::floor(width)));
  }
  return {static_cast<float>(view->rect.w), static_cast<float>(view->rect.h)};
}

void TextView::setAlign(TextAlign newAlign) { this->align = newAlign; }

void TextView::setVAlign(TextVAlign newVAlign) { this->valign = newVAlign; }

void TextView::setWrap(bool enabled) {
  if (wrapEnabled == enabled) {
    return;
  }
  wrapEnabled = enabled;
  if (!wrapEnabled) {
    currentWrapWidth = 0;
  }
  createTexture();
}
