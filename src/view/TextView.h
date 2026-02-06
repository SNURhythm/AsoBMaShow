#pragma once

#include "View.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <string>

class TextView : public View {
public:
  enum TextAlign { LEFT, CENTER, RIGHT };
  enum TextVAlign { TOP, MIDDLE, BOTTOM };
  TextView(const std::string &fontPath, int fontSize);
  ~TextView() override;

  void setText(const std::string &newText);
  void setColor(SDL_Color newColor);
  void setAlign(TextAlign newAlign);
  void setVAlign(TextVAlign newVAlign);

protected:
  void renderImpl(RenderContext &context) override;
  TextAlign align = TextAlign::LEFT;
  TextVAlign valign = TextVAlign::TOP;
  TTF_Font *font;

  SDL_Color color{};
  SDL_Rect rect{};
  std::string text;
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  static YGSize measureFunc(YGNodeConstRef node, float width,
                            YGMeasureMode widthMode, float height,
                            YGMeasureMode heightMode);

  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
  void createTexture();
};
