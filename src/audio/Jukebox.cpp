#include "Jukebox.h"
#include "../targets.h"
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#include <unistd.h>
#endif
#include "../ArchiveFile.h"
#include "../RAII.h"
#include "../StbImageRAII.h"
#include <SDL2/SDL.h>
#include <thread>
#include "../Utils.h"
#include "../game/GameState.h"
#include "../path.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include "ChartAssetExtensions.h"
#include "JukeboxLifecycle.h"
#include "JukeboxSoundResources.h"
#include "bgfx/bgfx.h"
#include <stb_image.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
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
constexpr std::uint64_t kArchiveAssetMaxInFlightBytes =
    64ull * 1024ull * 1024ull;
constexpr int kPrepMetronomeAccentWav = -100000;
constexpr int kPrepMetronomeRegularWav = -100001;
const path_t kPrepMetronomeAccentPath = PATH("@prep_metronome_accent");
const path_t kPrepMetronomeRegularPath = PATH("@prep_metronome_regular");
constexpr int kPrepMetronomeSampleRate = 48000;
constexpr int kPrepMetronomeChannels = 2;
constexpr double kPrepMetronomeClickSeconds = 0.045;
constexpr double kPrepMetronomePi = 3.14159265358979323846;

bool scheduledAudioEventLess(const ScheduledAudioEvent &left,
                             const ScheduledAudioEvent &right) {
  if (left.timeMicros != right.timeMicros) {
    return left.timeMicros < right.timeMicros;
  }
  if (left.wav != right.wav) {
    return left.wav < right.wav;
  }
  return static_cast<std::uint8_t>(left.bus) <
         static_cast<std::uint8_t>(right.bus);
}

std::vector<short> makePrepMetronomeClick(double frequency, double amplitude) {
  const int frames = static_cast<int>(std::lround(
      static_cast<double>(kPrepMetronomeSampleRate) *
      kPrepMetronomeClickSeconds));
  std::vector<short> pcm(static_cast<size_t>(frames) *
                         kPrepMetronomeChannels);
  for (int frame = 0; frame < frames; ++frame) {
    const double t = static_cast<double>(frame) /
                     static_cast<double>(kPrepMetronomeSampleRate);
    const double envelope = std::exp(-t * 90.0);
    const double sample =
        std::sin(2.0 * kPrepMetronomePi * frequency * t) * envelope * amplitude;
    const auto value = static_cast<short>(
        std::clamp(sample, -1.0, 1.0) * static_cast<double>(INT16_MAX));
    pcm[static_cast<size_t>(frame) * 2] = value;
    pcm[static_cast<size_t>(frame) * 2 + 1] = value;
  }
  return pcm;
}

bool chartHasVirtualAssetBase(const bms_parser::Chart &chart,
                              const ChartResourceTable &wavTable,
                              const ChartResourceTable &bmpTable,
                              bool loadVisualAssets) {
  for (const auto &[id, wavPath] : wavTable) {
    (void)id;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / wavPath,
                                       archivePath, innerPath)) {
      return true;
    }
  }

  if (!loadVisualAssets) {
    return false;
  }
  for (const auto &[id, bmpPath] : bmpTable) {
    (void)id;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / bmpPath,
                                       archivePath, innerPath)) {
      return true;
    }
  }
  return false;
}

#if TARGET_OS_ANDROID
struct UniqueFd {
  explicit UniqueFd(int fd) : value(fd) {}
  ~UniqueFd() {
    if (value >= 0) {
      close(value);
    }
  }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  int value;
};

std::optional<std::filesystem::path>
normalizedAndroidAssetReference(const std::filesystem::path &assetPath) {
  if (assetPath.empty()) {
    return std::nullopt;
  }
  std::string normalized = fspath_to_utf8(assetPath);
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }

  std::filesystem::path relativePath(normalized);
  relativePath = relativePath.lexically_normal();
  if (relativePath.empty() || relativePath == "." ||
      relativePath.is_absolute() || relativePath.has_root_path()) {
    return std::nullopt;
  }
  for (const auto &part : relativePath) {
    if (part == "." || part == "..") {
      return std::nullopt;
    }
  }
  return relativePath;
}

void addAndroidAssetDirectory(
    const std::filesystem::path &chartFolder,
    const std::filesystem::path &assetPath,
    std::vector<std::filesystem::path> &directories,
    std::unordered_set<path_t> &directoryKeys) {
  const auto relativePath = normalizedAndroidAssetReference(assetPath);
  if (!relativePath.has_value()) {
    return;
  }
  std::filesystem::path directory =
      (chartFolder / *relativePath).parent_path().lexically_normal();
  if (!IsAndroidTreePath(directory)) {
    return;
  }
  const path_t key = fspath_to_path_t(directory);
  if (directoryKeys.insert(key).second) {
    directories.push_back(std::move(directory));
  }
}

void prepareAndroidChartAssetDirectoryCache(bms_parser::Chart &chart,
                                            const ChartResourceTable &wavTable,
                                            const ChartResourceTable &bmpTable,
                                            bool loadVisualAssets,
                                            std::atomic_bool &isCancelled) {
  if (!IsAndroidTreePath(chart.Meta.Folder)) {
    return;
  }

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  std::vector<std::filesystem::path> directories;
  std::unordered_set<path_t> directoryKeys;
  std::filesystem::path chartFolder = chart.Meta.Folder.lexically_normal();
  const path_t chartFolderKey = fspath_to_path_t(chartFolder);
  directoryKeys.insert(chartFolderKey);
  directories.push_back(chartFolder);

  for (const auto &[id, wavPath] : wavTable) {
    (void)id;
    if (isCancelled) {
      return;
    }
    addAndroidAssetDirectory(chartFolder, wavPath, directories,
                             directoryKeys);
  }
  if (loadVisualAssets) {
    for (const auto &[id, bmpPath] : bmpTable) {
      (void)id;
      if (isCancelled) {
        return;
      }
      addAndroidAssetDirectory(chartFolder, bmpPath, directories,
                               directoryKeys);
    }
  }

  std::string errorMessage;
  if (!ClearAndroidTreeTransientFileCache(errorMessage)) {
    SDL_Log("Failed to clear Android SAF transient file cache: %s",
            errorMessage.c_str());
  }

  std::size_t warmedDirectories = 0;
  for (const auto &directory : directories) {
    if (isCancelled) {
      return;
    }
    errorMessage.clear();
    if (!CacheAndroidTreeDirectory(directory, errorMessage)) {
      SDL_Log("Failed to cache Android SAF directory %s: %s",
              fspath_to_utf8(directory).c_str(),
              errorMessage.c_str());
      continue;
    }
    warmedDirectories++;
  }

  const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now() - start)
                             .count();
  archive_file::appendDebugLogLine(
      "Warmed Android SAF chart directories: requested=" +
      std::to_string(directories.size()) +
      " warmed=" + std::to_string(warmedDirectories) + " ms=" +
      std::to_string(elapsedMs));
}
#endif

std::vector<std::string_view> toExtensionViews(const std::string *extensions,
                                               size_t count) {
  std::vector<std::string_view> views;
  views.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    views.emplace_back(extensions[i]);
  }
  return views;
}

std::vector<std::string_view> makeAudioExtensionViews() {
  return std::vector<std::string_view>(
      asobmshow::chart_assets::kAudioExtensions.begin(),
      asobmshow::chart_assets::kAudioExtensions.end());
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
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string fdError;
    const auto fd = OpenAndroidTreeFileDescriptor(path, fdError);
    if (!fd.has_value()) {
      SDL_Log("Failed to open Android image descriptor %s: %s",
              fspath_to_utf8(path).c_str(), fdError.c_str());
      return nullptr;
    }
    FILE *file = fdopen(*fd, "rb");
    if (file == nullptr) {
      close(*fd);
      SDL_Log("Failed to create FILE for Android image descriptor: %s",
              fspath_to_utf8(path).c_str());
      return nullptr;
    }
    unsigned char *data =
        stbi_load_from_file(file, width, height, channels, requestedChannels);
    fclose(file);
    return data;
  }
#endif
  if (!archive_file::isVirtualPath(path)) {
    const std::string utf8Path = fspath_to_utf8(path);
    return stbi_load(utf8Path.c_str(), width, height, channels,
                     requestedChannels);
  }

  std::vector<unsigned char> bytes;
  std::string errorMessage;
  if (!archive_file::readFile(path, bytes, &errorMessage)) {
    SDL_Log("Failed to read archived image %s: %s",
            fspath_to_utf8(path).c_str(),
            errorMessage.c_str());
    return nullptr;
  }
  return decodeImageBytes(bytes, width, height, channels, requestedChannels);
}

bool decodedImageDimensionsAreValid(int width, int height) {
  return width > 0 && height > 0 &&
         width <= std::numeric_limits<std::uint16_t>::max() &&
         height <= std::numeric_limits<std::uint16_t>::max() &&
         static_cast<std::uint64_t>(width) *
                 static_cast<std::uint64_t>(height) <=
             std::numeric_limits<std::uint32_t>::max() / 4;
}

struct ArchiveAssetBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_map<path_t, std::vector<int>> idsByPath;
};

enum class ArchiveChartAssetKind {
  Sound,
  Video,
  Image,
};

struct ArchiveChartAssetBatch {
  std::filesystem::path archivePath;
  std::vector<std::filesystem::path> innerPaths;
  std::unordered_set<path_t> uniquePaths;
  std::unordered_map<path_t, std::vector<int>> soundIdsByPath;
  std::unordered_map<path_t, std::vector<int>> videoIdsByPath;
  std::unordered_map<path_t, std::vector<int>> imageIdsByPath;
};

