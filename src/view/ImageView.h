//
// Created by XF on 8/28/2024.
//

#pragma once
#include "DecodedImageCache.h"
#include "ImageFade.h"
#include "ImageDecodeCoordinator.h"
#include "View.h"
#include "../path.h"
#include <bgfx/bgfx.h>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>

[[nodiscard]] bool
imageResourceAvailable(const std::filesystem::path &path);
[[nodiscard]] bool imageResourceAvailable(const std::filesystem::path &path,
                                          std::stop_token stop);

class ImageView : public View {
private:
  void renderImpl(RenderContext &context) override;
  using ImageCache = image_decode::DecodedImageData;
  void cancelAsyncRequest();
  bool resetDroppedAsyncRequest();
  void freeTexture();
  bool applyCachedTexture(const path_t &path, const std::string &key);
  bool applyCachedThumbnail(const path_t &path, const std::string &key);
  bool applyImage(const path_t &path, const std::string &key,
                  const ImageCache &cache,
                  bool storeCache = true);
  void applyAsyncImageIfReady();
  bool loadTexture(const path_t &path);
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
  static image_decode::DecodedImageCache imageCache;
  std::string currentImageKey;
  path_t currentImagePath;
  int currentImageWidth = 0;
  int currentImageHeight = 0;
  int asyncTargetWidth = 0;
  int asyncTargetHeight = 0;
  bool asyncImageBound = false;
  bool asyncImagePending = false;
  image_decode::ImageDecodeCoordinator::Ticket asyncTicket = 0;
  std::optional<path_t> sharedChartImagePath_;
  std::optional<ImageFade> fade_;
  std::optional<Color> scrimColor_;
  ThemeColorProvider themedScrimColorProvider_;

protected:
  void onLayout() override;
  void onThemeChanged() override;

public:
  ImageView() = delete;
  ImageView(int x, int y, int width, int height);
  ImageView(int x, int y, int width, int height, const path_t &path);
  ~ImageView() override;
  bool setImage(const path_t &path);
  bool setImageAsync(const path_t &path, bool prioritize = false);
  // Like setImageAsync but decodes at the source's full resolution and also
  // seeds the shared chart-image cache, so the gameplay skin's builtin
  // stage/back/banner loads reuse the same decoded pixels.
  bool setImageAsyncShared(const path_t &path, bool prioritize = false);
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
  static void evictDecodedImageCache();
  // Shared decoded-image cache used by both view thumbnails and the gameplay
  // skin's builtin chart images. Keyed by archive/content identity so the
  // skin reuses a stage/back/banner image already loaded for display.
  [[nodiscard]] static image_decode::DecodedImageCache &
  sharedDecodedImageCache();
  [[nodiscard]] static std::string chartImageCacheKey(const path_t &path);
  [[nodiscard]] static std::optional<image_decode::DecodedImageData>
  findChartImage(const path_t &path);
#if defined(ASOBMASHOW_IMAGE_VIEW_TESTING)
  [[nodiscard]] static std::size_t
  pendingAsyncDecodeCountForTesting(const path_t &path);
  [[nodiscard]] static std::size_t decodedImageCacheBytesForTesting();
#endif
};
