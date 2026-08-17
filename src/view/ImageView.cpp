//
// Created by XF on 8/28/2024.
//

#include "ImageView.h"
#include "../ArchiveFile.h"
#include "ImageFileDecoder.h"
#include "../path.h"
#include "../targets.h"
#if !TARGET_OS_WINDOWS
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif
#include "../RAII.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include <bgfx/bgfx.h>
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
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kArchivedThumbnailMaxDimension = 256;
constexpr int kImageMaximumDimension =
    static_cast<int>(std::numeric_limits<std::uint16_t>::max());
constexpr std::size_t kImageMaximumEncodedBytes = 32U * 1024U * 1024U;
#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
constexpr std::size_t kImageMaximumDecodedBytes = 64U * 1024U * 1024U;
#else
constexpr std::size_t kImageMaximumDecodedBytes =
    128U * 1024U * 1024U;
#endif
constexpr auto kInitialFifoWriterWait = std::chrono::milliseconds(200);
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

image_decode::ImageDecodeOptions imageDecodeOptions(
    int targetWidth = 0, int targetHeight = 0, std::stop_token stop = {}) {
  return {.maximumDimension = kImageMaximumDimension,
          .maximumEncodedBytes = kImageMaximumEncodedBytes,
          .maximumDecodedBytes = kImageMaximumDecodedBytes,
          .targetWidth = targetWidth,
          .targetHeight = targetHeight,
          .stop = stop};
}

#if !TARGET_OS_WINDOWS
std::optional<std::vector<std::byte>> readBoundedDescriptorBytes(
    int descriptor, std::size_t maximumBytes, std::stop_token stop,
    bool waitForInitialFifoWriter = false) {
  std::vector<std::byte> bytes;
  std::array<std::byte, 64U * 1024U> chunk{};
  const auto initialFifoWriterDeadline =
      std::chrono::steady_clock::now() + kInitialFifoWriterWait;
  bool fifoWriterObserved = false;
  for (;;) {
    if (stop.stop_requested()) return std::nullopt;
    const ssize_t count = read(descriptor, chunk.data(), chunk.size());
    if (count == 0) {
      if (stop.stop_requested()) return std::nullopt;
      if (!bytes.empty()) return bytes;
      if (!waitForInitialFifoWriter) return std::nullopt;
      if (fifoWriterObserved ||
          std::chrono::steady_clock::now() >= initialFifoWriterDeadline) {
        return std::nullopt;
      }

      // A nonblocking FIFO has no writer yet. Keep the read end alive so a
      // producer can attach, while poll bounds cancellation latency and the
      // initial wait prevents a dead producer from occupying a decode worker.
    }
    if (count > 0) {
      const auto readCount = static_cast<std::size_t>(count);
      if (readCount > maximumBytes - bytes.size()) return std::nullopt;
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
      continue;
    }

    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) return std::nullopt;
      fifoWriterObserved = fifoWriterObserved || waitForInitialFifoWriter;
    }

    pollfd descriptorPoll{.fd = descriptor, .events = POLLIN, .revents = 0};
    const int ready = poll(&descriptorPoll, 1, 25);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    if (ready == 0) continue;
    if (descriptorPoll.revents & (POLLERR | POLLNVAL)) return std::nullopt;
    if ((descriptorPoll.revents & POLLHUP) && !bytes.empty()) return bytes;
    if ((descriptorPoll.revents & POLLHUP) && waitForInitialFifoWriter) {
      return std::nullopt;
    }
  }
}
#endif

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

