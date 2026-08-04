//
// Created by XF on 8/28/2024.
//

#include "ImageView.h"
#include "../ArchiveFile.h"
#include "../path.h"
#include "../targets.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <unistd.h>
#endif
#include "../RAII.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include <bgfx/bgfx.h>
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../bgfx/bimg/3rdparty/stb/stb_image_resize.h"
#include "../StbImageRAII.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kArchivedThumbnailMaxDimension = 256;
#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
constexpr std::size_t kDecodedImageCacheBudget = 64U * 1024U * 1024U;
#else
constexpr std::size_t kDecodedImageCacheBudget = 128U * 1024U * 1024U;
#endif
constexpr std::array<unsigned char, 8> kArchivedThumbnailMagic = {
    'A', 'S', 'O', 'B', 'T', 'H', 'M', '1'};

using DecodedImage = image_decode::DecodedImageData;

struct ImageDecodeTimings {
  std::int64_t sourceAccessMillis = 0;
  std::int64_t sourceLoadDecodeMillis = 0;
  std::int64_t rgbaCopyMillis = 0;
  std::int64_t archivePreviewMillis = 0;
};

std::int64_t elapsedMillis(std::chrono::steady_clock::time_point started,
                           std::chrono::steady_clock::time_point finished) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(finished -
                                                                started)
      .count();
}

std::string imageCacheSourceKey(const path_t &path) {
  return archive_file::cacheKeyForPath(std::filesystem::path(path));
}

std::string imageAsyncCacheSourceKey(const path_t &path) {
  return "async-path:" +
         fspath_to_utf8(std::filesystem::path(path).lexically_normal());
}

std::string sizedImageCacheKey(std::string sourceKey, int targetWidth,
                               int targetHeight) {
  targetWidth = std::clamp(targetWidth, 1,
                           static_cast<int>(
                               std::numeric_limits<std::uint16_t>::max()));
  targetHeight = std::clamp(targetHeight, 1,
                            static_cast<int>(
                                std::numeric_limits<std::uint16_t>::max()));
  return std::move(sourceKey) + ":fit=" + std::to_string(targetWidth) + "x" +
         std::to_string(targetHeight);
}

std::string imageCacheKey(const path_t &path, int targetWidth,
                          int targetHeight) {
  return sizedImageCacheKey(imageCacheSourceKey(path), targetWidth,
                            targetHeight);
}

std::string imageAsyncCacheKey(const path_t &path, int targetWidth,
                               int targetHeight) {
  return sizedImageCacheKey(imageAsyncCacheSourceKey(path), targetWidth,
                            targetHeight);
}

bool decodedImageDimensionsAreValid(int width, int height) {
  return width > 0 && height > 0 &&
         width <= std::numeric_limits<std::uint16_t>::max() &&
         height <= std::numeric_limits<std::uint16_t>::max() &&
         static_cast<std::uint64_t>(width) *
                 static_cast<std::uint64_t>(height) <=
             std::numeric_limits<std::uint32_t>::max() / 4;
}

bool isArchiveEntryImagePath(const std::filesystem::path &path) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  return archive_file::splitVirtualPath(path, archivePath, innerPath);
}

std::filesystem::path
archivedThumbnailCacheKeyPath(const std::filesystem::path &path) {
  std::filesystem::path keyPath = path;
  keyPath += ".asobmashow-thumb256-v1.rgba";
  return keyPath;
}