struct ReusableVisualSources {
  std::vector<int> imageIds;
  size_t nextImageIndex = 0;
  std::vector<int> videoIds;
  size_t nextVideoIndex = 0;
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

bool addArchiveChartAssetTarget(
    std::unordered_map<path_t, ArchiveChartAssetBatch> &batches,
    std::vector<path_t> &batchOrder, const std::filesystem::path &path, int id,
    ArchiveChartAssetKind kind) {
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
            .emplace(archiveKey, ArchiveChartAssetBatch{
                                     .archivePath = archivePath,
                                     .innerPaths = {},
                                     .uniquePaths = {},
                                     .soundIdsByPath = {},
                                     .videoIdsByPath = {},
                                     .imageIdsByPath = {},
                                 })
            .first;
  }

  ArchiveChartAssetBatch &batch = batchIt->second;
  const path_t pathKey = fspath_to_path_t(path);
  if (batch.uniquePaths.insert(pathKey).second) {
    batch.innerPaths.push_back(innerPath);
  }

  switch (kind) {
  case ArchiveChartAssetKind::Sound:
    batch.soundIdsByPath[pathKey].push_back(id);
    break;
  case ArchiveChartAssetKind::Video:
    batch.videoIdsByPath[pathKey].push_back(id);
    break;
  case ArchiveChartAssetKind::Image:
    batch.imageIdsByPath[pathKey].push_back(id);
    break;
  }
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
    archive_file::appendDebugLogLine("Failed to index archive for assets: " +
                                     path_t_to_utf8(archiveKey) + ": " +
                                     errorMessage);
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
    std::vector<archive_file::FileData> &files, std::string *errorMessage,
    std::atomic_bool &isCancelled) {
  files.clear();
  const std::size_t workerCount =
      static_cast<std::size_t>(parallel_worker_count(batch.innerPaths.size()));
  if (workerCount > 1) {
    std::mutex filesMutex;
    std::vector<archive_file::FileData> concurrentFiles;
    concurrentFiles.reserve(batch.innerPaths.size());
    std::string concurrentError;
    auto onFile = [&](archive_file::FileData &&file) {
      if (isCancelled.load(std::memory_order_relaxed)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(filesMutex);
      concurrentFiles.push_back(std::move(file));
      return true;
    };
    const bool readOk = archive_file::readArchiveEntriesConcurrently(
        batch.archivePath, batch.innerPaths, std::move(onFile), workerCount,
        kArchiveAssetMaxInFlightBytes, &concurrentError, [&isCancelled]() {
          return !isCancelled.load(std::memory_order_relaxed);
        });
    if (readOk && concurrentFiles.size() == batch.innerPaths.size()) {
      files = std::move(concurrentFiles);
      return true;
    }
    if (readOk) {
      concurrentError = "Concurrent archive asset read returned " +
                        std::to_string(concurrentFiles.size()) + " of " +
                        std::to_string(batch.innerPaths.size()) +
                        " requested files.";
    }
    if (!concurrentError.empty() &&
        !isCancelled.load(std::memory_order_relaxed)) {
      archive_file::appendDebugLogLine(
          "Falling back to serial archive asset batch read: " +
          fspath_to_utf8(batch.archivePath) + ": " + concurrentError);
    }
  }

  auto pauseCallback = [&isCancelled]() {
    return !isCancelled.load(std::memory_order_relaxed);
  };
  if (range.has_value()) {
    std::string rangeError;
    if (archive_file::readArchiveEntriesInRange(batch.archivePath,
                                                batch.innerPaths, *range, files,
                                                &rangeError, pauseCallback) &&
        files.size() == batch.innerPaths.size()) {
      return true;
    }
    files.clear();
  }
  return archive_file::readArchiveEntries(batch.archivePath, batch.innerPaths,
                                          files, errorMessage, pauseCallback);
}

void replaceVideoPlayerLocked(
    std::unordered_map<int, std::unique_ptr<VideoPlayer>> &table, int id,
    std::unique_ptr<VideoPlayer> videoPlayer) {
  const auto existing = table.find(id);
  if (existing != table.end()) {
    existing->second = std::move(videoPlayer);
    return;
  }
  table.emplace(id, std::move(videoPlayer));
}

void destroyImageTexture(ImageData &image) {
  if (bgfx::isValid(image.texture)) {
    bgfx::destroy(image.texture);
    image.texture = BGFX_INVALID_HANDLE;
  }
}

void replaceImageLocked(std::unordered_map<int, ImageData> &table, int id,
                        ImageData image) {
  const auto existing = table.find(id);
  if (existing != table.end()) {
    destroyImageTexture(existing->second);
    existing->second = image;
    return;
  }
  table.emplace(id, image);
}

} // namespace

Jukebox::Jukebox(Stopwatch *stopwatch)
    : audio(stopwatch), stopwatch(stopwatch) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}

Jukebox::~Jukebox() {
  isPlaying = false;
  schedulerActive = false;
  wakeScheduler();
  if (playThread.joinable())
    playThread.join();
  const auto unloaded = audio.unloadSounds();
  if (!unloaded.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Jukebox::~Jukebox could not unload audio: %s",
                 unloaded.diagnostic.c_str());
  }
  clearVisualResources();
}
void Jukebox::render() {
  if (!visualsEnabled.load(std::memory_order_relaxed) ||
      visualsSuspended.load(std::memory_order_acquire)) {
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
        auto *videoPlayer = videoIt->second.get();
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
        auto *videoPlayer = videoIt->second.get();
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
         !visualsSuspended.load(std::memory_order_acquire) &&
         (currentBga.load(std::memory_order_relaxed) != -1 ||
          currentBmpLayer.load(std::memory_order_relaxed) != -1);
}

long long Jukebox::getScheduledAudioEndMicros() {
  long long endMicros = 0;
  for (const auto &event : audioList) {
    const auto wavIt = wavTableAbs.find(event.wav);
    if (wavIt == wavTableAbs.end()) {
      continue;
    }
    const auto durationMicros = audio.getSoundDurationMicros(wavIt->second);
    if (!durationMicros.has_value()) {
      continue;
    }
    endMicros = std::max(endMicros, event.timeMicros + *durationMicros);
  }
  return endMicros;
}

long long Jukebox::getScheduledVisualEndMicros() {
  std::unordered_map<int, long long> videoDurations;
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    videoDurations.reserve(videoPlayerTable.size());
    for (const auto &[visualId, videoPlayer] : videoPlayerTable) {
      if (videoPlayer != nullptr) {
        videoDurations[visualId] =
            std::max(0LL, videoPlayer->getDurationMicros());
      }
    }
  }

  std::unordered_set<int> imageIds;
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    imageIds.reserve(imageTable.size());
    for (const auto &[visualId, image] : imageTable) {
      (void)image;
      imageIds.insert(visualId);
    }
  }

  auto visualDurationFor = [&](int visualId) -> std::optional<long long> {
    auto videoIt = videoDurations.find(visualId);
    if (videoIt != videoDurations.end()) {
      return videoIt->second;
    }
    if (imageIds.find(visualId) != imageIds.end()) {
      return 0LL;
    }
    return std::nullopt;
  };

  long long endMicros = 0;
  auto extendFromEvents = [&](const std::vector<std::pair<long long, int>>
                                  &events) {
    long long activeStartMicros = -1;
    long long activeDurationMicros = 0;

    auto finishActiveVisual = [&](long long replacementMicros) {
      if (activeStartMicros < 0) {
        return;
      }
      long long activeEndMicros = activeStartMicros + activeDurationMicros;
      if (activeDurationMicros > 0 && replacementMicros >= 0) {
        activeEndMicros = std::min(activeEndMicros, replacementMicros);
      }
      endMicros = std::max(endMicros, activeEndMicros);
    };

    for (const auto &[eventMicros, visualId] : events) {
      const auto durationMicros = visualDurationFor(visualId);
      if (!durationMicros.has_value()) {
        continue;
      }
      finishActiveVisual(eventMicros);
      activeStartMicros = eventMicros;
      activeDurationMicros = *durationMicros;
    }

    finishActiveVisual(-1);
  };
  extendFromEvents(bmpList);
  extendFromEvents(bmpLayerList);
  return endMicros;
}

std::vector<std::filesystem::path> Jukebox::activeMaterializedVideoPaths()
    const {
  std::vector<std::filesystem::path> paths;
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  paths.reserve(videoMaterializedPathTable.size());
  for (const auto &[id, path] : videoMaterializedPathTable) {
    (void)id;
    if (!path.empty()) {
      paths.push_back(path);
    }
  }
  return paths;
}

void Jukebox::setVisualsEnabled(bool enabled) {
  visualsEnabled.store(enabled, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      if (videoPlayer.second != nullptr) {
        videoPlayer.second->setDecodeSuspended(
            !enabled || visualsSuspended.load(std::memory_order_acquire));
      }
    }
  }
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

void Jukebox::setVisualsSuspended(bool suspended) {
  const bool previous =
      visualsSuspended.exchange(suspended, std::memory_order_acq_rel);
  if (previous == suspended) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      if (videoPlayer.second != nullptr) {
        videoPlayer.second->setDecodeSuspended(
            suspended || !visualsEnabled.load(std::memory_order_relaxed));
      }
    }
  }
  wakeScheduler();
}

