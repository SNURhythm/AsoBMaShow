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
#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../bgfx/bimg/3rdparty/stb/stb_image_resize.h"
#include "../StbImageRAII.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kArchivedThumbnailMaxDimension = 256;
constexpr std::array<unsigned char, 8> kArchivedThumbnailMagic = {
    'A', 'S', 'O', 'B', 'T', 'H', 'M', '1'};

struct DecodedImage {
  int width = 0;
  int height = 0;
  std::shared_ptr<std::vector<unsigned char>> rgba;
};

std::string imageCacheKey(const path_t &path) {
  return archive_file::cacheKeyForPath(std::filesystem::path(path));
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

void writeCachedArchivedThumbnail(const std::filesystem::path &path,
                                  const DecodedImage &thumbnail) {
  if (!thumbnail.rgba ||
      !decodedImageDimensionsAreValid(thumbnail.width, thumbnail.height) ||
      thumbnail.width > kArchivedThumbnailMaxDimension ||
      thumbnail.height > kArchivedThumbnailMaxDimension) {
    return;
  }

  std::vector<unsigned char> bytes;
  bytes.reserve(kArchivedThumbnailMagic.size() + 8U + thumbnail.rgba->size());
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

std::optional<DecodedImage> decodeImageFile(const std::filesystem::path &path) {
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
    const auto fd = OpenAndroidTreeFileDescriptor(path, fdError);
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
    data.reset(stbi_load_from_file(file, &width, &height, &channels, 4));
    fclose(file);
  }
  if (!androidTreePath && !archive_file::isVirtualPath(path)) {
#else
  if (!archive_file::isVirtualPath(path)) {
#endif
    const std::string utf8Path = fspath_to_utf8(path);
    data.reset(stbi_load(utf8Path.c_str(), &width, &height, &channels, 4));
  } else {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    if (!archive_file::readFile(path, bytes, &errorMessage)) {
      SDL_Log("Failed to read archived image %s: %s",
              fspath_to_utf8(path).c_str(), errorMessage.c_str());
      return std::nullopt;
    }
    data.reset(stbi_load_from_memory(bytes.data(),
                                     static_cast<int>(bytes.size()), &width,
                                     &height, &channels, 4));
  }

  if (data == nullptr || !decodedImageDimensionsAreValid(width, height)) {
    return std::nullopt;
  }

  const size_t byteCount = static_cast<size_t>(width) *
                           static_cast<size_t>(height) * 4;
  auto rgba = std::make_shared<std::vector<unsigned char>>(byteCount);
  std::copy(data.get(), data.get() + byteCount, rgba->begin());
  DecodedImage decoded{.width = width, .height = height, .rgba = rgba};
  if (archiveEntryPath) {
    DecodedImage thumbnail = makeArchivedThumbnail(decoded);
    writeCachedArchivedThumbnail(path, thumbnail);
  }
  return decoded;
}

class ImageDecodeWorker {
public:
  static ImageDecodeWorker &instance() {
    static ImageDecodeWorker worker;
    return worker;
  }

  void request(const path_t &path, bool prioritize = false) {
    const std::string key = imageCacheKey(path);
    std::lock_guard<std::mutex> lock(mutex);
    if (ready.contains(key) || failed.contains(key) ||
        inFlight.contains(key)) {
      return;
    }
    if (queued.contains(key)) {
      if (prioritize) {
        promoteQueuedTask(key);
      }
      return;
    }
    Task task{.key = key, .path = path, .generation = generation};
    if (prioritize) {
      queue.push_back(std::move(task));
    } else {
      queue.push_front(std::move(task));
    }
    queued.insert(key);
    cv.notify_one();
  }

  std::optional<DecodedImage> takeReady(const path_t &path) {
    const std::string key = imageCacheKey(path);
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = ready.find(key);
    if (it == ready.end()) {
      return std::nullopt;
    }
    std::optional<DecodedImage> decoded(std::move(it->second));
    ready.erase(it);
    return decoded;
  }

  bool hasFailed(const path_t &path) {
    const std::string key = imageCacheKey(path);
    std::lock_guard<std::mutex> lock(mutex);
    return failed.contains(key);
  }

  void drop(const path_t &path) {
    const std::string key = imageCacheKey(path);
    std::lock_guard<std::mutex> lock(mutex);
    ready.erase(key);
    failed.erase(key);
    queued.erase(key);
    queue.erase(std::remove_if(queue.begin(), queue.end(),
                               [&key](const Task &task) {
                                 return task.key == key;
                               }),
                queue.end());
  }

  void dropAll() {
    std::lock_guard<std::mutex> lock(mutex);
    ++generation;
    queue.clear();
    queued.clear();
    ready.clear();
    failed.clear();
  }

private:
  struct Task {
    std::string key;
    path_t path;
    std::uint64_t generation = 0;
  };

  ImageDecodeWorker() : worker([this] { run(); }) {}

  ~ImageDecodeWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stop = true;
    }
    cv.notify_one();
    if (worker.joinable()) {
      worker.join();
    }
  }

  void run() {
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return stop || !queue.empty(); });
        if (stop) {
          return;
        }
        task = std::move(queue.back());
        queue.pop_back();
        queued.erase(task.key);
        inFlight.insert(task.key);
      }

      std::optional<DecodedImage> decoded =
          decodeImageFile(std::filesystem::path(task.path));

      {
        std::lock_guard<std::mutex> lock(mutex);
        inFlight.erase(task.key);
        if (task.generation != generation) {
          continue;
        }
        if (decoded.has_value()) {
          ready[task.key] = *decoded;
        } else {
          failed.insert(task.key);
        }
      }
    }
  }

  void promoteQueuedTask(const std::string &key) {
    const auto it = std::find_if(queue.begin(), queue.end(),
                                 [&key](const Task &task) {
                                   return task.key == key;
                                 });
    if (it == queue.end()) {
      return;
    }
    Task task = std::move(*it);
    queue.erase(it);
    queue.push_back(std::move(task));
    cv.notify_one();
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Task> queue;
  std::unordered_set<std::string> queued;
  std::unordered_set<std::string> inFlight;
  std::unordered_set<std::string> failed;
  std::map<std::string, DecodedImage> ready;
  std::thread worker;
  std::uint64_t generation = 0;
  bool stop = false;
};

