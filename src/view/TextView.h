#pragma once

#include "View.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <vector>

class TextView : public View {
public:
  enum TextAlign { LEFT, CENTER, RIGHT };
  enum TextVAlign { TOP, MIDDLE, BOTTOM };
  enum class TextOverflow { Visible, Hidden, Marquee };
  TextView(const std::string &fontPath, int fontSize);
  ~TextView() override;

  void setText(const std::string &newText);
  void setColor(SDL_Color newColor);
  void setAlign(TextAlign newAlign);
  void setVAlign(TextVAlign newVAlign);
  void setOverflow(TextOverflow newOverflow);
  void setWrap(bool enabled);

protected:
  struct FontFace {
    TTF_Font *font = nullptr;
    std::string path;
  };

  void renderImpl(RenderContext &context) override;
  [[nodiscard]] SDL_Rect resolvedTextRect() const;
  [[nodiscard]] float marqueeOffset(int viewportWidth);
  [[nodiscard]] int textLineHeight() const;
  [[nodiscard]] int measureTextWidth(const std::string &utf8) const;
  [[nodiscard]] TTF_Font *selectFont(Uint32 codepoint) const;
  [[nodiscard]] bool primaryFontSupportsText(const std::string &utf8) const;
  [[nodiscard]] std::vector<std::string> wrappedTextLines(int wrapWidth) const;
  [[nodiscard]] SDL_Surface *
  renderFallbackTextSurface(int wrapWidth, int &surfaceWidth,
                            int &surfaceHeight) const;
  TextAlign align = TextAlign::LEFT;
  TextVAlign valign = TextVAlign::TOP;
  TextOverflow overflow = TextOverflow::Visible;
  TTF_Font *font = nullptr;
  std::vector<FontFace> fontFaces;
  int fontSize = 0;
  bool ttfInitialized = false;

  SDL_Color color{};
  SDL_Rect rect{};
  std::string text;
  bool wrapEnabled = false;
  int currentWrapWidth = 0;
  Uint64 marqueeStartedAt = 0;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  static YGSize measureFunc(YGNodeConstRef node, float width,
                            YGMeasureMode widthMode, float height,
                            YGMeasureMode heightMode);

  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
  void createTexture(bool markDirty = true, bool force = true,
                     int requestedWrapWidth = -1);
};