bool Jukebox::getVisualsSuspended() const {
  return visualsSuspended.load(std::memory_order_acquire);
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
  if (isCancelled) {
    return false;
  }
  auto videoPlayer = std::make_unique<VideoPlayer>(stopwatch);
  const path_t playablePath = fspath_to_path_t(materializedPath);

  if (videoPlayer->loadVideo(path_t_to_utf8(playablePath), isCancelled)) {
    if (isCancelled) {
      return false;
    }
    auto *loadedVideoPlayer = videoPlayer.get();
    loadedVideoPlayer->setDecodeSuspended(
        !visualsEnabled.load(std::memory_order_relaxed) ||
        visualsSuspended.load(std::memory_order_acquire));
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    replaceVideoPlayerLocked(videoPlayerTable, id, std::move(videoPlayer));
    videoMaterializedPathTable[id] = materializedPath.lexically_normal();
    visualPathTable[id] = fspath_to_path_t(displayPath);

    SDL_Log("video width: %f, video height: %f", loadedVideoPlayer->viewWidth,
            loadedVideoPlayer->viewHeight);
    SDL_Log("Loaded video to id: %d", id);
    return true;
  }

  SDL_Log("Failed to load video: %s",
          fspath_to_utf8(displayPath).c_str());
  return false;
}

bool Jukebox::loadVideoPath(int id, const std::filesystem::path &path,
                            std::atomic_bool &isCancelled) {
  if (isCancelled) {
    return false;
  }
#if TARGET_OS_ANDROID
  if (IsAndroidTreePath(path)) {
    std::string fdError;
    const auto fd = OpenAndroidTreeFileDescriptor(path, fdError);
    if (!fd.has_value()) {
      SDL_Log("Failed to open Android video descriptor: %s", fdError.c_str());
      return false;
    }

    UniqueFd fdGuard(*fd);
    const std::filesystem::path playablePath =
        std::filesystem::path("/proc/self/fd") / std::to_string(*fd);
    const bool loaded = loadMaterializedVideoPath(id, playablePath, path,
                                                  isCancelled);
    if (loaded) {
      std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
      videoMaterializedPathTable.erase(id);
    }
    return loaded;
  }
#endif
  std::string materializeError;
  const auto playablePath =
      archive_file::materializeFile(path, &materializeError, &isCancelled);
  if (isCancelled) {
    return false;
  }
  if (!playablePath.has_value()) {
    SDL_Log("Failed to materialize video: %s", materializeError.c_str());
    return false;
  }
  return loadMaterializedVideoPath(id, *playablePath, path, isCancelled);
}

bool Jukebox::loadImageBytes(int id, const std::filesystem::path &path,
                             const std::vector<unsigned char> &bytes,
                             std::atomic_bool &isCancelled) {
  if (isCancelled) {
    return false;
  }
  const path_t displayPath = fspath_to_path_t(path);
  const std::string utf8Path = path_t_to_utf8(displayPath);
  int width, height, channels;
  StbiImageHandle data(decodeImageBytes(bytes, &width, &height, &channels, 4));
  if (isCancelled) {
    return false;
  }
  if (!data) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  if (!decodedImageDimensionsAreValid(width, height)) {
    SDL_Log("Invalid image dimensions for %s: %dx%d", utf8Path.c_str(), width,
            height);
    return false;
  }
  if (isCancelled) {
    return false;
  }
  const auto texture = bgfx::createTexture2D(
      width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
      bgfx::copy(data.get(), width * height * 4));
  if (!bgfx::isValid(texture)) {
    SDL_Log("Failed to create image texture: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s", utf8Path.c_str());
  ImageData image{
      .texture = texture,
      .width = width,
      .height = height,
      .channels = channels,
  };
  auto textureGuard = makeScopeExit([&image] { destroyImageTexture(image); });
  if (isCancelled) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    if (isCancelled) {
      return false;
    }
    replaceImageLocked(imageTable, id, image);
    visualPathTable[id] = displayPath;
    textureGuard.dismiss();
  }
  return true;
}

bool Jukebox::loadImagePath(int id, const std::filesystem::path &path,
                            std::atomic_bool &isCancelled) {
  if (isCancelled) {
    return false;
  }
  const path_t displayPath = fspath_to_path_t(path);
  const std::string utf8Path = path_t_to_utf8(displayPath);
  int width, height, channels;
  StbiImageHandle data(loadImageFile(path, &width, &height, &channels, 4));
  if (isCancelled) {
    return false;
  }
  if (!data) {
    SDL_Log("Failed to load image: %s", utf8Path.c_str());
    return false;
  }
  if (!decodedImageDimensionsAreValid(width, height)) {
    SDL_Log("Invalid image dimensions for %s: %dx%d", utf8Path.c_str(), width,
            height);
    return false;
  }
  if (isCancelled) {
    return false;
  }
  const auto texture = bgfx::createTexture2D(
      width, height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE,
      bgfx::copy(data.get(), width * height * 4));
  if (!bgfx::isValid(texture)) {
    SDL_Log("Failed to create image texture: %s", utf8Path.c_str());
    return false;
  }
  SDL_Log("Loaded image: %s", utf8Path.c_str());
  ImageData image{
      .texture = texture,
      .width = width,
      .height = height,
      .channels = channels,
  };
  auto textureGuard = makeScopeExit([&image] { destroyImageTexture(image); });
  if (isCancelled) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    if (isCancelled) {
      return false;
    }
    replaceImageLocked(imageTable, id, image);
    visualPathTable[id] = displayPath;
    textureGuard.dismiss();
  }
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
                         const ChartResourceTable &wavTable,
                         std::atomic_bool &isCancelled) {
  if (loadArchivedSounds(chart, wavTable, isCancelled)) {
    return;
  }

  using Clock = std::chrono::steady_clock;
  const auto loadStart = Clock::now();
  const std::size_t wavCount = chart.WavTable.size();
  const unsigned int workerCount = parallel_worker_count(wavTable.size());
  SDL_Log("Loading %zu referenced sounds from %zu wav entries using %u workers",
          wavTable.size(), wavCount, workerCount);

  std::vector<std::pair<int, std::optional<path_t>>> resolvedSoundPaths;
  resolvedSoundPaths.reserve(wavTable.size());
  for (const auto &[wavId, wavPath] : wavTable) {
    (void)wavPath;
    resolvedSoundPaths.emplace_back(wavId, std::nullopt);
  }
  std::atomic_size_t failedCount{0};
  const auto audioExtensionViews = makeAudioExtensionViews();

  wavTableAbs.clear();

  parallel_for_each_index(resolvedSoundPaths.size(), [&](size_t i) {
    if (isCancelled)
      return;
    const int wavId = resolvedSoundPaths[i].first;
    const auto wavIt = wavTable.find(wavId);
    if (wavIt == wavTable.end()) {
      return;
    }
    const auto &wavPath = wavIt->second;
    const std::filesystem::path basePath = chart.Meta.Folder / wavPath;
    const auto resolvedPath =
        archive_file::findFileWithExtensions(basePath, audioExtensionViews);
    if (resolvedPath.has_value()) {
      resolvedSoundPaths[i].second = fspath_to_path_t(*resolvedPath);
    } else {
      failedCount.fetch_add(1, std::memory_order_relaxed);
      SDL_Log("Failed to load sound for all extensions: %s",
              fspath_to_utf8(basePath).c_str());
    }
  });
  if (isCancelled) {
    return;
  }

  std::unordered_map<path_t, std::vector<int>> idsByPath;
  std::vector<path_t> uniqueSoundPaths;
  uniqueSoundPaths.reserve(wavTable.size());
  std::size_t duplicateCount = 0;
  for (const auto &[wavId, resolvedPath] : resolvedSoundPaths) {
    if (!resolvedPath.has_value()) {
      continue;
    }
    const path_t &soundPath = *resolvedPath;
    auto [idsIt, inserted] = idsByPath.emplace(soundPath, std::vector<int>{});
    if (inserted) {
      uniqueSoundPaths.push_back(soundPath);
    } else {
      ++duplicateCount;
    }
    idsIt->second.push_back(wavId);
  }

  std::mutex loadedPathsMutex;
  std::unordered_set<path_t> loadedPaths;
  parallel_for_each_index(uniqueSoundPaths.size(), [&](size_t i) {
    if (isCancelled) {
      return;
    }
    const path_t &soundPath = uniqueSoundPaths[i];
    if (!audio.loadSound(soundPath, isCancelled)) {
      return;
    }
    std::lock_guard<std::mutex> lock(loadedPathsMutex);
    loadedPaths.insert(soundPath);
  });
  if (isCancelled) {
    return;
  }

  std::size_t loadedIdCount = 0;
  for (const auto &[soundPath, ids] : idsByPath) {
    if (!loadedPaths.contains(soundPath)) {
      failedCount.fetch_add(ids.size(), std::memory_order_relaxed);
      for (const int wavId : ids) {
        SDL_Log("Failed to load sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
      continue;
    }
    for (const int wavId : ids) {
      wavTableAbs[wavId] = soundPath;
      ++loadedIdCount;
      SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "Loaded sound %d: %s",
                     wavId, path_t_to_utf8(soundPath).c_str());
    }
  }

  const double elapsedSeconds =
      std::chrono::duration<double>(Clock::now() - loadStart).count();
  SDL_Log("Loaded sounds summary: wav=%zu referenced=%zu unique=%zu loaded=%zu "
          "duplicate=%zu failed=%zu workers=%u time=%.2fs",
          wavCount, wavTable.size(), uniqueSoundPaths.size(), loadedIdCount,
          duplicateCount, failedCount.load(std::memory_order_relaxed),
          workerCount, elapsedSeconds);
}

bool Jukebox::loadArchivedSounds(bms_parser::Chart &chart,
                                 const ChartResourceTable &wavTable,
                                 std::atomic_bool &isCancelled) {
  bool hasVirtualAssetBase = false;
  for (const auto &[id, wavPath] : wavTable) {
    (void)id;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / wavPath,
                                       archivePath, innerPath)) {
      hasVirtualAssetBase = true;
      break;
    }
  }
  if (!hasVirtualAssetBase) {
    return false;
  }

  const auto audioExtensionViews = makeAudioExtensionViews();
  std::unordered_map<path_t, ArchiveAssetBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::vector<std::pair<int, path_t>> regularLoads;
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;

  wavTableAbs.clear();
  for (const auto &[wavId, wavPath] : wavTable) {
    if (isCancelled) {
      return true;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / wavPath;
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
              fspath_to_utf8(basePath).c_str());
      continue;
    }

    const std::filesystem::path resolvedAssetPath = *resolvedPath;
    const path_t soundPath = fspath_to_path_t(resolvedAssetPath);
    if (!addArchiveAssetTarget(archiveBatches, archiveBatchOrder,
                               resolvedAssetPath, wavId)) {
      regularLoads.emplace_back(wavId, soundPath);
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
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read sounds from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read sound batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " +
          errorMessage);
      continue;
    }

    std::mutex loadedPathsMutex;
    std::unordered_set<path_t> loadedPaths;
    parallel_for_each_index(files.size(), [&](size_t i) {
      if (isCancelled) {
        return;
      }
      const auto &file = files[i];
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t soundPath = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.idsByPath.find(soundPath);
      if (idsIt == batch.idsByPath.end()) {
        return;
      }

      if (!audio.loadSoundFromMemory(soundPath, file.bytes, isCancelled)) {
        return;
      }

      {
        std::lock_guard<std::mutex> lock(loadedPathsMutex);
        loadedPaths.insert(soundPath);
        for (const int wavId : idsIt->second) {
          wavTableAbs[wavId] = soundPath;
          SDL_Log("Loaded sound %d: %s", wavId,
                  path_t_to_utf8(soundPath).c_str());
        }
      }
    });

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

