//
// Created by XF on 8/28/2024.
//

#pragma once
#include "ImageFade.h"
#include "View.h"
#include "../path.h"
#include <bgfx/bgfx.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ImageView : public View {
private:
  void renderImpl(RenderContext &context) override;
  struct ImageCache {
    int width, height;
    std::shared_ptr<std::vector<unsigned char>> rgba;
  };
  void freeTexture();
  bool applyCachedTexture(const path_t &path);
  bool applyCachedThumbnail(const path_t &path);
  bool applyImage(const path_t &path, const ImageCache &cache,
                  bool storeCache = true);
  void applyAsyncImageIfReady();
  bool loadTexture(const path_t &path);
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
  static std::map<std::string, ImageCache> imageCache;
  std::string currentImageKey;
  path_t currentImagePath;
  int currentImageWidth = 0;
  int currentImageHeight = 0;
  bool asyncImagePending = false;
  std::optional<ImageFade> fade_;
  std::optional<Color> scrimColor_;
  ThemeColorProvider themedScrimColorProvider_;

protected:
  void onThemeChanged() override;

public:
  ImageView() = delete;
  ImageView(int x, int y, int width, int height);
  ImageView(int x, int y, int width, int height, const path_t &path);
  ~ImageView() override;
  bool setImage(const path_t &path);
  bool setImageAsync(const path_t &path, bool prioritize = false);
  void freeImage();
  ImageView *setFade(ImageFadeDirection direction, float strength);
  ImageView *clearFade();
  ImageView *setScrimColor(const Color &color);
  ImageView *setThemedScrimColor(ThemeColorProvider provider);
  ImageView *clearScrimColor();
  [[nodiscard]] const std::optional<ImageFade> &fade() const noexcept {
    return fade_;
  }
  [[nodiscard]] const std::optional<Color> &scrimColor() const noexcept {
    return scrimColor_;
  }
  [[nodiscard]] const path_t &imagePath() const { return currentImagePath; }
  [[nodiscard]] int imageWidth() const { return currentImageWidth; }
  [[nodiscard]] int imageHeight() const { return currentImageHeight; }

  static void dropCache(const path_t &path);
  static void dropAllCache();
};
