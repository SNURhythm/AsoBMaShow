#include "Jukebox.h"
#include "../ArchiveFile.h"
#include <SDL2/SDL.h>
#include <thread>
#include "../Utils.h"
#include "../game/GameState.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include "bgfx/bgfx.h"
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#ifdef _WIN32
#include <timeapi.h>
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

namespace {
constexpr long long kSchedulerTickMicros = 1000000LL / 8000;
constexpr long long kSchedulerMaxIdleSleepMicros = 250000;

std::vector<std::string_view> toExtensionViews(const std::string *extensions,
                                               size_t count) {
  std::vector<std::string_view> views;
  views.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    views.emplace_back(extensions[i]);
  }
  return views;
}

std::optional<std::filesystem::path>
findWithReplacedExtensions(const std::filesystem::path &basePath,
                           const std::string *extensions, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    std::filesystem::path path = basePath;
    path.replace_extension(extensions[i]);
    if (auto resolved = archive_file::findFileWithExtensions(path, {})) {
      return resolved;
    }
  }
  return std::nullopt;
}

unsigned char *decodeImageBytes(const std::vector<unsigned char> &bytes,
                                int *width, int *height, int *channels,
                                int requestedChannels) {
  if (bytes.empty() ||
      bytes.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return nullptr;
  }
  return stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                               width, height, channels, requestedChannels);
}

unsigned char *loadImageFile(const std::filesystem::path &path, int *width,
                             int *height, int *channels,
                             int requestedChannels) {
  if (!archive_file::isVirtualPath(path)) {
    const std::string utf8Path = path_t_to_utf8(fspath_to_path_t(path));
    return stbi_load(utf8Path.c_str(), width, height, channels,
                     requestedChannels);
  }

  std::vector<unsigned char> bytes;
  std::string errorMessage;
  if (!archive_file::readFile(path, bytes, &errorMessage)) {
    SDL_Log("Failed to read archived image %s: %s",
            path_t_to_utf8(fspath_to_path_t(path)).c_str(),
            errorMessage.c_str());
    return nullptr;
  }
  return decodeImageBytes(bytes, width, height, channels, requestedChannels);
}

struct ArchiveAssetBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_map<path_t, std::vector<int>> idsByPath;
};

bool addArchiveAssetTarget(
    std::unordered_map<path_t, ArchiveAssetBatch> &batches,
    std::vector<path_t> &batchOrder, const std::filesystem::path &path,
    int id) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!archive_file::splitVirtualPath(path, archivePath, innerPath)) {
    return false;
  }

  const path_t archiveKey = fspath_to_path_t(archivePath);
  auto batchIt = batches.find(archiveKey);
  if (batchIt == batches.end()) {
    batchOrder.push_back(archiveKey);
    batchIt =
        batches
            .emplace(archiveKey, ArchiveAssetBatch{
                                     .archivePath = archivePath,
                                     .innerPaths = {},
                                     .idsByPath = {},
                                 })
            .first;
  }

  const path_t pathKey = fspath_to_path_t(path);
  auto &ids = batchIt->second.idsByPath[pathKey];
  if (ids.empty()) {
    batchIt->second.innerPaths.push_back(innerPath);
  }
  ids.push_back(id);
  return true;
}

std::string lowerAsciiCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string normalizeArchiveLookupPath(const std::filesystem::path &path) {
  std::filesystem::path normalized = path.lexically_normal();
  std::string value = normalized.generic_string();
  while (!value.empty() && value.front() == '/') {
    value.erase(value.begin());
  }
  if (value == ".") {
    value.clear();
  }
  return value;
}

class ArchiveEntryLookup {
public:
  bool load(const std::filesystem::path &archivePath,
            std::string *errorMessage) {
    entries.clear();
    exact.clear();
    lower.clear();
    if (!archive_file::listEntries(archivePath, entries, errorMessage)) {
      return false;
    }
    for (const auto &entry : entries) {
      if (entry.directory) {
        continue;
      }
      const std::string normalized = normalizeArchiveLookupPath(entry.path);
      if (normalized.empty()) {
        continue;
      }
      exact.emplace(normalized, entry.path);
      lower.emplace(lowerAsciiCopy(normalized), entry.path);
    }
    return true;
  }

  std::optional<std::filesystem::path>
  find(const std::filesystem::path &innerPath,
       const std::vector<std::string_view> &extensions) const {
    if (const auto resolved = findExactOrLower(innerPath)) {
      return resolved;
    }
    for (std::string_view ext : extensions) {
      std::filesystem::path candidate = innerPath;
      candidate.replace_extension(std::string(ext));
      if (const auto resolved = findExactOrLower(candidate)) {
        return resolved;
      }
    }
    return std::nullopt;
  }

  std::optional<std::filesystem::path>
  findReplacedExtensions(const std::filesystem::path &innerPath,
                         const std::string *extensions, size_t count) const {
    for (size_t i = 0; i < count; ++i) {
      std::filesystem::path candidate = innerPath;
      candidate.replace_extension(extensions[i]);
      if (const auto resolved = findExactOrLower(candidate)) {
        return resolved;
      }
    }
    return std::nullopt;
  }

private:
  std::optional<std::filesystem::path>
  findExactOrLower(const std::filesystem::path &innerPath) const {
    const std::string normalized = normalizeArchiveLookupPath(innerPath);
    const auto exactIt = exact.find(normalized);
    if (exactIt != exact.end()) {
      return exactIt->second;
    }

    const auto lowerIt = lower.find(lowerAsciiCopy(normalized));
    if (lowerIt != lower.end()) {
      return lowerIt->second;
    }
    return std::nullopt;
  }

  std::vector<archive_file::Entry> entries;
  std::unordered_map<std::string, std::filesystem::path> exact;
  std::unordered_map<std::string, std::filesystem::path> lower;
};