bool Jukebox::loadArchivedChartAssets(
    bms_parser::Chart &chart, const ChartResourceTable &wavTable,
    const ChartResourceTable &bmpTable, bool loadVisualAssets,
    std::atomic_bool &isCancelled,
    audio::playback::BackendOperationResult &lifecycleResult) {
  using Clock = std::chrono::steady_clock;

  if (!chartHasVirtualAssetBase(chart, wavTable, bmpTable,
                                loadVisualAssets)) {
    return false;
  }

  std::vector<path_t> obsoletePaths;
  obsoletePaths.reserve(wavTableAbs.size());
  for (const auto &[wavId, path] : wavTableAbs) {
    (void)wavId;
    obsoletePaths.push_back(path);
  }
  auto unloaded = jukebox_sound_resources::PruneAndCommitSoundMap(
      audio, wavTableAbs, {}, obsoletePaths);
  if (!unloaded.success) {
    lifecycleResult = jukebox_lifecycle::ContextualizeFailure(
        std::move(unloaded), "Jukebox::loadChart", "unload");
    return true;
  }
  clearVisualResources();
  if (isCancelled) {
    return true;
  }

  const auto audioExtensionViews = makeAudioExtensionViews();
  const auto imageExtensionViews =
      toExtensionViews(imageExtensions, std::size(imageExtensions));
  const std::vector<std::string_view> noExtensions;

  std::unordered_map<path_t, ArchiveChartAssetBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::vector<std::pair<int, path_t>> regularSounds;
  std::vector<std::pair<int, std::filesystem::path>> regularVideos;
  std::vector<std::pair<int, std::filesystem::path>> regularImages;
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;

  for (const auto &[wavId, wavPath] : wavTable) {
    if (isCancelled) {
      return true;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / wavPath;
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
              fspath_to_utf8(basePath).c_str());
      continue;
    }

    if (!addArchiveChartAssetTarget(archiveBatches, archiveBatchOrder,
                                    *resolvedPath, wavId,
                                    ArchiveChartAssetKind::Sound)) {
      const std::filesystem::path resolvedSoundPath = *resolvedPath;
      regularSounds.emplace_back(wavId, fspath_to_path_t(resolvedSoundPath));
    }
  }

  if (loadVisualAssets) {
    for (const auto &[bmpId, bmpPath] : bmpTable) {
      if (isCancelled) {
        return true;
      }

      const std::filesystem::path basePath = chart.Meta.Folder / bmpPath;
      std::filesystem::path archivePath;
      std::filesystem::path innerPath;
      const bool baseIsVirtual =
          archive_file::splitVirtualPath(basePath, archivePath, innerPath);

      std::optional<std::filesystem::path> resolvedVideoPath;
      if (baseIsVirtual) {
        if (ArchiveEntryLookup *lookup =
                getArchiveLookup(archivePath, lookups)) {
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
        if (!addArchiveChartAssetTarget(archiveBatches, archiveBatchOrder,
                                        *resolvedVideoPath, bmpId,
                                        ArchiveChartAssetKind::Video)) {
          regularVideos.emplace_back(bmpId, *resolvedVideoPath);
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
          if (ArchiveEntryLookup *lookup =
                  getArchiveLookup(archivePath, lookups)) {
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
        if (!addArchiveChartAssetTarget(archiveBatches, archiveBatchOrder,
                                        *resolvedImagePath, bmpId,
                                        ArchiveChartAssetKind::Image)) {
          regularImages.emplace_back(bmpId, *resolvedImagePath);
        }
        found = true;
        break;
      }
      if (!found) {
        SDL_Log("Failed to load image or video for all extensions: %s",
                fspath_to_utf8(basePath).c_str());
      }
    }
  }

  if (archiveBatchOrder.empty()) {
    return false;
  }

  archive_file::appendDebugLogLine(
      "Loading archived chart assets with combined batches: archives=" +
      std::to_string(archiveBatchOrder.size()) +
      " regularSounds=" + std::to_string(regularSounds.size()) +
      " regularVideos=" + std::to_string(regularVideos.size()) +
      " regularImages=" + std::to_string(regularImages.size()));

  std::unordered_set<path_t> regularLoadedSounds;
  for (const auto &[wavId, soundPath] : regularSounds) {
    if (isCancelled) {
      return true;
    }
    const bool alreadyLoaded = regularLoadedSounds.contains(soundPath);
    if (!alreadyLoaded && !audio.loadSound(soundPath, isCancelled)) {
      continue;
    }
    regularLoadedSounds.insert(soundPath);
    wavTableAbs[wavId] = soundPath;
    SDL_Log("Loaded sound %d: %s", wavId,
            path_t_to_utf8(soundPath).c_str());
  }

  if (loadVisualAssets) {
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
      loadImagePath(id, path, isCancelled);
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

    const ArchiveChartAssetBatch &batch = batchIt->second;
    ArchiveAssetBatch readBatch{
        .archivePath = batch.archivePath,
        .innerPaths = batch.innerPaths,
        .idsByPath = {},
    };
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);

    const auto readStart = Clock::now();
    if (!readArchiveBatchEntries(readBatch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read chart assets from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read combined chart asset batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " +
          errorMessage);
      continue;
    }
    const auto readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - readStart)
                            .count();
    archive_file::appendDebugLogLine(
        "Read combined chart asset batch: " +
        fspath_to_utf8(batch.archivePath) +
        " targets=" + std::to_string(batch.innerPaths.size()) +
        " files=" + std::to_string(files.size()) +
        " ms=" + std::to_string(readMs));

    std::mutex loadedPathsMutex;
    std::unordered_set<path_t> loadedSoundPaths;
    parallel_for_each_index(files.size(), [&](size_t i) {
      if (isCancelled) {
        return;
      }

      const auto &file = files[i];
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t soundPath = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.soundIdsByPath.find(soundPath);
      if (idsIt == batch.soundIdsByPath.end()) {
        return;
      }

      if (!audio.loadSoundFromMemory(soundPath, file.bytes, isCancelled)) {
        return;
      }

      std::lock_guard<std::mutex> lock(loadedPathsMutex);
      loadedSoundPaths.insert(soundPath);
      for (const int wavId : idsIt->second) {
        wavTableAbs[wavId] = soundPath;
        SDL_Log("Loaded sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
    });

    for (const auto &[soundPath, ids] : batch.soundIdsByPath) {
      if (loadedSoundPaths.contains(soundPath)) {
        continue;
      }
      for (const int wavId : ids) {
        SDL_Log("Failed to load sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
    }

    if (!loadVisualAssets) {
      continue;
    }

    for (const auto &file : files) {
      if (isCancelled) {
        return true;
      }
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t pathKey = fspath_to_path_t(virtualPath);

      if (const auto idsIt = batch.videoIdsByPath.find(pathKey);
          idsIt != batch.videoIdsByPath.end()) {
        std::string materializeError;
        const auto playablePath = archive_file::materializeFileBytes(
            virtualPath, file.bytes, &materializeError, &isCancelled);
        if (isCancelled) {
          return true;
        }
        if (!playablePath.has_value()) {
          SDL_Log("Failed to materialize video: %s",
                  materializeError.c_str());
        } else {
          for (const int id : idsIt->second) {
            if (isCancelled) {
              return true;
            }
            loadMaterializedVideoPath(id, *playablePath, virtualPath,
                                      isCancelled);
          }
        }
      }

      if (const auto idsIt = batch.imageIdsByPath.find(pathKey);
          idsIt != batch.imageIdsByPath.end()) {
        for (const int id : idsIt->second) {
          if (isCancelled) {
            return true;
          }
          loadImageBytes(id, virtualPath, file.bytes, isCancelled);
        }
      }
    }
  }

  return true;
}

void Jukebox::loadBMPs(bms_parser::Chart &chart,
                       const ChartResourceTable &bmpTable,
                       std::atomic_bool &isCancelled) {
  if (loadArchivedBMPs(chart, bmpTable, isCancelled)) {
    return;
  }

  const auto imageExtensionViews =
      toExtensionViews(imageExtensions, std::size(imageExtensions));
  std::vector<int> bmpIds;
  bmpIds.reserve(bmpTable.size());
  for (const auto &[bmpId, bmpPath] : bmpTable) {
    (void)bmpPath;
    bmpIds.push_back(bmpId);
  }
  parallel_for_each_index(bmpIds.size(), [&](size_t i) {
    if (isCancelled)
      return;
    const int bmpId = bmpIds[i];
    const auto bmpIt = bmpTable.find(bmpId);
    if (bmpIt == bmpTable.end()) {
      return;
    }
    const auto &bmpPath = bmpIt->second;
    bool found = false;
    std::filesystem::path basePath = chart.Meta.Folder / bmpPath;
    std::filesystem::path path;

    if (auto resolvedVideoPath = findWithReplacedExtensions(
            basePath, videoExtensions, std::size(videoExtensions))) {
      if (isCancelled)
        return;
      path = *resolvedVideoPath;
      found = loadVideoPath(bmpId, path, isCancelled);
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
        if (loadImagePath(bmpId, path, isCancelled)) {
          break;
        }
      }
    }
  });
}

bool Jukebox::loadArchivedBMPs(bms_parser::Chart &chart,
                               const ChartResourceTable &bmpTable,
                               std::atomic_bool &isCancelled) {
  bool hasVirtualAssetBase = false;
  for (const auto &[id, bmpPath] : bmpTable) {
    (void)id;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chart.Meta.Folder / bmpPath,
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

  for (const auto &[bmpId, bmpPath] : bmpTable) {
    if (isCancelled) {
      return true;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / bmpPath;
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
                                 *resolvedVideoPath, bmpId)) {
        regularVideos.emplace_back(bmpId, *resolvedVideoPath);
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
                                 *resolvedImagePath, bmpId)) {
        regularImages.emplace_back(bmpId, *resolvedImagePath);
      }
      found = true;
      break;
    }
    if (!found) {
      SDL_Log("Failed to load image or video for all extensions: %s",
              fspath_to_utf8(basePath).c_str());
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
    loadImagePath(id, path, isCancelled);
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
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read videos from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read video batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " +
          errorMessage);
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
          virtualPath, file.bytes, &materializeError, &isCancelled);
      if (isCancelled) {
        return true;
      }
      if (!playablePath.has_value()) {
        SDL_Log("Failed to materialize video: %s",
                materializeError.c_str());
        continue;
      }
      for (const int id : idsIt->second) {
        if (isCancelled) {
          return true;
        }
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
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read images from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read image batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " +
          errorMessage);
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
        if (isCancelled) {
          return true;
        }
        loadImageBytes(id, virtualPath, file.bytes, isCancelled);
      }
    }
  }
  return true;
}