std::uint32_t readLittleEndianU32(const unsigned char *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void appendLittleEndianU32(std::vector<unsigned char> &bytes,
                           std::uint32_t value) {
  bytes.push_back(static_cast<unsigned char>(value & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

std::optional<DecodedImage>
readCachedArchivedThumbnail(const std::filesystem::path &path) {
  if (!isArchiveEntryImagePath(path)) {
    return std::nullopt;
  }

  const std::filesystem::path cachePath =
      archive_file::materializedFileCachePath(
          archivedThumbnailCacheKeyPath(path));
  std::ifstream file(cachePath, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::array<unsigned char, 16> header{};
  file.read(reinterpret_cast<char *>(header.data()),
            static_cast<std::streamsize>(header.size()));
  if (!file || !std::equal(kArchivedThumbnailMagic.begin(),
                           kArchivedThumbnailMagic.end(), header.begin())) {
    return std::nullopt;
  }

  const int width =
      static_cast<int>(readLittleEndianU32(header.data() + 8));
  const int height =
      static_cast<int>(readLittleEndianU32(header.data() + 12));
  if (!decodedImageDimensionsAreValid(width, height) ||
      width > kArchivedThumbnailMaxDimension ||
      height > kArchivedThumbnailMaxDimension) {
    return std::nullopt;
  }

  const size_t byteCount =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4U;
  auto rgba = std::make_shared<std::vector<unsigned char>>(byteCount);
  if (!rgba->empty()) {
    file.read(reinterpret_cast<char *>(rgba->data()),
              static_cast<std::streamsize>(rgba->size()));
  }
  if (!file) {
    return std::nullopt;
  }
  return DecodedImage{.width = width, .height = height, .rgba = rgba};
}

DecodedImage makeArchivedThumbnail(const DecodedImage &decoded) {
  const int maxDimension = std::max(decoded.width, decoded.height);
  if (maxDimension <= kArchivedThumbnailMaxDimension) {
    return decoded;
  }

  const float scale = static_cast<float>(kArchivedThumbnailMaxDimension) /
                      static_cast<float>(maxDimension);
  const int thumbnailWidth =
      std::max(1, static_cast<int>(std::round(decoded.width * scale)));
  const int thumbnailHeight =
      std::max(1, static_cast<int>(std::round(decoded.height * scale)));
  auto rgba = std::make_shared<std::vector<unsigned char>>(
      static_cast<size_t>(thumbnailWidth) *
      static_cast<size_t>(thumbnailHeight) * 4U);
  const int resized =
      stbir_resize_uint8(decoded.rgba->data(), decoded.width, decoded.height, 0,
                         rgba->data(), thumbnailWidth, thumbnailHeight, 0, 4);
  if (resized == 0) {
    return decoded;
  }
  return DecodedImage{.width = thumbnailWidth,
                      .height = thumbnailHeight,
                      .rgba = rgba};
}

DecodedImage downsampleDecodedImage(const DecodedImage &decoded,
                                    int targetWidth, int targetHeight) {
  if (!decoded.valid() || targetWidth <= 0 || targetHeight <= 0 ||
      (decoded.width <= targetWidth && decoded.height <= targetHeight)) {
    return decoded;
  }
  const double scale =
      std::min(static_cast<double>(targetWidth) / decoded.width,
               static_cast<double>(targetHeight) / decoded.height);
  const int resizedWidth =
      std::max(1, static_cast<int>(std::floor(decoded.width * scale)));
  const int resizedHeight =
      std::max(1, static_cast<int>(std::floor(decoded.height * scale)));
  auto rgba = std::make_shared<std::vector<unsigned char>>(
      static_cast<std::size_t>(resizedWidth) * resizedHeight * 4U);
  if (stbir_resize_uint8(decoded.rgba->data(), decoded.width, decoded.height, 0,
                        rgba->data(), resizedWidth, resizedHeight, 0, 4) == 0) {
    return decoded;
  }
  return {.width = resizedWidth,
          .height = resizedHeight,
          .rgba = std::move(rgba)};
}

void writeCachedArchivedThumbnail(const std::filesystem::path &path,
                                  const DecodedImage &thumbnail) {
  if (!thumbnail.rgba ||
      !decodedImageDimensionsAreValid(thumbnail.width, thumbnail.height) ||
      thumbnail.width > kArchivedThumbnailMaxDimension ||
      thumbnail.height > kArchivedThumbnailMaxDimension) {
    return;
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(kArchivedThumbnailMagic.size() + 8U +
                thumbnail.rgba->size());
  bytes.insert(bytes.end(), kArchivedThumbnailMagic.begin(),
               kArchivedThumbnailMagic.end());
  appendLittleEndianU32(bytes, static_cast<std::uint32_t>(thumbnail.width));
  appendLittleEndianU32(bytes, static_cast<std::uint32_t>(thumbnail.height));
  bytes.insert(bytes.end(), thumbnail.rgba->begin(), thumbnail.rgba->end());

  std::string errorMessage;
  if (!archive_file::materializeFileBytes(archivedThumbnailCacheKeyPath(path),
                                          bytes, &errorMessage) &&
      !errorMessage.empty()) {
    SDL_Log("Failed to cache archived image thumbnail %s: %s",
            fspath_to_utf8(path).c_str(), errorMessage.c_str());
  }
}

std::optional<DecodedImage>
decodeImageFile(const std::filesystem::path &path,
                ImageDecodeTimings *timings = nullptr, int targetWidth = 0,
                int targetHeight = 0) {
  ImageDecodeTimings measured;
  const bool archiveEntryPath = isArchiveEntryImagePath(path);
  int width = 0;
  int height = 0;
  int channels = 0;
  StbiImageHandle data(nullptr);
#if TARGET_OS_ANDROID
  bool androidTreePath = false;
  if (IsAndroidTreePath(path)) {
    androidTreePath = true;
    std::string fdError;
    const auto sourceAccessStarted = std::chrono::steady_clock::now();
    const auto fd = OpenAndroidTreeFileDescriptor(path, fdError);
    measured.sourceAccessMillis = elapsedMillis(
        sourceAccessStarted, std::chrono::steady_clock::now());
    if (!fd.has_value()) {
      SDL_Log("Failed to open Android image descriptor %s: %s",
              fspath_to_utf8(path).c_str(), fdError.c_str());
      return std::nullopt;
    }
    FILE *file = fdopen(*fd, "rb");
    if (file == nullptr) {
      close(*fd);
      SDL_Log("Failed to create FILE for Android image descriptor: %s",
              fspath_to_utf8(path).c_str());
      return std::nullopt;
    }
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    data.reset(stbi_load_from_file(file, &width, &height, &channels, 4));
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
    fclose(file);
  }
  if (!androidTreePath && !archive_file::isVirtualPath(path)) {
#else
  if (!archive_file::isVirtualPath(path)) {
#endif
    const std::string utf8Path = fspath_to_utf8(path);
#ifdef _WIN32
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    data.reset(stbi_load(utf8Path.c_str(), &width, &height, &channels, 4));
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
#else
    const auto sourceAccessStarted = std::chrono::steady_clock::now();
    UniqueResource<FILE, fclose> file(fopen(utf8Path.c_str(), "rb"));
    measured.sourceAccessMillis = elapsedMillis(
        sourceAccessStarted, std::chrono::steady_clock::now());
    if (file) {
      const auto sourceLoadStarted = std::chrono::steady_clock::now();
      data.reset(
          stbi_load_from_file(file.get(), &width, &height, &channels, 4));
      measured.sourceLoadDecodeMillis = elapsedMillis(
          sourceLoadStarted, std::chrono::steady_clock::now());
    }
#endif
  } else {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    const auto sourceAccessStarted = std::chrono::steady_clock::now();
    if (!archive_file::readFile(path, bytes, &errorMessage)) {
      measured.sourceAccessMillis = elapsedMillis(
          sourceAccessStarted, std::chrono::steady_clock::now());
      if (timings != nullptr) {
        *timings = measured;
      }
      SDL_Log("Failed to read archived image %s: %s",
              fspath_to_utf8(path).c_str(), errorMessage.c_str());
      return std::nullopt;
    }
    measured.sourceAccessMillis = elapsedMillis(
        sourceAccessStarted, std::chrono::steady_clock::now());
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    data.reset(stbi_load_from_memory(bytes.data(),
                                     static_cast<int>(bytes.size()), &width,
                                     &height, &channels, 4));
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
  }

  if (data == nullptr || !decodedImageDimensionsAreValid(width, height)) {
    if (timings != nullptr) {
      *timings = measured;
    }
    return std::nullopt;
  }

  const auto copyStarted = std::chrono::steady_clock::now();
  const size_t byteCount = static_cast<size_t>(width) *
                           static_cast<size_t>(height) * 4;
  auto rgba = std::make_shared<std::vector<unsigned char>>(byteCount);
  std::copy(data.get(), data.get() + byteCount, rgba->begin());
  measured.rgbaCopyMillis =
      elapsedMillis(copyStarted, std::chrono::steady_clock::now());
  DecodedImage decoded{.width = width, .height = height, .rgba = rgba};
  if (archiveEntryPath) {
    const auto archivePreviewStarted = std::chrono::steady_clock::now();
    DecodedImage thumbnail = makeArchivedThumbnail(decoded);
    writeCachedArchivedThumbnail(path, thumbnail);
    measured.archivePreviewMillis = elapsedMillis(
        archivePreviewStarted, std::chrono::steady_clock::now());
  }
  if (timings != nullptr) {
    *timings = measured;
  }
  return downsampleDecodedImage(decoded, targetWidth, targetHeight);
}


image_decode::ImageDecodeCoordinator &imageDecodeCoordinator() {
  static image_decode::ImageDecodeCoordinator coordinator(
      [](const image_decode::ImageDecodeRequest &request)
          -> std::optional<DecodedImage> {
        const auto started = std::chrono::steady_clock::now();
        ImageDecodeTimings timings;
        auto decoded = decodeImageFile(request.path, &timings,
                                       request.targetWidth,
                                       request.targetHeight);
        const auto workerMillis = elapsedMillis(
            started, std::chrono::steady_clock::now());
        if (workerMillis >= 250) {
          const std::string diagnostic =
              "Slow async image load: source=" +
              std::to_string(timings.sourceAccessMillis) +
              "ms load_decode=" +
              std::to_string(timings.sourceLoadDecodeMillis) +
              "ms copy=" + std::to_string(timings.rgbaCopyMillis) +
              "ms archive_preview=" +
              std::to_string(timings.archivePreviewMillis) +
              "ms worker=" + std::to_string(workerMillis) + "ms path=" +
              fspath_to_utf8(request.path);
          SDL_Log("%s", diagnostic.c_str());
          archive_file::appendDebugLogLine(diagnostic);
        }
        return decoded;
      });
  return coordinator;
}

void submitTexturedRoundedRect(const RenderContext &context,
                               bgfx::TextureHandle texture,
                               bgfx::UniformHandle sampler, int x, int y,
                               int width, int height, float radius,
                               bgfx::ProgramHandle program) {
  if (!bgfx::isValid(texture) || width <= 0 || height <= 0) {
    return;
  }

  radius = std::clamp(radius, 0.0f,
                      static_cast<float>(std::min(width, height)) * 0.5f);

  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};
  if (radius <= 0.5f) {
    constexpr uint32_t kVertexCount = 4;
    constexpr uint32_t kIndexCount = 6;
    if (bgfx::getAvailTransientVertexBuffer(
            kVertexCount, rendering::PosTexCoord0Vertex::ms_decl) <
            kVertexCount ||
        bgfx::getAvailTransientIndexBuffer(kIndexCount) < kIndexCount) {
      return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, kVertexCount,
                                     rendering::PosTexCoord0Vertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, kIndexCount);
    auto *vertices = reinterpret_cast<rendering::PosTexCoord0Vertex *>(tvb.data);
    vertices[0] = {static_cast<float>(x), static_cast<float>(y + height), 0.0f,
                   0.0f, 1.0f};
    vertices[1] = {static_cast<float>(x + width),
                   static_cast<float>(y + height), 0.0f, 1.0f, 1.0f};
    vertices[2] = {static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f,
                   0.0f};
    vertices[3] = {static_cast<float>(x + width), static_cast<float>(y), 0.0f,
                   1.0f, 0.0f};

    auto *indices = reinterpret_cast<uint16_t *>(tib.data);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    indices[3] = 1;
    indices[4] = 3;
    indices[5] = 2;
  } else {
    const int segments =
        std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
    const uint16_t ringVertexCount = static_cast<uint16_t>((segments + 1) * 4);
    const uint16_t vertexCount = static_cast<uint16_t>(ringVertexCount + 1);
    const uint16_t indexCount = static_cast<uint16_t>(ringVertexCount * 3);

    if (bgfx::getAvailTransientVertexBuffer(
            vertexCount, rendering::PosTexCoord0Vertex::ms_decl) <
            vertexCount ||
        bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount) {
      return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, vertexCount,
                                     rendering::PosTexCoord0Vertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, indexCount);

    auto *vertices = reinterpret_cast<rendering::PosTexCoord0Vertex *>(tvb.data);
    auto *indices = reinterpret_cast<uint16_t *>(tib.data);
    uint16_t vertexIndex = 0;

    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const auto appendVertex = [&](float px, float py) {
      vertices[vertexIndex++] = {px, py, 0.0f, (px - fx) / fw,
                                 (py - fy) / fh};
    };

    appendVertex(fx + fw * 0.5f, fy + fh * 0.5f);
    const auto appendCorner = [&](float cx, float cy, float startAngle) {
      for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = startAngle + t * (kPi * 0.5f);
        appendVertex(cx + std::cos(angle) * radius,
                     cy + std::sin(angle) * radius);
      }
    };

    appendCorner(fx + fw - radius, fy + radius, -kPi * 0.5f);
    appendCorner(fx + fw - radius, fy + fh - radius, 0.0f);
    appendCorner(fx + radius, fy + fh - radius, kPi * 0.5f);
    appendCorner(fx + radius, fy + radius, kPi);

    uint16_t index = 0;
    for (uint16_t i = 0; i < ringVertexCount; ++i) {
      indices[index++] = 0;
      indices[index++] = static_cast<uint16_t>(i + 1);
      indices[index++] = static_cast<uint16_t>((i + 1) % ringVertexCount + 1);
    }
  }

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, sampler, texture);
  context.applyTransform();
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA |
                 BGFX_STATE_MSAA);
  bgfx::submit(rendering::ui_view, program);
}
} // namespace

image_decode::DecodedImageCache ImageView::imageCache(
    kDecodedImageCacheBudget);
ImageView::ImageView(int x, int y, int width, int height, const path_t &path)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
  loadTexture(path);
}
ImageView::~ImageView() { freeImage(); }

bool ImageView::applyImage(const path_t &path, const std::string &key,
                           const ImageCache &cache, bool storeCache) {
  freeTexture();
  if (!cache.rgba || cache.rgba->empty() ||
      !decodedImageDimensionsAreValid(cache.width, cache.height)) {
    return false;
  }
  const size_t expectedByteCount = static_cast<size_t>(cache.width) *
                                   static_cast<size_t>(cache.height) * 4;
  if (cache.rgba->size() != expectedByteCount ||
      cache.rgba->size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const bgfx::TextureHandle nextTexture = bgfx::createTexture2D(
      static_cast<std::uint16_t>(cache.width),
      static_cast<std::uint16_t>(cache.height), false, 1,
      bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(cache.rgba->data(),
                 static_cast<std::uint32_t>(cache.rgba->size())));
  if (!bgfx::isValid(nextTexture)) {
    return false;
  }
  texture = nextTexture;
  currentImageKey = key;
  currentImagePath = path;
  currentImageWidth = cache.width;
  currentImageHeight = cache.height;
  if (storeCache) {
    (void)imageCache.put(key, cache);
  }
  return true;
}

bool ImageView::applyCachedTexture(const path_t &path,
                                   const std::string &key) {
  if (const auto cached = imageCache.get(key)) {
    cancelAsyncRequest();
    return applyImage(path, key, *cached, false);
  }
  if (asyncTicket != 0) {
    if (const auto loaded = imageDecodeCoordinator().takeReady(asyncTicket)) {
      asyncTicket = 0;
      return applyImage(path, key, *loaded);
    }
  }
  return false;
}

bool ImageView::applyCachedThumbnail(const path_t &path,
                                     const std::string &key) {
  const auto thumbnail =
      readCachedArchivedThumbnail(std::filesystem::path(path));
  if (!thumbnail.has_value()) {
    return false;
  }
  return applyImage(path, key, {.width = thumbnail->width,
                                .height = thumbnail->height,
                                .rgba = thumbnail->rgba},
                    false);
}

void ImageView::applyAsyncImageIfReady() {
  if (currentImageKey.empty() || !asyncImagePending) {
    return;
  }
  if (resetDroppedAsyncRequest()) {
    const path_t path = currentImagePath;
    setImageAsync(path, true);
  }
  if (applyCachedTexture(currentImagePath, currentImageKey)) {
    asyncImagePending = false;
    return;
  }
  if (asyncTicket != 0 &&
      imageDecodeCoordinator().hasFailed(asyncTicket)) {
    cancelAsyncRequest();
    asyncImagePending = false;
  }
}

bool ImageView::loadTexture(const path_t &path) {
  asyncImageBound = false;
  asyncTargetWidth = 0;
  asyncTargetHeight = 0;
  const std::string utf8Path = path_t_to_utf8(path);
  const int targetWidth = std::max(1, getWidth());
  const int targetHeight = std::max(1, getHeight());
  const std::string key = imageCacheKey(path, targetWidth, targetHeight);
  if (currentImageKey == key && bgfx::isValid(texture) &&
      !asyncImagePending) {
    return true;
  }
  cancelAsyncRequest();
  asyncImagePending = false;
  if (applyCachedTexture(path, key)) {
    return true;
  }

  const auto decoded = decodeImageFile(std::filesystem::path(path), nullptr,
                                       targetWidth, targetHeight);
  if (!decoded.has_value()) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s; width: %d; height: %d", utf8Path.c_str(),
          decoded->width, decoded->height);
  return applyImage(path, key, {.width = decoded->width,
                                .height = decoded->height,
                                .rgba = decoded->rgba});
}

void ImageView::freeTexture() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }
  texture = BGFX_INVALID_HANDLE;
  currentImageWidth = 0;
  currentImageHeight = 0;
}