ArchiveEntryLookup *getArchiveLookup(
    const std::filesystem::path &archivePath,
    std::unordered_map<path_t, ArchiveEntryLookup> &lookups) {
  const path_t archiveKey = fspath_to_path_t(archivePath);
  auto [it, inserted] = lookups.emplace(archiveKey, ArchiveEntryLookup{});
  if (!inserted) {
    return &it->second;
  }

  std::string errorMessage;
  if (!it->second.load(archivePath, &errorMessage)) {
    SDL_Log("Failed to index archive %s: %s",
            path_t_to_utf8(archiveKey).c_str(), errorMessage.c_str());
    lookups.erase(it);
    return nullptr;
  }
  return &lookups.find(archiveKey)->second;
}

std::optional<archive_file::EntryRange>
entryRangeForChartArchive(const bms_parser::Chart &chart,
                          const std::filesystem::path &archivePath) {
  std::filesystem::path chartArchivePath;
  std::filesystem::path chartInnerPath;
  if (!archive_file::splitVirtualPath(chart.Meta.BmsPath, chartArchivePath,
                                      chartInnerPath)) {
    return std::nullopt;
  }
  if (fspath_to_path_t(chartArchivePath.lexically_normal()) !=
      fspath_to_path_t(archivePath.lexically_normal())) {
    return std::nullopt;
  }
  return archive_file::entryRangeForFolder(chart.Meta.Folder);
}

bool readArchiveBatchEntries(
    const ArchiveAssetBatch &batch,
    const std::optional<archive_file::EntryRange> &range,
    std::vector<archive_file::FileData> &files, std::string *errorMessage) {
  if (range.has_value()) {
    std::string rangeError;
    if (archive_file::readArchiveEntriesInRange(
            batch.archivePath, batch.innerPaths, *range, files, &rangeError) &&
        files.size() == batch.innerPaths.size()) {
      return true;
    }
    files.clear();
  }
  return archive_file::readArchiveEntries(batch.archivePath, batch.innerPaths,
                                          files, errorMessage);
}
} // namespace

Jukebox::Jukebox(Stopwatch *stopwatch)
    : audio(stopwatch), stopwatch(stopwatch) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}

Jukebox::~Jukebox() {
  isPlaying = false;
  wakeScheduler();
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  audio.unloadSounds();
  clearVisualResources();
}
void Jukebox::render() {
  if (!visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  syncVisualClockToAudio();
  const int bga = currentBga.load(std::memory_order_relaxed);
  if (bga != -1) {
    bool rendered = false;
    {
      std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
      auto videoIt = videoPlayerTable.find(bga);
      if (videoIt != videoPlayerTable.end()) {
        auto *videoPlayer = videoIt->second;
        videoPlayer->update();
        const auto rect = calculateBgaRect(videoPlayer->getFrameWidth(),
                                           videoPlayer->getFrameHeight());
        videoPlayer->viewX = rect.x;
        videoPlayer->viewY = rect.y;
        videoPlayer->viewWidth = rect.width;
        videoPlayer->viewHeight = rect.height;
        videoPlayer->viewId = rendering::bga_view;
        videoPlayer->render();
        rendered = true;
      }
    }
    if (!rendered) {
      std::lock_guard<std::mutex> lock(imageTableMutex);
      auto imageIt = imageTable.find(bga);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_view);
      }
    }
  }
  const int bmpLayer = currentBmpLayer.load(std::memory_order_relaxed);
  if (bmpLayer != -1) {
    bool rendered = false;
    {
      std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
      auto videoIt = videoPlayerTable.find(bmpLayer);
      if (videoIt != videoPlayerTable.end()) {
        auto *videoPlayer = videoIt->second;
        videoPlayer->update();
        const auto rect = calculateBgaRect(videoPlayer->getFrameWidth(),
                                           videoPlayer->getFrameHeight());
        videoPlayer->viewX = rect.x;
        videoPlayer->viewY = rect.y;
        videoPlayer->viewWidth = rect.width;
        videoPlayer->viewHeight = rect.height;
        videoPlayer->viewId = rendering::bga_layer_view;
        videoPlayer->render();
        rendered = true;
      }
    }
    if (!rendered) {
      std::lock_guard<std::mutex> lock(imageTableMutex);
      auto imageIt = imageTable.find(bmpLayer);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_layer_view);
      }
    }
  }
}

bool Jukebox::hasActiveVisuals() const {
  return visualsEnabled.load(std::memory_order_relaxed) &&
         (currentBga.load(std::memory_order_relaxed) != -1 ||
          currentBmpLayer.load(std::memory_order_relaxed) != -1);
}

void Jukebox::setVisualsEnabled(bool enabled) {
  visualsEnabled.store(enabled, std::memory_order_relaxed);
  wakeScheduler();
  if (enabled) {
    return;
  }

  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
}

bool Jukebox::getVisualsEnabled() const {
  return visualsEnabled.load(std::memory_order_relaxed);
}

void Jukebox::setBgaOffsetMs(int offsetMs) {
  const int previous = bgaOffsetMs.exchange(offsetMs, std::memory_order_relaxed);
  if (previous != offsetMs) {
    wakeScheduler();
  }
}