std::vector<Jukebox::ResolvedSoundAsset>
Jukebox::resolveSoundAssets(bms_parser::Chart &chart,
                            const ChartResourceTable &wavTable,
                            std::atomic_bool &isCancelled) {
  const auto audioExtensionViews = makeAudioExtensionViews();
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;
  std::vector<ResolvedSoundAsset> assets;
  assets.reserve(wavTable.size());

  for (const auto &[wavId, wavPath] : wavTable) {
    if (isCancelled) {
      break;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / wavPath;
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    std::optional<std::filesystem::path> resolvedPath;
    if (archive_file::splitVirtualPath(basePath, archivePath, innerPath)) {
      if (ArchiveEntryLookup *lookup = getArchiveLookup(archivePath, lookups)) {
        if (const auto resolvedInner = lookup->find(innerPath,
                                                    audioExtensionViews)) {
          resolvedPath = archive_file::makeVirtualPath(archivePath,
                                                       *resolvedInner);
        }
      }
    } else {
      resolvedPath =
          archive_file::findFileWithExtensions(basePath, audioExtensionViews);
    }

    if (!resolvedPath.has_value()) {
      SDL_Log("Failed to load sound for all extensions: %s",
              fspath_to_utf8(basePath).c_str());
      continue;
    }

    const path_t key = fspath_to_path_t(*resolvedPath);
    assets.push_back({.id = wavId, .path = *resolvedPath, .key = key});
  }

  return assets;
}

std::vector<Jukebox::ResolvedVisualAsset>
Jukebox::resolveVisualAssets(bms_parser::Chart &chart,
                             const ChartResourceTable &bmpTable,
                             std::atomic_bool &isCancelled) {
  const auto imageExtensionViews =
      toExtensionViews(imageExtensions, std::size(imageExtensions));
  const std::vector<std::string_view> noExtensions;
  std::unordered_map<path_t, ArchiveEntryLookup> lookups;
  std::vector<ResolvedVisualAsset> assets;
  assets.reserve(bmpTable.size());

  for (const auto &[bmpId, bmpPath] : bmpTable) {
    if (isCancelled) {
      break;
    }

    const std::filesystem::path basePath = chart.Meta.Folder / bmpPath;
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
      assets.push_back({.id = bmpId,
                        .path = *resolvedVideoPath,
                        .key = fspath_to_path_t(*resolvedVideoPath),
                        .video = true});
      continue;
    }

    bool found = false;
    for (const auto &ext : imageExtensionViews) {
      if (isCancelled) {
        break;
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
      assets.push_back({.id = bmpId,
                        .path = *resolvedImagePath,
                        .key = fspath_to_path_t(*resolvedImagePath),
                        .video = false});
      found = true;
      break;
    }

    if (!found && !isCancelled) {
      SDL_Log("Failed to load image or video for all extensions: %s",
              fspath_to_utf8(basePath).c_str());
    }
  }

  return assets;
}

audio::playback::BackendOperationResult Jukebox::loadResolvedChartResources(
    bms_parser::Chart &chart, const ChartResourceTable &wavTable,
    const ChartResourceTable &bmpTable, bool loadVisualAssets,
    std::atomic_bool &isCancelled) {
  const auto soundAssets = resolveSoundAssets(chart, wavTable, isCancelled);
  if (isCancelled) {
    return {.success = true};
  }
  auto reconciled = reconcileSoundResources(chart, soundAssets, isCancelled);
  if (!reconciled.success) {
    return reconciled;
  }
  if (isCancelled) {
    return {.success = true};
  }

  if (loadVisualAssets) {
    const auto visualAssets = resolveVisualAssets(chart, bmpTable, isCancelled);
    if (isCancelled) {
      return {.success = true};
    }
    reconcileVisualResources(chart, visualAssets, isCancelled);
  } else {
    clearVisualResources();
  }
  return {.success = true};
}

audio::playback::BackendOperationResult
Jukebox::reconcileSoundResources(bms_parser::Chart &chart,
                                 const std::vector<ResolvedSoundAsset> &assets,
                                 std::atomic_bool &isCancelled) {
  std::unordered_map<int, path_t> nextWavTable;
  nextWavTable.reserve(assets.size());

  std::unordered_set<path_t> oldPaths;
  oldPaths.reserve(wavTableAbs.size());
  std::unordered_set<path_t> requiredPaths;
  requiredPaths.reserve(assets.size());
  std::vector<ResolvedSoundAsset> assetsToLoad;
  assetsToLoad.reserve(assets.size());

  for (const auto &[id, path] : wavTableAbs) {
    (void)id;
    oldPaths.insert(path);
  }
  for (const auto &asset : assets) {
    requiredPaths.insert(asset.key);
    if (oldPaths.contains(asset.key)) {
      nextWavTable[asset.id] = asset.key;
    } else {
      assetsToLoad.push_back(asset);
    }
  }

  std::unordered_map<path_t, ArchiveAssetBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::vector<ResolvedSoundAsset> regularLoads;
  for (const auto &asset : assetsToLoad) {
    if (!addArchiveAssetTarget(archiveBatches, archiveBatchOrder, asset.path,
                               asset.id)) {
      regularLoads.push_back(asset);
    }
  }

  std::unordered_map<path_t, std::vector<int>> regularIdsByPath;
  std::vector<path_t> regularLoadPaths;
  regularLoadPaths.reserve(regularLoads.size());
  for (const auto &asset : regularLoads) {
    auto [idsIt, inserted] = regularIdsByPath.emplace(asset.key,
                                                      std::vector<int>{});
    if (inserted) {
      regularLoadPaths.push_back(asset.key);
    }
    idsIt->second.push_back(asset.id);
  }

  std::mutex loadedRegularPathsMutex;
  std::unordered_set<path_t> loadedRegularPaths;
  parallel_for_each_index(regularLoadPaths.size(), [&](size_t i) {
    if (isCancelled) {
      return;
    }
    const path_t &path = regularLoadPaths[i];
    if (!audio.loadSound(path, isCancelled)) {
      return;
    }
    std::lock_guard<std::mutex> lock(loadedRegularPathsMutex);
    loadedRegularPaths.insert(path);
  });

  for (const auto &[path, ids] : regularIdsByPath) {
    if (!loadedRegularPaths.contains(path)) {
      for (const int wavId : ids) {
        SDL_Log("Failed to load sound %d: %s", wavId,
                path_t_to_utf8(path).c_str());
      }
      continue;
    }
    for (const int wavId : ids) {
      nextWavTable[wavId] = path;
      SDL_Log("Loaded sound %d: %s", wavId, path_t_to_utf8(path).c_str());
    }
  }

  for (const auto &archiveKey : archiveBatchOrder) {
    if (isCancelled) {
      return {.success = true};
    }
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      continue;
    }

    const ArchiveAssetBatch &batch = batchIt->second;
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);
    if (!readArchiveBatchEntries(batch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read sounds from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read differential sound batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " + errorMessage);
      continue;
    }

    std::mutex loadedPathsMutex;
    std::unordered_set<path_t> loadedPaths;
    parallel_for_each_index(files.size(), [&](size_t i) {
      if (isCancelled) {
        return;
      }
      const auto &file = files[i];
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t soundPath = fspath_to_path_t(virtualPath);
      const auto idsIt = batch.idsByPath.find(soundPath);
      if (idsIt == batch.idsByPath.end()) {
        return;
      }
      if (!audio.loadSoundFromMemory(soundPath, file.bytes, isCancelled)) {
        return;
      }
      std::lock_guard<std::mutex> lock(loadedPathsMutex);
      loadedPaths.insert(soundPath);
    });

    for (const auto &[soundPath, ids] : batch.idsByPath) {
      if (!loadedPaths.contains(soundPath)) {
        for (const int wavId : ids) {
          SDL_Log("Failed to load sound %d: %s", wavId,
                  path_t_to_utf8(soundPath).c_str());
        }
        continue;
      }
      for (const int wavId : ids) {
        nextWavTable[wavId] = soundPath;
        SDL_Log("Loaded sound %d: %s", wavId,
                path_t_to_utf8(soundPath).c_str());
      }
    }
  }

  std::vector<path_t> obsoletePaths;
  obsoletePaths.reserve(oldPaths.size());
  for (const path_t &path : oldPaths) {
    if (!requiredPaths.contains(path)) {
      obsoletePaths.push_back(path);
    }
  }
  auto committed = jukebox_sound_resources::PruneAndCommitSoundMap(
      audio, wavTableAbs, std::move(nextWavTable), obsoletePaths);
  if (!committed.success) {
    return jukebox_lifecycle::ContextualizeFailure(
        std::move(committed), "Jukebox::reconcileSoundResources", "unload");
  }
  return committed;
}

