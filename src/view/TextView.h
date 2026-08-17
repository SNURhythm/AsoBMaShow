#pragma once

#include "View.h"
#include <bgfx/bgfx.h>
#include <SDL2/SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <unordered_map>
#include <vector>

class TextView : public View {
public:
  enum TextAlign { LEFT, CENTER, RIGHT };
  enum TextVAlign { TOP, MIDDLE, BOTTOM };
  enum class TextOverflow { Visible, Hidden, Marquee };
  enum class FontWeight { Regular, Bold };
  TextView(const std::string &fontPath, int fontSize,
           FontWeight fontWeight = FontWeight::Regular);
  ~TextView() override;

  void setText(const std::string &newText);
  [[nodiscard]] const std::string &getText() const { return text; }
  void setColor(SDL_Color newColor);
  void setThemedColor(ThemeColorProvider provider);
  void setAlign(TextAlign newAlign);
  void setVAlign(TextVAlign newVAlign);
  void setOverflow(TextOverflow newOverflow);
  void setWrap(bool enabled);
  [[nodiscard]] bgfx::TextureHandle textureHandle() const { return texture; }
  [[nodiscard]] int textureWidth() const { return rect.w; }
  [[nodiscard]] int textureHeight() const { return rect.h; }
  [[nodiscard]] int measureTextWidth(const std::string &utf8);
  [[nodiscard]] const std::string &primaryFontPath() const {
    return primaryFontPath_;
  }
  [[nodiscard]] int pointSize() const noexcept { return fontSize; }
  [[nodiscard]] FontWeight fontWeight() const noexcept {
    return fontWeight_;
  }
  [[nodiscard]] SDL_Color currentColor() const noexcept { return color; }

protected:
  [[nodiscard]] RenderBounds renderingBounds() const override;
  struct FontFace {
    TTF_Font *font = nullptr;
    std::string path;
  };
  struct SelectedFont {
    TTF_Font *font = nullptr;
    bool iosSystemFont = false;
  };
  struct FontRun {
    SelectedFont source;
    std::string text;
  };

  void renderImpl(RenderContext &context) override;
  void onThemeChanged() override;
  [[nodiscard]] SDL_Rect resolvedTextRect() const;
  [[nodiscard]] float marqueeOffset(int viewportWidth);
  [[nodiscard]] int textLineHeight() const;
  [[nodiscard]] int rasterTextLineHeight() const;
  [[nodiscard]] int measureRasterTextWidth(const std::string &utf8);
  SelectedFont selectFont(Uint32 codepoint);
  [[nodiscard]] bool hasFontSource(const SelectedFont &source) const;
  [[nodiscard]] bool sameFontSource(const SelectedFont &lhs,
                                    const SelectedFont &rhs) const;
  [[nodiscard]] int measureFontSourceTextWidth(const SelectedFont &source,
                                               const std::string &utf8);
  [[nodiscard]] int fontSourceAscent(const SelectedFont &source);
  [[nodiscard]] SDL_Surface *
  renderFontSourceTextSurface(const SelectedFont &source,
                              const std::string &utf8);
  [[nodiscard]] bool primaryFontSupportsText(const std::string &utf8) const;
  void ensureFontsForText(const std::string &utf8);
  void includeFontMetrics(TTF_Font *loadedFont);
  void includeIOSSystemFontMetrics();
  [[nodiscard]] TTF_Font *loadFallbackFontAt(size_t pathIndex, bool required);
  [[nodiscard]] std::vector<std::string> wrappedTextLines(int wrapWidth);
  [[nodiscard]] SDL_Surface *renderFallbackTextSurface(int wrapWidth,
                                                       int &surfaceWidth,
                                                       int &surfaceHeight);
  TextAlign align = TextAlign::LEFT;
  TextVAlign valign = TextVAlign::TOP;
  TextOverflow overflow = TextOverflow::Visible;
  TTF_Font *font = nullptr;
  std::vector<std::string> fallbackFontPaths;
  std::string primaryFontPath_;
  std::vector<FontFace> fontFaces;
  size_t nextFallbackFontPath = 0;
  std::unordered_map<Uint32, SelectedFont> fontSelectionCache;
  int fontSize = 0;
  FontWeight fontWeight_ = FontWeight::Regular;
  int fontStyle_ = TTF_STYLE_NORMAL;
  int fontRasterSize = 0;
  int fontLineHeight = 0;
  int fontAscent = 0;
  int fontDescent = 0;
  int iosSystemFontLineHeight = 0;
  int iosSystemFontAscent = 0;
  int iosSystemFontDescent = 0;
  bool iosSystemFontMetricsIncluded = false;
  bool ttfInitialized = false;

  SDL_Color color{};
  ThemeColorProvider themedColorProvider;
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