void ImageView::cancelAsyncRequest() {
  if (asyncTicket != 0) {
    imageDecodeCoordinator().cancel(asyncTicket);
    asyncTicket = 0;
  }
}

bool ImageView::resetDroppedAsyncRequest() {
  if (asyncTicket == 0 || imageDecodeCoordinator().isTracked(asyncTicket)) {
    return false;
  }
  asyncTicket = 0;
  return true;
}

bool ImageView::setImage(const path_t &path) { return loadTexture(path); }
bool ImageView::setImageAsync(const path_t &path, bool prioritize) {
  resetDroppedAsyncRequest();
  if (asyncTicket != 0 &&
      imageDecodeCoordinator().hasFailed(asyncTicket)) {
    cancelAsyncRequest();
  }
  const int targetWidth = std::max(1, getWidth());
  const int targetHeight = std::max(1, getHeight());
  asyncImageBound = true;
  asyncTargetWidth = targetWidth;
  asyncTargetHeight = targetHeight;
  const std::string key =
      isArchiveEntryImagePath(std::filesystem::path(path))
          ? imageCacheKey(path, targetWidth, targetHeight)
          : imageAsyncCacheKey(path, targetWidth, targetHeight);
  if (currentImageKey == key) {
    if (applyCachedTexture(path, key)) {
      asyncImagePending = false;
      return true;
    }
    if (!bgfx::isValid(texture)) {
      applyCachedThumbnail(path, key);
    }
    if (asyncTicket == 0) {
      asyncTicket = imageDecodeCoordinator().request(
          {.key = key,
           .path = std::filesystem::path(path),
           .targetWidth = targetWidth,
           .targetHeight = targetHeight,
           .priority = prioritize});
    }
    if (asyncTicket != 0 &&
        !imageDecodeCoordinator().hasFailed(asyncTicket)) {
      asyncImagePending = true;
    } else {
      asyncImagePending = false;
    }
    return false;
  }

  cancelAsyncRequest();
  if (currentImagePath != path) {
    freeTexture();
  }
  currentImageKey = key;
  currentImagePath = path;
  if (applyCachedTexture(path, key)) {
    asyncImagePending = false;
    return true;
  }
  applyCachedThumbnail(path, key);
  asyncTicket = imageDecodeCoordinator().request(
      {.key = key,
       .path = std::filesystem::path(path),
       .targetWidth = targetWidth,
       .targetHeight = targetHeight,
       .priority = prioritize});
  asyncImagePending =
      asyncTicket != 0 && !imageDecodeCoordinator().hasFailed(asyncTicket);
  return false;
}
void ImageView::freeImage() {
  cancelAsyncRequest();
  currentImageKey.clear();
  currentImagePath.clear();
  asyncImageBound = false;
  asyncTargetWidth = 0;
  asyncTargetHeight = 0;
  asyncImagePending = false;
  freeTexture();
}
ImageView *ImageView::setFade(ImageFadeDirection direction, float strength) {
  fade_ = makeImageFade(direction, strength);
  return this;
}
ImageView *ImageView::clearFade() {
  fade_.reset();
  return this;
}
ImageView *ImageView::setScrimColor(const Color &color) {
  themedScrimColorProvider_ = {};
  scrimColor_ = color;
  return this;
}
ImageView *ImageView::setThemedScrimColor(ThemeColorProvider provider) {
  themedScrimColorProvider_ = std::move(provider);
  if (themedScrimColorProvider_) {
    scrimColor_ = themedScrimColorProvider_();
  } else {
    scrimColor_.reset();
  }
  return this;
}
ImageView *ImageView::clearScrimColor() {
  themedScrimColorProvider_ = {};
  scrimColor_.reset();
  return this;
}
void ImageView::onThemeChanged() {
  View::onThemeChanged();
  if (themedScrimColorProvider_) {
    scrimColor_ = themedScrimColorProvider_();
  }
}
void ImageView::onLayout() {
  View::onLayout();
  if (!asyncImageBound || currentImagePath.empty()) {
    return;
  }
  const int targetWidth = std::max(1, getWidth());
  const int targetHeight = std::max(1, getHeight());
  if (targetWidth <= asyncTargetWidth && targetHeight <= asyncTargetHeight) {
    return;
  }
  const path_t path = currentImagePath;
  setImageAsync(path, true);
}
void ImageView::renderImpl(RenderContext &context) {
  applyAsyncImageIfReady();
  if (!bgfx::isValid(texture)) {
    return;
  }
  bgfx::ProgramHandle program;
  if (fade_.has_value() || scrimColor_.has_value()) {
    const ImageFade fade = fade_.value_or(
        makeImageFade(ImageFadeDirection::LeftToRight, 0.0F));
    const auto fadeParams = imageFadeShaderParameters(fade);
    bgfx::setUniform(
        rendering::UniformCache::getInstance().getVec4("u_imageFadeParams"),
        fadeParams.data());

    const Color scrim = scrimColor_.value_or(Color(0, 0, 0, 0));
    constexpr float inv255 = 1.0F / 255.0F;
    const std::array<float, 4> scrimParams = {
        static_cast<float>(scrim.r) * inv255,
        static_cast<float>(scrim.g) * inv255,
        static_cast<float>(scrim.b) * inv255,
        static_cast<float>(scrim.a) * inv255};
    bgfx::setUniform(
        rendering::UniformCache::getInstance().getVec4("u_imageScrimColor"),
        scrimParams.data());
    program = rendering::ShaderManager::getInstance().getProgram(
        "vs_text.bin", "fs_image_fade.bin");
  } else {
    program = rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  }
  submitTexturedRoundedRect(context, texture, s_texColor, getX(), getY(),
                            getWidth(), getHeight(), getCornerRadius(), program);
}
ImageView::ImageView(int x, int y, int width, int height)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}
void ImageView::dropCache(const path_t &path) {
  const std::string asyncPrefix = imageAsyncCacheSourceKey(path) + ":fit=";
  (void)imageCache.erasePrefix(asyncPrefix);
  imageDecodeCoordinator().dropPrefix(asyncPrefix);
  const std::string metadataPrefix = imageCacheSourceKey(path) + ":fit=";
  (void)imageCache.erasePrefix(metadataPrefix);
  imageDecodeCoordinator().dropPrefix(metadataPrefix);
}
void ImageView::dropAllCache() {
  imageCache.clear();
  imageDecodeCoordinator().dropAll();
}
void ImageView::evictDecodedImageCache() {
  imageCache.clearEvictable();
  imageDecodeCoordinator().dropAll();
}

#if defined(ASOBMASHOW_IMAGE_VIEW_TESTING)
std::size_t
ImageView::pendingAsyncDecodeCountForTesting(const path_t &path) {
  return imageDecodeCoordinator().pendingCountPrefix(
      imageAsyncCacheSourceKey(path) + ":fit=");
}
std::size_t ImageView::decodedImageCacheBytesForTesting() {
  return imageCache.bytes();
}
#endif