void Jukebox::reconcileVisualResources(
    bms_parser::Chart &chart, const std::vector<ResolvedVisualAsset> &assets,
    std::atomic_bool &isCancelled) {
  std::unordered_map<int, std::unique_ptr<VideoPlayer>> oldVideoPlayers;
  std::unordered_map<int, std::filesystem::path> oldMaterializedPaths;
  std::unordered_map<int, ImageData> oldImages;
  std::unordered_map<int, path_t> oldVisualPaths = std::move(visualPathTable);

  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    oldVideoPlayers = std::move(videoPlayerTable);
    oldMaterializedPaths = std::move(videoMaterializedPathTable);
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    oldImages = std::move(imageTable);
  }

  std::unordered_map<int, std::unique_ptr<VideoPlayer>> nextVideoPlayers;
  nextVideoPlayers.reserve(assets.size());
  std::unordered_map<int, std::filesystem::path> nextMaterializedPaths;
  std::unordered_map<int, ImageData> nextImages;
  nextImages.reserve(assets.size());
  std::unordered_map<int, path_t> nextVisualPaths;
  nextVisualPaths.reserve(assets.size());
  std::unordered_map<path_t, ReusableVisualSources> oldSourcesByPath;
  oldSourcesByPath.reserve(oldVisualPaths.size());
  std::vector<ResolvedVisualAsset> unresolvedAssets;
  unresolvedAssets.reserve(assets.size());

  for (const auto &[id, path] : oldVisualPaths) {
    const bool hasVideo = oldVideoPlayers.contains(id);
    const bool hasImage = oldImages.contains(id);
    if (!hasVideo && !hasImage) {
      continue;
    }

    ReusableVisualSources &sources = oldSourcesByPath[path];
    if (hasVideo) {
      sources.videoIds.push_back(id);
    }
    if (hasImage) {
      sources.imageIds.push_back(id);
    }
  }

  auto sourceIsAvailable = [&](const ResolvedVisualAsset &asset,
                               int sourceId) {
    return asset.video ? oldVideoPlayers.contains(sourceId)
                       : oldImages.contains(sourceId);
  };

  auto trySelectSourceId = [&](const ResolvedVisualAsset &asset)
      -> std::optional<int> {
    const auto sameIdPathIt = oldVisualPaths.find(asset.id);
    if (sameIdPathIt != oldVisualPaths.end() &&
        sameIdPathIt->second == asset.key &&
        sourceIsAvailable(asset, asset.id)) {
      return asset.id;
    }

    const auto sourcesIt = oldSourcesByPath.find(asset.key);
    if (sourcesIt == oldSourcesByPath.end()) {
      return std::nullopt;
    }

    ReusableVisualSources &sources = sourcesIt->second;
    std::vector<int> &ids = asset.video ? sources.videoIds : sources.imageIds;
    size_t &nextIndex =
        asset.video ? sources.nextVideoIndex : sources.nextImageIndex;
    while (nextIndex < ids.size()) {
      const int sourceId = ids[nextIndex++];
      if (sourceIsAvailable(asset, sourceId)) {
        return sourceId;
      }
    }

    return std::nullopt;
  };

  for (const auto &asset : assets) {
    const auto sourceId = trySelectSourceId(asset);
    if (!sourceId.has_value()) {
      unresolvedAssets.push_back(asset);
      continue;
    }

    nextVisualPaths[asset.id] = asset.key;
    if (asset.video) {
      nextVideoPlayers[asset.id] = std::move(oldVideoPlayers[*sourceId]);
      oldVideoPlayers.erase(*sourceId);
      if (const auto materializedIt = oldMaterializedPaths.find(*sourceId);
          materializedIt != oldMaterializedPaths.end()) {
        nextMaterializedPaths[asset.id] = std::move(materializedIt->second);
        oldMaterializedPaths.erase(materializedIt);
      }
    } else {
      const auto imageIt = oldImages.find(*sourceId);
      nextImages[asset.id] = imageIt->second;
      oldImages.erase(imageIt);
    }

    if (*sourceId != asset.id) {
      SDL_Log("Reassigned visual resource %d -> %d: %s", *sourceId, asset.id,
              path_t_to_utf8(asset.key).c_str());
    }
  }

  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    videoPlayerTable = std::move(nextVideoPlayers);
    videoMaterializedPathTable = std::move(nextMaterializedPaths);
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    imageTable = std::move(nextImages);
  }
  visualPathTable = std::move(nextVisualPaths);

  for (auto &[id, image] : oldImages) {
    (void)id;
    destroyImageTexture(image);
  }
  for (auto &[id, videoPlayer] : oldVideoPlayers) {
    (void)id;
    if (videoPlayer != nullptr) {
      videoPlayer->stop();
    }
  }

  std::unordered_map<path_t, ArchiveChartAssetBatch> archiveBatches;
  std::vector<path_t> archiveBatchOrder;
  std::vector<ResolvedVisualAsset> regularLoads;
  for (const auto &asset : unresolvedAssets) {
    if (!addArchiveChartAssetTarget(
            archiveBatches, archiveBatchOrder, asset.path, asset.id,
            asset.video ? ArchiveChartAssetKind::Video
                        : ArchiveChartAssetKind::Image)) {
      regularLoads.push_back(asset);
    }
  }

  for (const auto &asset : regularLoads) {
    if (isCancelled) {
      return;
    }
    if (asset.video) {
      loadVideoPath(asset.id, asset.path, isCancelled);
    } else {
      loadImagePath(asset.id, asset.path, isCancelled);
    }
  }

  for (const auto &archiveKey : archiveBatchOrder) {
    if (isCancelled) {
      return;
    }
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      continue;
    }

    const ArchiveChartAssetBatch &batch = batchIt->second;
    ArchiveAssetBatch readBatch{
        .archivePath = batch.archivePath,
        .innerPaths = batch.innerPaths,
        .idsByPath = {},
    };
    std::vector<archive_file::FileData> files;
    std::string errorMessage;
    const auto entryRange = entryRangeForChartArchive(chart, batch.archivePath);
    if (!readArchiveBatchEntries(readBatch, entryRange, files, &errorMessage,
                                 isCancelled)) {
      SDL_Log("Failed to read visual assets from archive %s: %s",
              fspath_to_utf8(batch.archivePath).c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to read differential visual batch from archive: " +
          fspath_to_utf8(batch.archivePath) + ": " + errorMessage);
      continue;
    }

    for (const auto &file : files) {
      if (isCancelled) {
        return;
      }
      const std::filesystem::path virtualPath =
          archive_file::makeVirtualPath(batch.archivePath, file.path);
      const path_t pathKey = fspath_to_path_t(virtualPath);
      if (const auto idsIt = batch.videoIdsByPath.find(pathKey);
          idsIt != batch.videoIdsByPath.end()) {
        std::string materializeError;
        const auto playablePath = archive_file::materializeFileBytes(
            virtualPath, file.bytes, &materializeError, &isCancelled);
        if (isCancelled) {
          return;
        }
        if (!playablePath.has_value()) {
          SDL_Log("Failed to materialize video: %s",
                  materializeError.c_str());
        } else {
          for (const int id : idsIt->second) {
            if (isCancelled) {
              return;
            }
            loadMaterializedVideoPath(id, *playablePath, virtualPath,
                                      isCancelled);
          }
        }
      }
      if (const auto idsIt = batch.imageIdsByPath.find(pathKey);
          idsIt != batch.imageIdsByPath.end()) {
        for (const int id : idsIt->second) {
          if (isCancelled) {
            return;
          }
          loadImageBytes(id, virtualPath, file.bytes, isCancelled);
        }
      }
    }
  }
}