std::optional<DecodedImage> makeArchivedThumbnail(const DecodedImage &decoded,
                                                   std::stop_token stop) {
  return image_decode::resizeDecodedImage(
      decoded, imageDecodeOptions(kArchivedThumbnailMaxDimension,
                                  kArchivedThumbnailMaxDimension, stop));
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
                int targetHeight = 0, std::stop_token stop = {}) {
  ImageDecodeTimings measured;
  const bool archiveEntryPath = isArchiveEntryImagePath(path);
  const auto options = imageDecodeOptions(targetWidth, targetHeight, stop);
  if (stop.stop_requested()) return std::nullopt;
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
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
    const auto closeDescriptor = makeScopeExit([fd = *fd] { (void)close(fd); });
    const int flags = fcntl(*fd, F_GETFL);
    if (flags < 0 || fcntl(*fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      SDL_Log("Failed to make Android image descriptor nonblocking: %s",
              fspath_to_utf8(path).c_str());
      return std::nullopt;
    }
    struct stat status {};
    if (fstat(*fd, &status) != 0 ||
        (S_ISREG(status.st_mode) &&
         (status.st_size <= 0 || static_cast<std::uintmax_t>(status.st_size) >
                                      kImageMaximumEncodedBytes))) {
      SDL_Log("Android image descriptor is outside encoded byte bounds: %s",
              fspath_to_utf8(path).c_str());
      return std::nullopt;
    }
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    const auto bytes = readBoundedDescriptorBytes(*fd,
                                                  kImageMaximumEncodedBytes,
                                                  stop);
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
    if (!bytes || stop.stop_requested()) return std::nullopt;
    const auto decodeStarted = std::chrono::steady_clock::now();
    auto decoded = image_decode::decodeImageMemory(*bytes, options);
    measured.sourceLoadDecodeMillis += elapsedMillis(
        decodeStarted, std::chrono::steady_clock::now());
    if (timings != nullptr) *timings = measured;
    return decoded;
  }
#endif
  if (!archive_file::isVirtualPath(path)) {
#if !TARGET_OS_WINDOWS
    std::error_code statusError;
    const auto status = std::filesystem::status(path, statusError);
    if (!statusError && !std::filesystem::is_regular_file(status)) {
      const auto sourceAccessStarted = std::chrono::steady_clock::now();
      const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK);
      measured.sourceAccessMillis = elapsedMillis(
          sourceAccessStarted, std::chrono::steady_clock::now());
      if (descriptor < 0) return std::nullopt;
      const auto closeDescriptor =
          makeScopeExit([descriptor] { (void)close(descriptor); });
      const auto sourceLoadStarted = std::chrono::steady_clock::now();
      const auto bytes = readBoundedDescriptorBytes(
          descriptor, kImageMaximumEncodedBytes, stop,
          std::filesystem::is_fifo(status));
      measured.sourceLoadDecodeMillis = elapsedMillis(
          sourceLoadStarted, std::chrono::steady_clock::now());
      if (!bytes) return std::nullopt;
      const auto decodeStarted = std::chrono::steady_clock::now();
      auto decoded = image_decode::decodeImageMemory(*bytes, options);
      measured.sourceLoadDecodeMillis += elapsedMillis(
          decodeStarted, std::chrono::steady_clock::now());
      if (timings != nullptr) *timings = measured;
      return decoded;
    }
#endif
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    auto decoded = image_decode::decodeImageFile(path, options);
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
    if (timings != nullptr) *timings = measured;
    return decoded;
  } else {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    const auto sourceAccessStarted = std::chrono::steady_clock::now();
    if (!archive_file::readFileBounded(path, bytes,
                                       kImageMaximumEncodedBytes,
                                       &errorMessage, stop)) {
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
    if (stop.stop_requested()) {
      if (timings != nullptr) *timings = measured;
      return std::nullopt;
    }
    const auto sourceLoadStarted = std::chrono::steady_clock::now();
    auto decoded = image_decode::decodeImageMemory(
        std::span<const std::byte>(reinterpret_cast<const std::byte *>(bytes.data()),
                                   bytes.size()),
        imageDecodeOptions(0, 0, stop));
    measured.sourceLoadDecodeMillis = elapsedMillis(
        sourceLoadStarted, std::chrono::steady_clock::now());
    if (!decoded.has_value()) {
      if (timings != nullptr) *timings = measured;
      return std::nullopt;
    }
    if (archiveEntryPath) {
      const auto archivePreviewStarted = std::chrono::steady_clock::now();
      if (const auto thumbnail = makeArchivedThumbnail(*decoded, stop);
          thumbnail && !stop.stop_requested()) {
        writeCachedArchivedThumbnail(path, *thumbnail);
      }
      measured.archivePreviewMillis = elapsedMillis(
          archivePreviewStarted, std::chrono::steady_clock::now());
    }
    if (timings != nullptr) *timings = measured;
    return image_decode::resizeDecodedImage(*decoded, options);
  }
}