void submitTexturedRoundedRect(const RenderContext &context,
                               bgfx::TextureHandle texture,
                               bgfx::UniformHandle sampler, int x, int y,
                               int width, int height, float radius) {
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
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA |
                 BGFX_STATE_MSAA);
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  bgfx::submit(rendering::ui_view, kProgram);
}
} // namespace

std::map<std::string, ImageView::ImageCache> ImageView::imageCache = {};
ImageView::ImageView(int x, int y, int width, int height, const path_t &path)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
  loadTexture(path);
}
ImageView::~ImageView() { freeTexture(); }

bool ImageView::applyImage(const path_t &path, const ImageCache &cache,
                           bool storeCache) {
  freeTexture();
  const std::string key = imageCacheKey(path);
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
    imageCache[key] = cache;
  }
  return true;
}

bool ImageView::applyCachedTexture(const path_t &path) {
  const std::string key = imageCacheKey(path);
  const auto localIt = imageCache.find(key);
  if (localIt != imageCache.end()) {
    return applyImage(path, localIt->second);
  }
  if (const auto loaded = ImageDecodeWorker::instance().takeReady(path)) {
    return applyImage(path, {.width = loaded->width,
                             .height = loaded->height,
                             .rgba = loaded->rgba});
  }
  return false;
}

bool ImageView::applyCachedThumbnail(const path_t &path) {
  const auto thumbnail =
      readCachedArchivedThumbnail(std::filesystem::path(path));
  if (!thumbnail.has_value()) {
    return false;
  }
  return applyImage(path, {.width = thumbnail->width,
                           .height = thumbnail->height,
                           .rgba = thumbnail->rgba},
                    false);
}

void ImageView::applyAsyncImageIfReady() {
  if (currentImageKey.empty() || !asyncImagePending) {
    return;
  }
  if (applyCachedTexture(currentImagePath)) {
    asyncImagePending = false;
    return;
  }
  if (ImageDecodeWorker::instance().hasFailed(currentImagePath)) {
    asyncImagePending = false;
  }
}

bool ImageView::loadTexture(const path_t &path) {
  const std::string utf8Path = path_t_to_utf8(path);
  const std::string key = imageCacheKey(path);
  if (currentImageKey == key && bgfx::isValid(texture) &&
      !asyncImagePending) {
    return true;
  }
  asyncImagePending = false;
  if (applyCachedTexture(path)) {
    return true;
  }

  const auto decoded = decodeImageFile(std::filesystem::path(path));
  if (!decoded.has_value()) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s; width: %d; height: %d", utf8Path.c_str(),
          decoded->width, decoded->height);
  return applyImage(path, {.width = decoded->width,
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

bool ImageView::setImage(const path_t &path) { return loadTexture(path); }
bool ImageView::setImageAsync(const path_t &path, bool prioritize) {
  const std::string key = imageCacheKey(path);
  if (currentImageKey == key) {
    if (applyCachedTexture(path)) {
      asyncImagePending = false;
      return true;
    }
    if (!bgfx::isValid(texture)) {
      applyCachedThumbnail(path);
    }
    if (!ImageDecodeWorker::instance().hasFailed(path)) {
      asyncImagePending = true;
      ImageDecodeWorker::instance().request(path, prioritize);
    } else {
      asyncImagePending = false;
    }
    return false;
  }

  freeTexture();
  currentImageKey = key;
  currentImagePath = path;
  if (applyCachedTexture(path)) {
    asyncImagePending = false;
    return true;
  }
  applyCachedThumbnail(path);
  ImageDecodeWorker::instance().request(path, prioritize);
  asyncImagePending = !ImageDecodeWorker::instance().hasFailed(path);
  return false;
}
void ImageView::freeImage() {
  currentImageKey.clear();
  currentImagePath.clear();
  asyncImagePending = false;
  freeTexture();
}
void ImageView::renderImpl(RenderContext &context) {
  applyAsyncImageIfReady();
  if (!bgfx::isValid(texture)) {
    return;
  }
  submitTexturedRoundedRect(context, texture, s_texColor, getX(), getY(),
                            getWidth(), getHeight(), getCornerRadius());
}
ImageView::ImageView(int x, int y, int width, int height)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}
void ImageView::dropCache(const path_t &path) {
  const std::string key = imageCacheKey(path);
  imageCache.erase(key);
  ImageDecodeWorker::instance().drop(path);
}
void ImageView::dropAllCache() {
  imageCache.clear();
  ImageDecodeWorker::instance().dropAll();
}