void Jukebox::clearVisualResources() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  bmpCursor = 0;
  bmpLayerCursor = 0;
  lastVisualTimelineMicros = -1;
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    videoPlayerTable.clear();
    videoMaterializedPathTable.clear();
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    for (auto &image : imageTable) {
      destroyImageTexture(image.second);
    }
    imageTable.clear();
  }
  visualPathTable.clear();
}

void Jukebox::scheduleVisuals(bms_parser::Chart &chart,
                              std::atomic_bool &isCancelled) {
  bmpCursor = 0;
  bmpLayerCursor = 0;
  lastVisualTimelineMicros = -1;
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
  schedulerActive = false;
  wakeScheduler();
  if (playThread.joinable()) {
    playThread.join();
  }
  clearVisualResources();
  scheduleVisuals(chart, isCancelled);
  if (isCancelled || !visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  loadBMPs(chart, chart.ReferencedBmpTable, isCancelled);
}

void Jukebox::unloadVisuals() { clearVisualResources(); }

audio::playback::BackendOperationResult
Jukebox::loadChart(bms_parser::Chart &chart, bool scheduleNotes,
                   std::atomic_bool &isCancelled) {
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  const auto stopped = jukebox_lifecycle::StopSessionForTransition(
      audio, "Jukebox::loadChart", lifecycleState, [this] { wakeScheduler(); });
  if (!stopped.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", stopped.diagnostic.c_str());
    return stopped;
  }
  if (playThread.joinable()) {
    SDL_Log("Joining playThread");
    playThread.join();
  }

  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      if (videoPlayer.second != nullptr) {
        videoPlayer.second->stop();
      }
    }
  }
  if (isCancelled)
    return {.success = true};

  const bool loadVisualAssets = visualsEnabled.load(std::memory_order_relaxed);
#if TARGET_OS_ANDROID
  prepareAndroidChartAssetDirectoryCache(chart, chart.ReferencedWavTable,
                                         chart.ReferencedBmpTable,
                                         loadVisualAssets, isCancelled);
  if (isCancelled)
    return {.success = true};
#endif
  audio::playback::BackendOperationResult archiveLifecycleResult{.success =
                                                                     true};
  const bool archived = loadArchivedChartAssets(
      chart, chart.ReferencedWavTable, chart.ReferencedBmpTable,
      loadVisualAssets, isCancelled, archiveLifecycleResult);
  if (!archiveLifecycleResult.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s",
                 archiveLifecycleResult.diagnostic.c_str());
    return archiveLifecycleResult;
  }
  if (archived) {
    if (isCancelled)
      return {.success = true};
    schedule(chart, scheduleNotes, isCancelled);
    SDL_Log("Chart loaded");
    return {.success = true};
  }

  auto loaded = loadResolvedChartResources(chart, chart.ReferencedWavTable,
                                           chart.ReferencedBmpTable,
                                           loadVisualAssets, isCancelled);
  if (!loaded.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", loaded.diagnostic.c_str());
    return loaded;
  }
  if (isCancelled)
    return {.success = true};
  schedule(chart, scheduleNotes, isCancelled);
  SDL_Log("Chart loaded");
  return {.success = true};
}

bool Jukebox::hasLoadedResources() const {
  if (!wavTableAbs.empty() || !visualPathTable.empty()) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    if (!videoPlayerTable.empty()) {
      return true;
    }
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    if (!imageTable.empty()) {
      return true;
    }
  }
  return false;
}

audio::playback::BackendOperationResult
Jukebox::reloadChartResources(bms_parser::Chart &chart, bool scheduleNotes,
                              std::atomic_bool &isCancelled) {
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  const auto stopped = jukebox_lifecycle::StopSessionForTransition(
      audio, "Jukebox::reloadChartResources", lifecycleState,
      [this] { wakeScheduler(); });
  if (!stopped.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", stopped.diagnostic.c_str());
    return stopped;
  }
  if (playThread.joinable()) {
    SDL_Log("Joining playThread");
    playThread.join();
  }

  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      if (videoPlayer.second != nullptr) {
        videoPlayer.second->stop();
      }
    }
  }

  if (isCancelled) {
    return {.success = true};
  }

  const bool loadVisualAssets = visualsEnabled.load(std::memory_order_relaxed);
#if TARGET_OS_ANDROID
  prepareAndroidChartAssetDirectoryCache(chart, chart.ReferencedWavTable,
                                         chart.ReferencedBmpTable,
                                         loadVisualAssets, isCancelled);
  if (isCancelled) {
    return {.success = true};
  }
#endif

  auto loaded = loadResolvedChartResources(chart, chart.ReferencedWavTable,
                                           chart.ReferencedBmpTable,
                                           loadVisualAssets, isCancelled);
  if (!loaded.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", loaded.diagnostic.c_str());
    return loaded;
  }
  if (isCancelled) {
    return {.success = true};
  }
  schedule(chart, scheduleNotes, isCancelled);
  SDL_Log("Chart resources reloaded");
  return {.success = true};
}

void Jukebox::schedule(bms_parser::Chart &chart, bool scheduleNotes,
                       std::atomic_bool &isCancelled,
                       std::optional<long long> noteScheduleCutoffMicros,
                       const prep_metronome::PrepMetronomePlan
                           *prepMetronomePlan) {
  audioCursor = 0;
  audioList.clear();
  scheduleVisuals(chart, isCancelled);
  for (auto &measure : chart.Measures) {
    if (isCancelled)
      return;
    for (auto &timeline : measure->TimeLines) {
      if (isCancelled)
        return;
      std::vector<ScheduledAudioEvent> notes;
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
          notes.push_back(makeScheduledAudioEvent(
              timeline->Timing, note->Wav, JukeboxAudioSource::ChartNote));
        }
      }
      for (auto &bgNote : timeline->BackgroundNotes) {
        if (isCancelled)
          return;
        if (bgNote->Wav == bms_parser::Parser::NoWav)
          continue;
        if (!wavTableAbs.contains(bgNote->Wav))
          continue;
        notes.push_back(makeScheduledAudioEvent(
            timeline->Timing, bgNote->Wav, JukeboxAudioSource::BackgroundNote));
      }
      std::sort(notes.begin(), notes.end(), scheduledAudioEventLess);
      for (auto &note : notes) {
        if (isCancelled)
          return;
        audioList.push_back(note);
      }
    }
  }
  if (prepMetronomePlan != nullptr && prepMetronomePlan->enabled) {
    ensurePrepMetronomeSoundsLoaded();
    for (const auto &click : prepMetronomePlan->clicks) {
      audioList.push_back(makeScheduledAudioEvent(
          click.timeMicros,
          click.accent ? kPrepMetronomeAccentWav : kPrepMetronomeRegularWav,
          JukeboxAudioSource::PrepMetronome));
    }
  }
  std::sort(audioList.begin(), audioList.end(), scheduledAudioEventLess);
}
void Jukebox::playKeySound(int wav) {
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  jukebox_lifecycle::PlayKeySoundIfPublished(lifecycleState, [this, wav] {
    if (const auto it = wavTableAbs.find(wav); it != wavTableAbs.end()) {
      audio.playSound(
          it->second.c_str(),
          audioBusForJukeboxSource(JukeboxAudioSource::DirectKeysound));
    }
  });
}

void Jukebox::scheduleAudioFromCursor() {
  while (audioCursor < audioList.size()) {
    const auto &target = audioList[audioCursor];
    if (const auto it = wavTableAbs.find(target.wav); it != wavTableAbs.end()) {
      audio.scheduleSound(it->second, target.bus, target.timeMicros);
    }
    audioCursor++;
  }
}

void Jukebox::playOverlappingAudioAt(long long micro) {
  struct OverlappingAudio {
    path_t path;
    long long offsetMicros = 0;
    audio::Bus bus = audio::Bus::Bgm;
  };
  std::vector<OverlappingAudio> overlapping;
  const auto seekIt = std::lower_bound(
      audioList.begin(), audioList.end(), micro,
      [](const ScheduledAudioEvent &entry, long long targetMicros) {
        return entry.timeMicros < targetMicros;
      });
  for (auto it = std::make_reverse_iterator(seekIt); it != audioList.rend();
       ++it) {
    const auto &target = *it;
    const auto wavIt = wavTableAbs.find(target.wav);
    if (wavIt == wavTableAbs.end()) {
      continue;
    }
    const auto duration = audio.getSoundDurationMicros(wavIt->second);
    if (!duration.has_value()) {
      continue;
    }
    const auto request = makeOverlappingAudioRequest(target, micro, *duration);
    if (!request.has_value()) {
      continue;
    }
    overlapping.push_back({.path = wavIt->second,
                           .offsetMicros = request->offsetMicros,
                           .bus = request->bus});
  }

  for (auto it = overlapping.rbegin(); it != overlapping.rend(); ++it) {
    audio.playSound(it->path, it->bus, it->offsetMicros);
  }
}

void Jukebox::ensurePrepMetronomeSoundsLoaded() {
  audio.loadGeneratedSound(kPrepMetronomeAccentPath,
                           makePrepMetronomeClick(1760.0, 0.65),
                           kPrepMetronomeChannels, kPrepMetronomeSampleRate);
  audio.loadGeneratedSound(kPrepMetronomeRegularPath,
                           makePrepMetronomeClick(1100.0, 0.5),
                           kPrepMetronomeChannels, kPrepMetronomeSampleRate);
  wavTableAbs[kPrepMetronomeAccentWav] = kPrepMetronomeAccentPath;
  wavTableAbs[kPrepMetronomeRegularWav] = kPrepMetronomeRegularPath;
}

