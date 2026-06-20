//
// Created by XF on 8/28/2024.
//

#include "ImageView.h"
#include "../ArchiveFile.h"
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
#include "../StbImageRAII.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {
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

std::optional<DecodedImage> decodeImageFile(const std::filesystem::path &path) {
  int width = 0;
  int height = 0;
  int channels = 0;
  StbiImageHandle data(nullptr);
  if (!archive_file::isVirtualPath(path)) {
    const std::string utf8Path = path_t_to_utf8(fspath_to_path_t(path));
    data.reset(stbi_load(utf8Path.c_str(), &width, &height, &channels, 4));
  } else {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    if (!archive_file::readFile(path, bytes, &errorMessage)) {
      SDL_Log("Failed to read archived image %s: %s",
              path_t_to_utf8(fspath_to_path_t(path)).c_str(),
              errorMessage.c_str());
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
  return DecodedImage{.width = width, .height = height, .rgba = rgba};
}

class ImageDecodeWorker {
public:
  static ImageDecodeWorker &instance() {
    static ImageDecodeWorker worker;
    return worker;
  }

  void request(const path_t &path) {
    const std::string key = imageCacheKey(path);
    std::lock_guard<std::mutex> lock(mutex);
    if (ready.contains(key) || failed.contains(key) || queued.contains(key) ||
        inFlight.contains(key)) {
      return;
    }
    queue.push_back({.key = key, .path = path, .generation = generation});
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
} // namespace

std::map<std::string, ImageView::ImageCache> ImageView::imageCache = {};
ImageView::ImageView(int x, int y, int width, int height, const path_t &path)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
  loadTexture(path);
}
ImageView::~ImageView() { freeTexture(); }

bool ImageView::applyImage(const path_t &path, const ImageCache &cache) {
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
  imageCache[key] = cache;
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

void ImageView::applyAsyncImageIfReady() {
  if (currentImageKey.empty() || bgfx::isValid(texture)) {
    return;
  }
  applyCachedTexture(currentImagePath);
}

bool ImageView::loadTexture(const path_t &path) {
  const std::string utf8Path = path_t_to_utf8(path);
  const std::string key = imageCacheKey(path);
  if (currentImageKey == key && bgfx::isValid(texture)) {
    return true;
  }
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
}

bool ImageView::setImage(const path_t &path) { return loadTexture(path); }
bool ImageView::setImageAsync(const path_t &path) {
  const std::string key = imageCacheKey(path);
  if (currentImageKey == key) {
    if (bgfx::isValid(texture)) {
      return true;
    }
    if (applyCachedTexture(path)) {
      return true;
    }
    if (!ImageDecodeWorker::instance().hasFailed(path)) {
      ImageDecodeWorker::instance().request(path);
    }
    return false;
  }

  freeTexture();
  currentImageKey = key;
  currentImagePath = path;
  if (applyCachedTexture(path)) {
    return true;
  }
  ImageDecodeWorker::instance().request(path);
  return false;
}
void ImageView::freeImage() {
  currentImageKey.clear();
  currentImagePath.clear();
  freeTexture();
}
void ImageView::renderImpl(RenderContext &context) {
  applyAsyncImageIfReady();
  if (!bgfx::isValid(texture)) {
    return;
  }
  // Submit a quad with the image texture
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  //  SDL_Log("Rendering video texture frame %d; time: %f", currentFrame,
  //  currentFrame / 30.0f);

  bgfx::allocTransientVertexBuffer(&tvb, 4,
                                   rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  // Define quad vertices
  vertex[0].x = getX();
  vertex[0].y = getY() + getHeight();
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;
  vertex[1].x = getX() + getWidth();
  vertex[1].y = getY() + getHeight();
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;
  vertex[2].x = getX();
  vertex[2].y = getY();
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;
  vertex[3].x = getX() + getWidth();
  vertex[3].y = getY();
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;

  // Define quad indices
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  bgfx::setTexture(0, s_texColor, texture);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  bgfx::submit(rendering::ui_view, kProgram);
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