void Jukebox::setBgaDisplayMode(AppSettings::BgaDisplayMode mode) {
  bgaDisplayMode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

bool Jukebox::loadMaterializedVideoPath(
    int id, const std::filesystem::path &materializedPath,
    const std::filesystem::path &displayPath, std::atomic_bool &isCancelled) {
  auto videoPlayer = new VideoPlayer(stopwatch);
  const path_t playablePath = fspath_to_path_t(materializedPath);

  if (videoPlayer->loadVideo(path_t_to_utf8(playablePath), isCancelled)) {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    videoPlayerTable[id] = videoPlayer;

    SDL_Log("video width: %f, video height: %f", videoPlayer->viewWidth,
            videoPlayer->viewHeight);
    SDL_Log("Loaded video to id: %d", id);
    return true;
  }

  SDL_Log("Failed to load video: %s",
          path_t_to_utf8(fspath_to_path_t(displayPath)).c_str());
  delete videoPlayer;
  return false;
}

bool Jukebox::loadVideoPath(int id, const std::filesystem::path &path,
                            std::atomic_bool &isCancelled) {
  std::string materializeError;
  const auto playablePath =
      archive_file::materializeFile(path, &materializeError);
  if (!playablePath.has_value()) {
    SDL_Log("Failed to materialize video: %s", materializeError.c_str());
    return false;
  }
  return loadMaterializedVideoPath(id, *playablePath, path, isCancelled);
}

bool Jukebox::loadImageBytes(int id, const std::filesystem::path &path,
                             const std::vector<unsigned char> &bytes) {
  const path_t displayPath = fspath_to_path_t(path);
  const std::string utf8Path = path_t_to_utf8(displayPath);
  int width, height, channels;
  unsigned char *data = decodeImageBytes(bytes, &width, &height, &channels, 4);
  if (!data) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s", utf8Path.c_str());
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    imageTable[id] = {
        .texture = bgfx::createTexture2D(
            width, height, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE, bgfx::copy(data, width * height * 4)),
        .width = width,
        .height = height,
        .channels = channels,
    };
  }
  stbi_image_free(data);
  return true;
}

bool Jukebox::loadImagePath(int id, const std::filesystem::path &path) {
  const path_t displayPath = fspath_to_path_t(path);
  const std::string utf8Path = path_t_to_utf8(displayPath);
  int width, height, channels;
  unsigned char *data = loadImageFile(path, &width, &height, &channels, 4);
  if (!data) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s", utf8Path.c_str());
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    imageTable[id] = {
        .texture = bgfx::createTexture2D(
            width, height, false, 1, bgfx::TextureFormat::RGBA8,
            BGFX_TEXTURE_NONE, bgfx::copy(data, width * height * 4)),
        .width = width,
        .height = height,
        .channels = channels,
    };
  }
  stbi_image_free(data);
  return true;
}

Jukebox::BgaRect Jukebox::calculateBgaRect(int sourceWidth,
                                           int sourceHeight) const {
  const float targetWidth = static_cast<float>(rendering::window_width);
  const float targetHeight = static_cast<float>(rendering::window_height);
  if (targetWidth <= 0.0f || targetHeight <= 0.0f) {
    return {};
  }

  const auto mode = static_cast<AppSettings::BgaDisplayMode>(
      bgaDisplayMode.load(std::memory_order_relaxed));
  if (mode == AppSettings::BgaDisplayMode::Stretch || sourceWidth <= 0 ||
      sourceHeight <= 0) {
    return {0.0f, 0.0f, targetWidth, targetHeight};
  }

  const float scaleX = targetWidth / static_cast<float>(sourceWidth);
  const float scaleY = targetHeight / static_cast<float>(sourceHeight);
  const float scale = mode == AppSettings::BgaDisplayMode::Fill
                          ? std::max(scaleX, scaleY)
                          : std::min(scaleX, scaleY);
  const float width = static_cast<float>(sourceWidth) * scale;
  const float height = static_cast<float>(sourceHeight) * scale;
  return {(targetWidth - width) * 0.5f, (targetHeight - height) * 0.5f, width,
          height};
}

void Jukebox::loadSounds(bms_parser::Chart &chart,
                         std::atomic_bool &isCancelled) {
  if (loadArchivedSounds(chart, isCancelled)) {
    return;
  }

  std::mutex wavTableLock;
  std::mutex loadedPathsLock;
  std::unordered_set<path_t> loadedPaths;
  const auto audioExtensionViews =
      toExtensionViews(audioExtensions, std::size(audioExtensions));

  wavTableAbs.clear();

  parallel_for(chart.WavTable.size(), [&](int start, int end) {
    auto wav = std::next(chart.WavTable.begin(), start);
    for (int i = start; i < end; i++, ++wav) {
      if (isCancelled)
        return;
      bool found = false;
      std::filesystem::path basePath = chart.Meta.Folder / wav->second;
      const auto resolvedPath =
          archive_file::findFileWithExtensions(basePath, audioExtensionViews);
      if (resolvedPath.has_value()) {
        const std::filesystem::path path = *resolvedPath;

        const path_t soundPath = fspath_to_path_t(path);
        bool needsLoad = true;
        {
          std::lock_guard<std::mutex> lock(loadedPathsLock);
          needsLoad = !loadedPaths.contains(soundPath);
        }

        if (needsLoad && !audio.loadSound(soundPath, isCancelled)) {
          continue;
        }

        if (needsLoad) {
          std::lock_guard<std::mutex> lock(loadedPathsLock);
          loadedPaths.insert(soundPath);
          SDL_Log("Loaded sound %d: %s", wav->first,
                  path_t_to_utf8(soundPath).c_str());
        }

        {
          std::lock_guard<std::mutex> lock(wavTableLock);
          auto idx = wav->first;
          wavTableAbs[idx] = soundPath;
        }

        found = true;
      }
      if (!found) {
        SDL_Log("Failed to load sound for all extensions: %s",
                basePath.c_str());
      }
    }
  });
}