void Jukebox::wakeScheduler() { schedulerWakeCv.notify_all(); }

void Jukebox::syncVisualClockToAudio() {
  std::lock_guard<std::mutex> positionLock(seekLock);
  if (schedulerActive.load(std::memory_order_acquire) &&
      isPlaying.load(std::memory_order_acquire) && stopwatch->isRunning()) {
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
  return activateVisualAt(visualId, viewId, 0);
}

bool Jukebox::activateVisualAt(int visualId, bgfx::ViewId viewId,
                               long long elapsedMicros) {
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    auto videoIt = videoPlayerTable.find(visualId);
    if (videoIt != videoPlayerTable.end()) {
      auto *videoPlayer = videoIt->second.get();
      if (elapsedMicros > 0) {
        videoPlayer->playFrom(elapsedMicros);
      } else {
        videoPlayer->seek(0);
        videoPlayer->play();
      }
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

void Jukebox::advanceVisualsAtTimelineMicros(long long bgaTimelineMicros) {
  bgaTimelineMicros = std::max(0LL, bgaTimelineMicros);

  std::lock_guard<std::mutex> lock(seekLock);
  if (lastVisualTimelineMicros < 0 ||
      bgaTimelineMicros < lastVisualTimelineMicros) {
    currentBga.store(-1, std::memory_order_relaxed);
    currentBmpLayer.store(-1, std::memory_order_relaxed);
    bmpCursor = 0;
    bmpLayerCursor = 0;
    std::lock_guard<std::mutex> videoLock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      videoPlayer.second->stop();
    }
  }

  stopwatch->seek(bgaTimelineMicros);
  while (bmpCursor < bmpList.size()) {
    const auto &target = bmpList[bmpCursor];
    if (bgaTimelineMicros < target.first) {
      break;
    }
    const long long elapsedMicros =
        std::max(0LL, bgaTimelineMicros - target.first);
    if (activateVisualAt(target.second, rendering::bga_view, elapsedMicros)) {
      currentBga.store(target.second, std::memory_order_relaxed);
    }
    bmpCursor++;
  }
  while (bmpLayerCursor < bmpLayerList.size()) {
    const auto &target = bmpLayerList[bmpLayerCursor];
    if (bgaTimelineMicros < target.first) {
      break;
    }
    const long long elapsedMicros =
        std::max(0LL, bgaTimelineMicros - target.first);
    if (activateVisualAt(target.second, rendering::bga_layer_view,
                         elapsedMicros)) {
      currentBmpLayer.store(target.second, std::memory_order_relaxed);
    }
    bmpLayerCursor++;
  }
  lastVisualTimelineMicros = bgaTimelineMicros;
}

void Jukebox::seekVisualsToSongTime(long long rawSongMicros) {
  if (!visualsEnabled.load(std::memory_order_relaxed) ||
      visualsSuspended.load(std::memory_order_acquire)) {
    return;
  }
  advanceVisualsAtTimelineMicros(getBgaTimelineMicros(rawSongMicros));
}

void Jukebox::renderVisualsAt(long long micro) {
  if (!visualsEnabled.load(std::memory_order_relaxed) ||
      visualsSuspended.load(std::memory_order_acquire)) {
    return;
  }
  advanceVisualsAtTimelineMicros(micro);
  render();
}

audio::playback::BackendOperationResult Jukebox::play(long long startMicros) {
  std::lock_guard<std::mutex> lock(playThreadLock);
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  const auto stopped = jukebox_lifecycle::StopSessionForTransition(
      audio, "Jukebox::play", lifecycleState, [this] { wakeScheduler(); });
  if (!stopped.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", stopped.diagnostic.c_str());
    return stopped;
  }
  if (playThread.joinable()) {
    playThread.join();
  }

  const long long bgaTimelineMicro = getBgaTimelineMicros(startMicros);
  const jukebox_lifecycle::CursorPosition target{
      .audio = static_cast<size_t>(std::distance(
          audioList.begin(),
          std::lower_bound(
              audioList.begin(), audioList.end(), startMicros,
              [](const ScheduledAudioEvent &event, long long targetMicros) {
                return event.timeMicros < targetMicros;
              }))),
      .bmp = static_cast<size_t>(std::distance(
          bmpList.begin(),
          std::lower_bound(bmpList.begin(), bmpList.end(), bgaTimelineMicro,
                           [](const auto &event, long long targetMicros) {
                             return event.first < targetMicros;
                           }))),
      .bmpLayer = static_cast<size_t>(std::distance(
          bmpLayerList.begin(),
          std::lower_bound(bmpLayerList.begin(), bmpLayerList.end(),
                           bgaTimelineMicro,
                           [](const auto &event, long long targetMicros) {
                             return event.first < targetMicros;
                           }))),
  };
  const auto started = jukebox_lifecycle::StartPlayback(
      audio, "Jukebox::play", lifecycleState, target,
      [this, startMicros] {
        audio.seekClock(startMicros);
        scheduleAudioFromCursor();
        playOverlappingAudioAt(startMicros);
      },
      [this] { wakeScheduler(); });
  if (!started.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", started.diagnostic.c_str());
    return started;
  }
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
    jukebox_lifecycle::SessionState lifecycleState{
        .isPlaying = isPlaying,
        .schedulerActive = schedulerActive,
        .stopwatch = *stopwatch,
        .transitionMutex = playThreadLock,
        .positionMutex = seekLock,
        .audioCursor = audioCursor,
        .bmpCursor = bmpCursor,
        .bmpLayerCursor = bmpLayerCursor,
        .currentBga = currentBga,
        .currentBmpLayer = currentBmpLayer,
    };
    while (schedulerActive.load(std::memory_order_acquire)) {
      if (!isPlaying.load(std::memory_order_acquire) ||
          !stopwatch->isRunning()) {
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
        if (!jukebox_lifecycle::CanAdvanceSchedulerLocked(lifecycleState)) {
          prevTimestamp = Clock::now();
          continue;
        }
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

        const bool shouldAdvanceVisuals =
            visualsEnabled.load(std::memory_order_relaxed) &&
            !visualsSuspended.load(std::memory_order_acquire);
        if (shouldAdvanceVisuals) {
          while (bmpCursor < bmpList.size()) {
            auto &target = bmpList[bmpCursor];
            if (bgaPositionMicro < target.first) {
              break;
            }
            bool activated = false;
            {
              std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
              auto videoIt = videoPlayerTable.find(target.second);
              if (videoIt != videoPlayerTable.end()) {
                auto *videoPlayer = videoIt->second.get();
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
            bool activated = false;
            {
              std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
              auto videoIt = videoPlayerTable.find(target.second);
              if (videoIt != videoPlayerTable.end()) {
                auto *videoPlayer = videoIt->second.get();
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
  return {.success = true};
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
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  return jukebox_lifecycle::ReadPublishedTime(
      lifecycleState, [this] { return audio.getTimeMicros(); });
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
audio::playback::BackendOperationResult Jukebox::stop() {
  std::lock_guard<std::mutex> playGuard(playThreadLock);
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  const auto stopped = jukebox_lifecycle::StopPlayback(
      audio, "Jukebox::stop", lifecycleState, [this] { wakeScheduler(); });
  if (!stopped.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", stopped.diagnostic.c_str());
    return stopped;
  }
  if (playThread.joinable())
    playThread.join();
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
  return {.success = true};
}

audio::playback::BackendOperationResult Jukebox::seek(long long micro) {
  jukebox_lifecycle::SessionState lifecycleState{
      .isPlaying = isPlaying,
      .schedulerActive = schedulerActive,
      .stopwatch = *stopwatch,
      .transitionMutex = playThreadLock,
      .positionMutex = seekLock,
      .audioCursor = audioCursor,
      .bmpCursor = bmpCursor,
      .bmpLayerCursor = bmpLayerCursor,
      .currentBga = currentBga,
      .currentBmpLayer = currentBmpLayer,
  };
  const long long bgaTimelineMicro = getBgaTimelineMicros(micro);
  const jukebox_lifecycle::CursorPosition target{
      .audio = static_cast<size_t>(std::distance(
          audioList.begin(),
          std::lower_bound(
              audioList.begin(), audioList.end(), micro,
              [](const ScheduledAudioEvent &event, long long targetMicros) {
                return event.timeMicros < targetMicros;
              }))),
      .bmp = static_cast<size_t>(std::distance(
          bmpList.begin(),
          std::lower_bound(bmpList.begin(), bmpList.end(), bgaTimelineMicro,
                           [](const auto &event, long long targetMicros) {
                             return event.first < targetMicros;
                           }))),
      .bmpLayer = static_cast<size_t>(std::distance(
          bmpLayerList.begin(),
          std::lower_bound(bmpLayerList.begin(), bmpLayerList.end(),
                           bgaTimelineMicro,
                           [](const auto &event, long long targetMicros) {
                             return event.first < targetMicros;
                           }))),
  };

  const auto transitioned = jukebox_lifecycle::ExecuteSeekTransition(
      audio, "Jukebox::seek", lifecycleState, target, bgaTimelineMicro,
      [this, micro](bool wasPlaying) {
        audio.seekClock(micro);
        if (wasPlaying) {
          scheduleAudioFromCursor();
          playOverlappingAudioAt(micro);
        }
      },
      [this] { wakeScheduler(); });
  if (!transitioned.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "%s", transitioned.diagnostic.c_str());
  }
  return transitioned;
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
