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
constexpr float kMarqueePixelsPerSecond = 48.0f;
constexpr Uint64 kMarqueeStartDelayMs = 700;
constexpr Uint64 kMarqueeEdgeDelayMs = 850;

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
      font = TTF_OpenFont("assets/fonts/arial.ttf", fontSize); // Fallback font
    }
  }
  color = {255, 255, 255, 255}; // Default color: white
  rect = {0, 0, 0, 0};
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
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
  marqueeStartedAt = SDL_GetTicks64();
  createTexture();
}

void TextView::renderImpl(RenderContext &context) {
  if (!bgfx::isValid(texture)) {
    return;
  }

  SDL_Rect drawRect = resolvedTextRect();
  const bool clip =
      overflow != TextOverflow::Visible && getWidth() > 0 && getHeight() > 0;
  if (overflow == TextOverflow::Marquee && !wrapEnabled &&
      rect.w > getWidth()) {
    drawRect.x =
        getX() - static_cast<int>(std::round(marqueeOffset(getWidth())));
  }

  const auto submitText = [this, &context, &drawRect]() {
    rendering::PosTexVertex vertices[] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},                           // Top-left
        {static_cast<float>(drawRect.w), 0.0f, 0.0f, 1.0f, 0.0f}, // Top-right
        {static_cast<float>(drawRect.w), static_cast<float>(drawRect.h), 0.0f,
         1.0f, 1.0f},                                            // Bottom-right
        {0.0f, static_cast<float>(drawRect.h), 0.0f, 0.0f, 1.0f} // Bottom-left
    };

    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    bgfx::TransientVertexBuffer tvb;
    bgfx::TransientIndexBuffer tib;
    bgfx::allocTransientVertexBuffer(&tvb, 4, rendering::PosTexVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    bx::memCopy(tvb.data, vertices, sizeof(vertices));
    bx::memCopy(tib.data, indices, sizeof(indices));

    float translate[16];
    bx::mtxTranslate(translate, drawRect.x, drawRect.y, 0.0f);
    bgfx::setTransform(translate);
    bgfx::setTexture(0, s_texColor, texture);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);

    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
    bgfx::submit(rendering::ui_view, kProgram);
  };

  if (clip) {
    ScissorScope scissor(context, getX(), getY(), getWidth(), getHeight());
    submitText();
  } else {
    submitText();
  }
}

SDL_Rect TextView::resolvedTextRect() const {
  const int contentHeight =
      rect.h > 0 ? rect.h : (font != nullptr ? TTF_FontHeight(font) : 0);
  SDL_Rect drawRect = {getX(), getY(), rect.w, contentHeight};
  const int width = getWidth();
  const int height = getHeight();

  switch (align) {
  case TextAlign::LEFT:
    break;
  case TextAlign::CENTER:
    drawRect.x += (width - rect.w) / 2;
    break;
  case TextAlign::RIGHT:
    drawRect.x += width - rect.w;
    break;
  }

  switch (valign) {
  case TextVAlign::TOP:
    break;
  case TextVAlign::MIDDLE:
    drawRect.y += (height - contentHeight) / 2;
    break;
  case TextVAlign::BOTTOM:
    drawRect.y += height - contentHeight;
    break;
  }

  return drawRect;
}

float TextView::marqueeOffset(int viewportWidth) {
  const int overflowWidth = rect.w - viewportWidth;
  if (overflowWidth <= 0) {
    return 0.0f;
  }

  if (marqueeStartedAt == 0) {
    marqueeStartedAt = SDL_GetTicks64();
  }

  const float scrollDurationMs =
      static_cast<float>(overflowWidth) / kMarqueePixelsPerSecond * 1000.0f;
  const Uint64 scrollMs =
      std::max<Uint64>(1, static_cast<Uint64>(std::round(scrollDurationMs)));
  const Uint64 cycleMs = kMarqueeStartDelayMs + scrollMs + kMarqueeEdgeDelayMs +
                         scrollMs + kMarqueeEdgeDelayMs;
  Uint64 phase = (SDL_GetTicks64() - marqueeStartedAt) % cycleMs;

  if (phase < kMarqueeStartDelayMs) {
    return 0.0f;
  }
  phase -= kMarqueeStartDelayMs;

  if (phase < scrollMs) {
    return static_cast<float>(overflowWidth) * static_cast<float>(phase) /
           static_cast<float>(scrollMs);
  }
  phase -= scrollMs;

  if (phase < kMarqueeEdgeDelayMs) {
    return static_cast<float>(overflowWidth);
  }
  phase -= kMarqueeEdgeDelayMs;

  if (phase < scrollMs) {
    return static_cast<float>(overflowWidth) *
           (1.0f - static_cast<float>(phase) / static_cast<float>(scrollMs));
  }

  return 0.0f;
}

void TextView::setColor(SDL_Color newColor) {
  if (newColor.r == color.r && newColor.g == color.g && newColor.b == color.b &&
      newColor.a == color.a) {
    return;
  }
  this->color = newColor;
  createTexture(); // Update the texture since newColor has changed
}

void TextView::createTexture(bool markDirty, bool force,
                             int requestedWrapWidth) {
  const int effectiveWrapWidth =
      wrapEnabled ? std::max(0, requestedWrapWidth >= 0 ? requestedWrapWidth
                                                        : currentWrapWidth)
                  : 0;
  if (!force && effectiveWrapWidth == currentWrapWidth &&
      bgfx::isValid(texture)) {
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
  if (view->wrapEnabled && widthMode != YGMeasureModeUndefined &&
      width > 0.0f) {
    view->createTexture(false, false, static_cast<int>(std::floor(width)));
  }

  float measuredWidth = static_cast<float>(view->rect.w);
  if (view->overflow != TextOverflow::Visible &&
      widthMode != YGMeasureModeUndefined && width > 0.0f) {
    measuredWidth = widthMode == YGMeasureModeExactly
                        ? width
                        : std::min(measuredWidth, width);
  }
  return {measuredWidth, static_cast<float>(view->rect.h)};
}

void TextView::setAlign(TextAlign newAlign) { this->align = newAlign; }

void TextView::setVAlign(TextVAlign newVAlign) { this->valign = newVAlign; }

void TextView::setOverflow(TextOverflow newOverflow) {
  if (overflow == newOverflow) {
    return;
  }
  overflow = newOverflow;
  marqueeStartedAt = SDL_GetTicks64();
  YGNodeMarkDirty(getNode());
}

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