bool Jukebox::loadArchivedSounds(bms_parser::Chart &chart,
                                 std::atomic_bool &isCancelled) {
  bool hasVirtualAssetBase = false;
  for (const auto &wav : chart.WavTable) {
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / wav.second,
                                       archivePath, innerPath)) {
      hasVirtualAssetBase = true;
      break;
    }
  }
  if (!hasVirtualAssetBase) {
    return false;
  }

  const auto audioExtensionViews =
      toExtensionViews(audioExtensions, std::size(audioExtensions));
  std::unordered_map<path_t, ArchiveAssetBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::vector<std::pair<int, path_t>> regularLoads;
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;

  wavTableAbs.clear();
  for (const auto &wav : chart.WavTable) {
    if (isCancelled) {
      return true;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / wav.second;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    std::optional<std::filesystem::path> resolvedPath;
    if (archive_file::splitVirtualPath(basePath, archivePath, innerPath)) {
      if (ArchiveEntryLookup *lookup = getArchiveLookup(archivePath, lookups)) {
        if (const auto resolvedInner =
                lookup->find(innerPath, audioExtensionViews)) {
          resolvedPath =
              archive_file::makeVirtualPath(archivePath, *resolvedInner);
        }
      }
    } else {
      resolvedPath =
          archive_file::findFileWithExtensions(basePath, audioExtensionViews);
    }
    if (!resolvedPath.has_value()) {
      SDL_Log("Failed to load sound for all extensions: %s",
              path_t_to_utf8(fspath_to_path_t(basePath)).c_str());
      continue;
    }

    const std::filesystem::path resolvedAssetPath = *resolvedPath;
    const path_t soundPath = fspath_to_path_t(resolvedAssetPath);
    if (!addArchiveAssetTarget(archiveBatches, archiveBatchOrder,
                               resolvedAssetPath, wav.first)) {
      regularLoads.emplace_back(wav.first, soundPath);
    }
  }

  for (const auto &[wavId, soundPath] : regularLoads) {
    if (isCancelled) {
      return true;
    }
    if (audio.loadSound(soundPath, isCancelled)) {
      wavTableAbs[wavId] = soundPath;
      SDL_Log("Loaded sound %d: %s", wavId,
              path_t_to_utf8(soundPath).c_str());
    }
  }

  for (const auto &archiveKey : archiveBatchOrder) {
    if (isCancelled) {
      return true;
    }
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      continue;
    }

    const ArchiveAssetBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage)) {
      SDL_Log("Failed to read sounds from archive %s: %s",
              path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
              errorMessage.c_str());
      continue;
    }

    std::unordered_set<path_t> loadedPaths;
    for (const auto &file : files) {
      if (isCancelled) {
        return true;
      }
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t soundPath = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.idsByPath.find(soundPath);
      if (idsIt == batch.idsByPath.end()) {
        continue;
      }

      if (!audio.loadSoundFromMemory(soundPath, file.bytes, isCancelled)) {
        continue;
      }
      loadedPaths.insert(soundPath);
      for (const int wavId : idsIt->second) {
        wavTableAbs[wavId] = soundPath;
        SDL_Log("Loaded sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
    }

    for (const auto &[soundPath, ids] : batch.idsByPath) {
      if (loadedPaths.contains(soundPath)) {
        continue;
      }
      for (const int wavId : ids) {
        SDL_Log("Failed to load sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
    }
  }
  return true;
}
void Jukebox::loadBMPs(bms_parser::Chart &chart,
                       std::atomic_bool &isCancelled) {
  if (loadArchivedBMPs(chart, isCancelled)) {
    return;
  }

  const auto imageExtensionViews =
      toExtensionViews(imageExtensions, std::size(imageExtensions));
  parallel_for(chart.BmpTable.size(), [&](int start, int end) {
    auto bmp = std::next(chart.BmpTable.begin(), start);
    for (int i = start; i < end; i++, ++bmp) {
      if (isCancelled)
        return;
      bool found = false;
      std::filesystem::path basePath = chart.Meta.Folder / bmp->second;
      std::filesystem::path path;

      if (auto resolvedVideoPath = findWithReplacedExtensions(
              basePath, videoExtensions, std::size(videoExtensions))) {
        if (isCancelled)
          return;
        path = *resolvedVideoPath;
        // calculate hash of base path
        // auto pathHash = bms_parser::md5(basePath.string());
        // auto fileName = pathHash + "-" + std::to_string(bmp->first) + ".mp4";
        // auto transcodedPath =
        //     (Utils::GetDocumentsPath("temp") / fileName).string();
        // if (!std::filesystem::exists(transcodedPath)) {
        //   // mkdir
        //   std::filesystem::create_directories(Utils::GetDocumentsPath("temp"));
        //   int result = transcode(path.string().c_str(),
        //   transcodedPath.c_str(),
        //                          &isCancelled);
        //   if (isCancelled) {
        //     // delete transcoded file
        //     std::filesystem::remove(transcodedPath);
        //     return;
        //   }
        //   if (result != 0) {
        //     SDL_Log("Failed to transcode video: %ls", path.c_str());
        //     continue;
        //   }
        // }
        found = loadVideoPath(bmp->first, path, isCancelled);
      }

      // if not found, fall back to image loading
      if (!found) {
        for (const auto &ext : imageExtensionViews) {
          if (isCancelled)
            return;
          path = basePath;
          path.replace_extension(std::string(ext));
          const auto resolvedImagePath =
              archive_file::findFileWithExtensions(path, {});
          if (!resolvedImagePath.has_value()) {
            continue;
          }
          path = *resolvedImagePath;
          if (loadImagePath(bmp->first, path)) {
            break;
          }
        }
      }
    }
  });
}

bool Jukebox::loadArchivedBMPs(bms_parser::Chart &chart,
                               std::atomic_bool &isCancelled) {
  bool hasVirtualAssetBase = false;
  for (const auto &bmp : chart.BmpTable) {
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / bmp.second,
                                       archivePath, innerPath)) {
      hasVirtualAssetBase = true;
      break;
    }
  }
  if (!hasVirtualAssetBase) {
    return false;
  }

  const std::vector<std::string_view> noExtensions;
  const auto imageExtensionViews =
      toExtensionViews(imageExtensions, std::size(imageExtensions));
  std::unordered_map<path_t, ArchiveAssetBatch> imageBatches;
  std::unordered_map<path_t, ArchiveAssetBatch> videoBatches;
  std::vector<path_t> imageBatchOrder;
  std::vector<path_t> videoBatchOrder;
  std::vector<std::pair<int, std::filesystem::path>> regularImages;
  std::vector<std::pair<int, std::filesystem::path>> regularVideos;
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;

  for (const auto &bmp : chart.BmpTable) {
    if (isCancelled) {
      return true;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / bmp.second;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    const bool baseIsVirtual =
        archive_file::splitVirtualPath(basePath, archivePath, innerPath);
    std::optional<std::filesystem::path> resolvedVideoPath;
    if (baseIsVirtual) {
      if (ArchiveEntryLookup *lookup = getArchiveLookup(archivePath, lookups)) {
        if (const auto resolvedInner = lookup->findReplacedExtensions(
                innerPath, videoExtensions, std::size(videoExtensions))) {
          resolvedVideoPath =
              archive_file::makeVirtualPath(archivePath, *resolvedInner);
        }
      }
    } else {
      resolvedVideoPath = findWithReplacedExtensions(
          basePath, videoExtensions, std::size(videoExtensions));
    }
    if (resolvedVideoPath.has_value()) {
      if (!addArchiveAssetTarget(videoBatches, videoBatchOrder,
                                 *resolvedVideoPath, bmp.first)) {
        regularVideos.emplace_back(bmp.first, *resolvedVideoPath);
      }
      continue;
    }

    bool found = false;
    for (const auto &ext : imageExtensionViews) {
      if (isCancelled) {
        return true;
      }
      std::filesystem::path path = basePath;
      path.replace_extension(std::string(ext));
      std::optional<std::filesystem::path> resolvedImagePath;
      if (baseIsVirtual) {
        if (ArchiveEntryLookup *lookup = getArchiveLookup(archivePath, lookups)) {
          std::filesystem::path candidateInner = innerPath;
          candidateInner.replace_extension(std::string(ext));
          if (const auto resolvedInner =
                  lookup->find(candidateInner, noExtensions)) {
            resolvedImagePath =
                archive_file::makeVirtualPath(archivePath, *resolvedInner);
          }
        }
      } else {
        resolvedImagePath = archive_file::findFileWithExtensions(path, {});
      }
      if (!resolvedImagePath.has_value()) {
        continue;
      }
      if (!addArchiveAssetTarget(imageBatches, imageBatchOrder,
                                 *resolvedImagePath, bmp.first)) {
        regularImages.emplace_back(bmp.first, *resolvedImagePath);
      }
      found = true;
      break;
    }
    if (!found) {
      SDL_Log("Failed to load image or video for all extensions: %s",
              path_t_to_utf8(fspath_to_path_t(basePath)).c_str());
    }
  }

  for (const auto &[id, path] : regularVideos) {
    if (isCancelled) {
      return true;
    }
    loadVideoPath(id, path, isCancelled);
  }
  for (const auto &[id, path] : regularImages) {
    if (isCancelled) {
      return true;
    }
    loadImagePath(id, path);
  }

  for (const auto &archiveKey : videoBatchOrder) {
    if (isCancelled) {
      return true;
    }
    const auto batchIt = videoBatches.find(archiveKey);
    if (batchIt == videoBatches.end()) {
      continue;
    }

    const ArchiveAssetBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage)) {
      SDL_Log("Failed to read videos from archive %s: %s",
              path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
              errorMessage.c_str());
      continue;
    }

    for (const auto &file : files) {
      if (isCancelled) {
        return true;
      }
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t pathKey = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.idsByPath.find(pathKey);
      if (idsIt == batch.idsByPath.end()) {
        continue;
      }

      std::string materializeError;
      const auto playablePath = archive_file::materializeFileBytes(
          virtualPath, file.bytes, &materializeError);
      if (!playablePath.has_value()) {
        SDL_Log("Failed to materialize video: %s",
                materializeError.c_str());
        continue;
      }
      for (const int id : idsIt->second) {
        loadMaterializedVideoPath(id, *playablePath, virtualPath,
                                  isCancelled);
      }
    }
  }

  for (const auto &archiveKey : imageBatchOrder) {
    if (isCancelled) {
      return true;
    }
    const auto batchIt = imageBatches.find(archiveKey);
    if (batchIt == imageBatches.end()) {
      continue;
    }

    const ArchiveAssetBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage)) {
      SDL_Log("Failed to read images from archive %s: %s",
              path_t_to_utf8(fspath_to_path_t(batch.archivePath)).c_str(),
              errorMessage.c_str());
      continue;
    }

    for (const auto &file : files) {
      if (isCancelled) {
        return true;
      }
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t pathKey = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.idsByPath.find(pathKey);
      if (idsIt == batch.idsByPath.end()) {
        continue;
      }
      for (const int id : idsIt->second) {
        loadImageBytes(id, virtualPath, file.bytes);
      }
    }
  }
  return true;
}