image_decode::ImageDecodeCoordinator &imageDecodeCoordinator() {
  static image_decode::ImageDecodeCoordinator coordinator(
      [](const image_decode::ImageDecodeRequest &request, std::stop_token stop)
          -> std::optional<DecodedImage> {
        const auto started = std::chrono::steady_clock::now();
        ImageDecodeTimings timings;
        auto decoded = decodeImageFile(request.path, &timings,
                                       request.targetWidth,
                                       request.targetHeight, stop);
        const auto workerMillis = elapsedMillis(
            started, std::chrono::steady_clock::now());
        if (workerMillis >= 250) {
          const std::string diagnostic =
              "Slow async image load: source=" +
              std::to_string(timings.sourceAccessMillis) +
              "ms load_decode=" +
              std::to_string(timings.sourceLoadDecodeMillis) +
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
                               bgfx::ProgramHandle program,
                               std::span<const rendering::UiBatchUniform> uniforms) {
  if (!bgfx::isValid(texture) || width <= 0 || height <= 0) {
    return;
  }

  radius = std::clamp(radius, 0.0f,
                      static_cast<float>(std::min(width, height)) * 0.5f);

  std::vector<rendering::PosTexCoord0Vertex> vertices;
  std::vector<uint16_t> indices;
  if (radius <= 0.5f) {
    constexpr uint32_t kVertexCount = 4;
    constexpr uint32_t kIndexCount = 6;
    vertices.reserve(kVertexCount);
    indices.reserve(kIndexCount);
    vertices = {{static_cast<float>(x), static_cast<float>(y + height), 0.0f,
                 0.0f, 1.0f},
                {static_cast<float>(x + width), static_cast<float>(y + height),
                 0.0f, 1.0f, 1.0f},
                {static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f,
                 0.0f},
                {static_cast<float>(x + width), static_cast<float>(y), 0.0f,
                 1.0f, 0.0f}};
    indices = {0, 1, 2, 1, 3, 2};
  } else {
    const int segments =
        std::clamp(static_cast<int>(std::ceil(radius / 4.0f)), 4, 12);
    const uint16_t ringVertexCount = static_cast<uint16_t>((segments + 1) * 4);
    const uint16_t vertexCount = static_cast<uint16_t>(ringVertexCount + 1);
    const uint16_t indexCount = static_cast<uint16_t>(ringVertexCount * 3);

    vertices.reserve(vertexCount);
    indices.reserve(indexCount);

    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);
    const auto appendVertex = [&](float px, float py) {
      vertices.push_back({px, py, 0.0f, (px - fx) / fw, (py - fy) / fh});
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

    for (uint16_t i = 0; i < ringVertexCount; ++i) {
      indices.push_back(0);
      indices.push_back(static_cast<uint16_t>(i + 1));
      indices.push_back(static_cast<uint16_t>((i + 1) % ringVertexCount + 1));
    }
  }
  auto state = context.makeUiBatchState(
      program, BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
  state.texture = texture;
  state.sampler = sampler;
  state.uniformCount = std::min(uniforms.size(), state.uniforms.size());
  std::copy_n(uniforms.begin(), state.uniformCount, state.uniforms.begin());
  context.appendUiTextured(vertices, indices, state);
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
  std::array<rendering::UiBatchUniform, 2> uniforms;
  std::size_t uniformCount = 0;
  if (fade_.has_value() || scrimColor_.has_value()) {
    const ImageFade fade = fade_.value_or(
        makeImageFade(ImageFadeDirection::LeftToRight, 0.0F));
    const auto fadeParams = imageFadeShaderParameters(fade);
    uniforms[uniformCount++] = {
        .handle =
            rendering::UniformCache::getInstance().getVec4("u_imageFadeParams"),
        .value = {fadeParams[0], fadeParams[1], fadeParams[2], fadeParams[3]}};

    const Color scrim = scrimColor_.value_or(Color(0, 0, 0, 0));
    constexpr float inv255 = 1.0F / 255.0F;
    const std::array<float, 4> scrimParams = {
        static_cast<float>(scrim.r) * inv255,
        static_cast<float>(scrim.g) * inv255,
        static_cast<float>(scrim.b) * inv255,
        static_cast<float>(scrim.a) * inv255};
    uniforms[uniformCount++] = {
        .handle =
            rendering::UniformCache::getInstance().getVec4("u_imageScrimColor"),
        .value = {scrimParams[0], scrimParams[1], scrimParams[2],
                  scrimParams[3]}};
    program = rendering::ShaderManager::getInstance().getProgram(
        "vs_text.bin", "fs_image_fade.bin");
  } else {
    program = rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  }
  submitTexturedRoundedRect(context, texture, s_texColor, getX(), getY(),
                            getWidth(), getHeight(), getCornerRadius(), program,
                            std::span<const rendering::UiBatchUniform>(uniforms)
                                .first(uniformCount));
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