void Jukebox::clearVisualResources() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      delete videoPlayer.second;
    }
    videoPlayerTable.clear();
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    for (auto &image : imageTable) {
      bgfx::destroy(image.second.texture);
    }
    imageTable.clear();
  }
}

void Jukebox::scheduleVisuals(bms_parser::Chart &chart,
                              std::atomic_bool &isCancelled) {
  bmpCursor = 0;
  bmpLayerCursor = 0;
  bmpList.clear();
  bmpLayerList.clear();
  for (auto &measure : chart.Measures) {
    if (isCancelled)
      return;
    for (auto &timeline : measure->TimeLines) {
      if (isCancelled)
        return;
      if (timeline->BgaBase != -1) {
        bmpList.emplace_back(timeline->Timing, timeline->BgaBase);
      }
      if (timeline->BgaLayer != -1) {
        bmpLayerList.emplace_back(timeline->Timing, timeline->BgaLayer);
      }
    }
  }
}

void Jukebox::loadVisuals(bms_parser::Chart &chart,
                          std::atomic_bool &isCancelled) {
  isPlaying = false;
  if (playThread.joinable()) {
    playThread.join();
  }
  clearVisualResources();
  scheduleVisuals(chart, isCancelled);
  if (isCancelled || !visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  loadBMPs(chart, isCancelled);
}

void Jukebox::unloadVisuals() { clearVisualResources(); }

void Jukebox::loadChart(bms_parser::Chart &chart, bool scheduleNotes,
                        std::atomic_bool &isCancelled) {
  isPlaying = false;
  if (playThread.joinable()) {
    SDL_Log("Joining playThread");
    playThread.join();
  }

  audio.stopSounds();
  audio.unloadSounds();

  clearVisualResources();
  if (isCancelled)
    return;
  SDL_Log("Loading sounds");
  std::thread loadSoundThread(
      [this, &chart, &isCancelled] { loadSounds(chart, isCancelled); });
  if (visualsEnabled.load(std::memory_order_relaxed)) {
    SDL_Log("Loading videos");
    loadBMPs(chart, isCancelled);
  }
  loadSoundThread.join();

  if (isCancelled)
    return;
  schedule(chart, scheduleNotes, isCancelled);
  SDL_Log("Chart loaded");
}

void Jukebox::schedule(bms_parser::Chart &chart, bool scheduleNotes,
                       std::atomic_bool &isCancelled,
                       std::optional<long long> noteScheduleCutoffMicros) {
  audioCursor = 0;
  audioList.clear();
  scheduleVisuals(chart, isCancelled);
  for (auto &measure : chart.Measures) {
    if (isCancelled)
      return;
    for (auto &timeline : measure->TimeLines) {
      if (isCancelled)
        return;
      std::vector<std::pair<long long, int>> notes;
      const bool includeTimelineNotes =
          scheduleNotes ||
          (noteScheduleCutoffMicros.has_value() &&
           timeline->Timing < *noteScheduleCutoffMicros);
      if (includeTimelineNotes) {
        for (auto &note : timeline->Notes) {
          if (isCancelled)
            return;
          if (note == nullptr)
            continue;
          if (note->Wav == bms_parser::Parser::NoWav)
            continue;
          if (!wavTableAbs.contains(note->Wav))
            continue;
          notes.emplace_back(timeline->Timing, note->Wav);
        }
      }
      for (auto &bgNote : timeline->BackgroundNotes) {
        if (isCancelled)
          return;
        if (bgNote->Wav == bms_parser::Parser::NoWav)
          continue;
        if (!wavTableAbs.contains(bgNote->Wav))
          continue;
        notes.emplace_back(timeline->Timing, bgNote->Wav);
      }
      std::sort(notes.begin(), notes.end());
      for (auto &note : notes) {
        if (isCancelled)
          return;
        audioList.push_back(note);
      }
    }
  }
  std::sort(audioList.begin(), audioList.end());
}
void Jukebox::playKeySound(int wav) {
  if (!isPlaying) {
    return;
  }
  if (const auto it = wavTableAbs.find(wav); it != wavTableAbs.end()) {
    audio.playSound(it->second.c_str());
  }
}

void Jukebox::scheduleAudioFromCursor() {
  while (audioCursor < audioList.size()) {
    const auto &target = audioList[audioCursor];
    if (const auto it = wavTableAbs.find(target.second);
        it != wavTableAbs.end()) {
      audio.scheduleSound(it->second, target.first);
    }
    audioCursor++;
  }
}

void Jukebox::playOverlappingAudioAt(long long micro) {
  std::vector<std::pair<path_t, long long>> overlapping;
  const auto seekIt =
      std::lower_bound(audioList.begin(), audioList.end(), micro,
                       [](const std::pair<long long, int> &entry,
                          long long targetMicros) {
                         return entry.first < targetMicros;
                       });
  for (auto it = std::make_reverse_iterator(seekIt); it != audioList.rend();
       ++it) {
    const auto &target = *it;
    const auto wavIt = wavTableAbs.find(target.second);
    if (wavIt == wavTableAbs.end()) {
      continue;
    }
    const long long elapsed = micro - target.first;
    const auto duration = audio.getSoundDurationMicros(wavIt->second);
    if (!duration.has_value() || elapsed >= *duration) {
      continue;
    }
    overlapping.emplace_back(wavIt->second, elapsed);
  }

  for (auto it = overlapping.rbegin(); it != overlapping.rend(); ++it) {
    audio.playSound(it->first, it->second);
  }
}

void Jukebox::wakeScheduler() { schedulerWakeCv.notify_all(); }

void Jukebox::syncVisualClockToAudio() {
  if (isPlaying.load(std::memory_order_relaxed) && stopwatch->isRunning()) {
    stopwatch->seek(getBgaTimelineMicros(audio.getTimeMicros()));
  }
}

long long Jukebox::getBgaOffsetMicros() const {
  return static_cast<long long>(bgaOffsetMs.load(std::memory_order_relaxed)) *
         1000LL;
}

long long Jukebox::getBgaTimelineMicros(long long rawSongMicros) const {
  return rawSongMicros + getBgaOffsetMicros();
}

long long
Jukebox::getRawSongMicrosForBgaTarget(long long bgaTargetMicros) const {
  return bgaTargetMicros - getBgaOffsetMicros();
}

bool Jukebox::activateVisual(int visualId, bgfx::ViewId viewId) {
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    auto videoIt = videoPlayerTable.find(visualId);
    if (videoIt != videoPlayerTable.end()) {
      auto *videoPlayer = videoIt->second;
      videoPlayer->seek(0);
      videoPlayer->play();
      videoPlayer->viewWidth = rendering::window_width;
      videoPlayer->viewHeight = rendering::window_height;
      videoPlayer->viewId = viewId;
      return true;
    }
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    if (imageTable.find(visualId) != imageTable.end()) {
      return true;
    }
  }
  return false;
}

void Jukebox::renderVisualsAt(long long micro) {
  if (!visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard<std::mutex> lock(seekLock);
  stopwatch->seek(micro);
  while (bmpCursor < bmpList.size()) {
    const auto &target = bmpList[bmpCursor];
    if (micro < target.first) {
      break;
    }
    if (activateVisual(target.second, rendering::bga_view)) {
      currentBga.store(target.second, std::memory_order_relaxed);
    }
    bmpCursor++;
  }
  while (bmpLayerCursor < bmpLayerList.size()) {
    const auto &target = bmpLayerList[bmpLayerCursor];
    if (micro < target.first) {
      break;
    }
    if (activateVisual(target.second, rendering::bga_layer_view)) {
      currentBmpLayer.store(target.second, std::memory_order_relaxed);
    }
    bmpLayerCursor++;
  }
  render();
}

void Jukebox::play() {
  std::lock_guard<std::mutex> lock(playThreadLock);
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  isPlaying = true;
  stopwatch->reset();
  audio.seekClock(0);
  {
    std::lock_guard<std::mutex> lock(seekLock);
    audioCursor = 0;
    scheduleAudioFromCursor();
  }
  audio.startDevice();
  stopwatch->start();
  wakeScheduler();
  SDL_Log("Jukebox visual scheduler is event-driven");

  playThread = std::thread([this] {
#ifdef _WIN32
    // Set thread priority using MMCS for audio playback
    HANDLE taskHandle = nullptr;
    DWORD taskIndex = 0;
    taskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (taskHandle) {
      // Set thread priority to high
      AvSetMmThreadPriority(taskHandle, AVRT_PRIORITY_CRITICAL);
    }
    timeBeginPeriod(1);
#endif
    using Clock = std::chrono::steady_clock;
    auto prevTimestamp = Clock::now();
    while (isPlaying) {
      if (!stopwatch->isRunning()) {
        std::unique_lock<std::mutex> waitLock(schedulerWaitMutex);
        schedulerWakeCv.wait_for(
            waitLock,
            std::chrono::microseconds(kSchedulerMaxIdleSleepMicros));
        prevTimestamp = Clock::now();
        continue;
      }

      long long nextWakeMicros = std::numeric_limits<long long>::max();
      {
        // Keep scheduling state consistent with seek/reset.
        std::lock_guard<std::mutex> lock(seekLock);
        const long long positionMicro = audio.getTimeMicros();
        const long long bgaPositionMicro = getBgaTimelineMicros(positionMicro);
        stopwatch->seek(bgaPositionMicro);
        auto scheduleNextWake = [&](long long targetMicros) {
          nextWakeMicros =
              std::min(nextWakeMicros, std::max(targetMicros, positionMicro));
        };

        if (onTickCb) {
          onTickCb(positionMicro);
          scheduleNextWake(positionMicro + kSchedulerTickMicros);
        }

        while (bmpCursor < bmpList.size()) {
          auto &target = bmpList[bmpCursor];
          if (bgaPositionMicro < target.first) {
            break;
          }
          if (!visualsEnabled.load(std::memory_order_relaxed)) {
            bmpCursor++;
            continue;
          }
          bool activated = false;
          {
            std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
            auto videoIt = videoPlayerTable.find(target.second);
            if (videoIt != videoPlayerTable.end()) {
              auto *videoPlayer = videoIt->second;
              videoPlayer->seek(0);
              videoPlayer->play();
              videoPlayer->viewWidth = rendering::window_width;
              videoPlayer->viewHeight = rendering::window_height;
              videoPlayer->viewId = rendering::bga_view;
              activated = true;
            }
          }
          if (!activated) {
            std::lock_guard<std::mutex> lock(imageTableMutex);
            if (imageTable.find(target.second) != imageTable.end()) {
              activated = true;
            }
          }
          if (activated) {
            currentBga.store(target.second, std::memory_order_relaxed);
          }
          bmpCursor++;
        }
        if (bmpCursor < bmpList.size()) {
          scheduleNextWake(
              getRawSongMicrosForBgaTarget(bmpList[bmpCursor].first));
        }
        while (bmpLayerCursor < bmpLayerList.size()) {
          auto &target = bmpLayerList[bmpLayerCursor];
          if (bgaPositionMicro < target.first) {
            break;
          }
          if (!visualsEnabled.load(std::memory_order_relaxed)) {
            bmpLayerCursor++;
            continue;
          }
          bool activated = false;
          {
            std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
            auto videoIt = videoPlayerTable.find(target.second);
            if (videoIt != videoPlayerTable.end()) {
              auto *videoPlayer = videoIt->second;
              videoPlayer->seek(0);
              videoPlayer->play();
              videoPlayer->viewWidth = rendering::window_width;
              videoPlayer->viewHeight = rendering::window_height;
              videoPlayer->viewId = rendering::bga_layer_view;
              activated = true;
            }
          }
          if (!activated) {
            std::lock_guard<std::mutex> lock(imageTableMutex);
            if (imageTable.find(target.second) != imageTable.end()) {
              activated = true;
            }
          }
          if (activated) {
            currentBmpLayer.store(target.second, std::memory_order_relaxed);
          }
          bmpLayerCursor++;
        }
        if (bmpLayerCursor < bmpLayerList.size()) {
          scheduleNextWake(getRawSongMicrosForBgaTarget(
              bmpLayerList[bmpLayerCursor].first));
        }
      }

      auto currentTimestamp = Clock::now();
      size_t idx =
          performanceAnalytics.loopDeltaIndex.load(std::memory_order_relaxed);
      const auto deltaMicros =
          std::chrono::duration_cast<std::chrono::microseconds>(
              currentTimestamp - prevTimestamp)
              .count();
      performanceAnalytics.loopDeltaTimes[idx].store(
          static_cast<uint32_t>(deltaMicros), std::memory_order_relaxed);
      size_t newIdx = (idx + 1) % Jukebox::PerformanceAnalytics::BUFFER_SIZE;
      performanceAnalytics.loopDeltaIndex.store(newIdx,
                                                std::memory_order_relaxed);
      prevTimestamp = currentTimestamp;

      long long sleepMicros = kSchedulerMaxIdleSleepMicros;
      if (nextWakeMicros != std::numeric_limits<long long>::max()) {
        const long long currentSongMicros = audio.getTimeMicros();
        const long long untilNextMicros = nextWakeMicros - currentSongMicros;
        if (untilNextMicros <= 0) {
          std::this_thread::yield();
          continue;
        }
        sleepMicros = std::min(kSchedulerMaxIdleSleepMicros, untilNextMicros);
      }
      std::unique_lock<std::mutex> waitLock(schedulerWaitMutex);
      schedulerWakeCv.wait_for(waitLock,
                               std::chrono::microseconds(sleepMicros));
    }
#ifdef _WIN32
    // Clean up MMCS handle
    if (taskHandle) {
      AvRevertMmThreadCharacteristics(taskHandle);
    }
    timeEndPeriod(1);
#endif
  });
}
void Jukebox::renderImage(ImageData &image, int viewId) {

  if (!bgfx::isValid(image.texture)) {
    return;
  }
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::allocTransientVertexBuffer(&tvb, 4,
                                   rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;
  const auto rect = calculateBgaRect(image.width, image.height);
  vertex[0].x = rect.x;
  vertex[0].y = rect.y + rect.height;
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;
  vertex[1].x = rect.x + rect.width;
  vertex[1].y = rect.y + rect.height;
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;
  vertex[2].x = rect.x;
  vertex[2].y = rect.y;
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;
  vertex[3].x = rect.x + rect.width;
  vertex[3].y = rect.y;
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texColor, image.texture);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kBgaProgram =
      rendering::ShaderManager::getInstance().getProgram("vs_text.bin",
                                                         "fs_text.bin");
  static const bgfx::ProgramHandle kBgaLayerProgram =
      rendering::ShaderManager::getInstance().getProgram("vs_text.bin",
                                                         "fs_bgalayer.bin");
  bgfx::submit(viewId,
               viewId == rendering::bga_view ? kBgaProgram : kBgaLayerProgram);
}

long long Jukebox::getTimeMicros() {
  if (isPlaying.load(std::memory_order_relaxed)) {
    return audio.getTimeMicros();
  }
  return stopwatch->elapsedMicros();
}
void Jukebox::pause() {
  SDL_Log("Pausing");
  audio.seekClock(audio.getTimeMicros());
  stopwatch->pause();
  wakeScheduler();
}
void Jukebox::resume() {
  audio.seekClock(audio.getTimeMicros());
  stopwatch->resume();
  wakeScheduler();
}
bool Jukebox::isPaused() { return !stopwatch->isRunning(); }
void Jukebox::stop() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  isPlaying = false;
  wakeScheduler();
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
}
void Jukebox::seek(long long micro) {
  std::lock_guard<std::mutex> lock(seekLock);
  const long long bgaTimelineMicro = getBgaTimelineMicros(micro);
  stopwatch->seek(bgaTimelineMicro);
  audio.stopSounds();
  audio.seekClock(micro);
  // move cursors to micro
  audioCursor = 0;
  bmpCursor = 0;
  bmpLayerCursor = 0;
  while (audioCursor < audioList.size() &&
         audioList[audioCursor].first < micro) {
    audioCursor++;
  }
  scheduleAudioFromCursor();
  playOverlappingAudioAt(micro);
  if (isPlaying) {
    audio.startDevice();
  }
  wakeScheduler();
  while (bmpCursor < bmpList.size() &&
         bmpList[bmpCursor].first < bgaTimelineMicro) {
    bmpCursor++;
  }
  while (bmpLayerCursor < bmpLayerList.size() &&
         bmpLayerList[bmpLayerCursor].first < bgaTimelineMicro) {
    bmpLayerCursor++;
  }
}

double Jukebox::getAvgDeltaTime() {
  size_t currentWriteIndex =
      performanceAnalytics.loopDeltaIndex.load(std::memory_order_acquire);
  while (performanceAnalytics.cursor != currentWriteIndex) {
    const auto loopRunTime = static_cast<double>(
        performanceAnalytics.loopDeltaTimes[performanceAnalytics.cursor].load(
            std::memory_order_relaxed));
    performanceAnalytics.statsSum += loopRunTime;
    performanceAnalytics.statsCount++;
    if (performanceAnalytics.statsCount >= 100) {
      performanceAnalytics.avgDeltaTime =
          performanceAnalytics.statsSum / performanceAnalytics.statsCount;
      performanceAnalytics.statsSum = 0;
      performanceAnalytics.statsCount = 0;
    }
    performanceAnalytics.cursor =
        (performanceAnalytics.cursor + 1) % PerformanceAnalytics::BUFFER_SIZE;
  }

  return performanceAnalytics.avgDeltaTime;
}
