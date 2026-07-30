#include "ChartLibraryScanner.h"

#include "ArchiveFile.h"
#include "BmsChartFile.h"
#include "BmsMetadataText.h"
#include "CanonicalDigest.h"
#include "ChartScanWorkScheduler.h"
#include "ThreadCompat.h"
#include "Utils.h"
#include "path.h"
#include "repositories/ChartStorageIdentity.h"
#include "targets.h"
#if TARGET_OS_ANDROID
#include "AndroidNatives.h"
#endif

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace {
using asobmshow::bms_metadata::normalizedHash;

constexpr int kArchiveParseCheckpointInterval = 100;
constexpr int kIndividualParseCheckpointInterval = 1000;
constexpr std::size_t kIndividualParseBatchSize = 512;
constexpr std::size_t kArchiveParseMaxInFlightFiles = 12;
constexpr std::size_t kArchiveParseResultChunkSize =
    kArchiveParseMaxInFlightFiles;
constexpr std::uint64_t kArchiveParseMaxInFlightBytes =
    16ull * 1024ull * 1024ull;
constexpr std::size_t kArchiveDirectConcurrentMinCharts = 16;
constexpr std::size_t kArchiveClassificationPauseInterval = 256;
constexpr const char *kScanCheckpointPhaseIndividual = "individual";
constexpr const char *kScanCheckpointPhaseArchive = "archive";

std::int64_t clampScanInteger(std::uint64_t value) {
  return value > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())
             ? std::numeric_limits<std::int64_t>::max()
             : static_cast<std::int64_t>(value);
}

bool splitStoredArchiveVirtualPath(const std::filesystem::path &path,
                                   std::filesystem::path &archivePath,
                                   std::filesystem::path &innerPath) {
  archivePath.clear();
  innerPath.clear();
  if (path.empty()) {
    return false;
  }

  std::filesystem::path current;
  bool foundArchive = false;
  for (const auto &part : path.lexically_normal()) {
    if (!foundArchive) {
      current /= part;
      if (archive_file::hasSupportedArchiveExtension(current)) {
        archivePath = current;
        foundArchive = true;
      }
      continue;
    }
    innerPath /= part;
  }

  if (!foundArchive || innerPath.empty()) {
    archivePath.clear();
    innerPath.clear();
    return false;
  }
  return true;
}

bool stopRequested(const std::stop_token *stopToken) {
  return stopToken != nullptr && stopToken->stop_requested();
}

bool parsedChartMetaHasStableIdentity(const bms_parser::ChartMeta &meta) {
  const std::string md5 = normalizedHash(meta.MD5);
  const std::string sha256 = normalizedHash(meta.SHA256);
  return !meta.BmsPath.empty() &&
         canonical_digest::isCanonicalLowerHex(md5, 32) &&
         canonical_digest::isCanonicalLowerHex(sha256, 64);
}

bool parsedChartMetaLooksInsertable(const bms_parser::ChartMeta &meta) {
  return parsedChartMetaHasStableIdentity(meta) &&
         (meta.TotalNotes > 0 || meta.TotalLandmineNotes > 0);
}

struct ArchiveScanResult {
  bool readable = false;
  bool solid = false;
  int fileCount = 0;
  std::uint64_t uncompressedSize = 0;
  std::vector<std::filesystem::path> chartPaths;
};

ArchiveScanResult
scanArchiveForChartsOrSolid(const std::filesystem::path &archivePath,
                            const ChartScanPauseCallback &pauseCallback) {
  auto pauseIfNeeded = [&]() {
    return pauseCallback == nullptr || pauseCallback();
  };
  ArchiveScanResult result;
  std::vector<archive_file::Entry> entries;
  std::string errorMessage;
  const std::string archiveText = fspath_to_utf8(archivePath);
  archive_file::appendDebugLogLine("Scanning archive for BMS charts: " +
                                   archiveText);
  if (!pauseIfNeeded()) {
    return result;
  }
  if (!archive_file::listEntries(archivePath, entries, &errorMessage,
                                 pauseCallback)) {
    if (!errorMessage.empty()) {
      SDL_Log("Failed to scan archive %s: %s", archiveText.c_str(),
              errorMessage.c_str());
      archive_file::appendDebugLogLine(
          "Failed to scan archive: " + archiveText + ": " + errorMessage);
    }
    return result;
  }
  result.readable = true;
  std::unordered_set<path_t> seenArchiveChartPaths;
  for (std::size_t entryIndex = 0; entryIndex < entries.size(); ++entryIndex) {
    if (entryIndex > 0 &&
        entryIndex % kArchiveClassificationPauseInterval == 0 &&
        !pauseIfNeeded()) {
      result.readable = false;
      return result;
    }
    const auto &entry = entries[entryIndex];
    if (entry.directory) {
      continue;
    }
    ++result.fileCount;
    const std::uint64_t remaining =
        std::numeric_limits<std::uint64_t>::max() - result.uncompressedSize;
    result.uncompressedSize += std::min(entry.size, remaining);
    if (entry.solid) {
      result.solid = true;
    }
    if (result.solid ||
        !asobmshow::bms_chart_file::isBmsChartPath(entry.path)) {
      continue;
    }
    const std::filesystem::path chartPath =
        archive_file::makeVirtualPath(archivePath, entry.path);
    const path_t key = fspath_to_path_t(chartPath);
    if (!seenArchiveChartPaths.insert(key).second) {
      continue;
    }
    result.chartPaths.push_back(chartPath);
  }
  if (!pauseIfNeeded()) {
    result.readable = false;
    return result;
  }
  if (result.solid) {
    result.chartPaths.clear();
  }

  archive_file::appendDebugLogLine(
      "Archive chart scan complete: " + archiveText +
      " charts=" + std::to_string(result.chartPaths.size()) +
      " entries=" + std::to_string(entries.size()) +
      " files=" + std::to_string(result.fileCount) +
      " solid=" + std::string(result.solid ? "yes" : "no") +
      " estimatedUnpacked=" + std::to_string(result.uncompressedSize));
  if (result.solid) {
    archive_file::appendDebugLogLine(
        "Skipped chart probing for solid archive: " + archiveText);
  }
  return result;
}

std::int64_t fileTimeToSqlNs(std::filesystem::file_time_type time) {
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         time.time_since_epoch())
                         .count();
  if (nanos > std::numeric_limits<std::int64_t>::max()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (nanos < std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(nanos);
}

bool archiveFileState(const std::filesystem::path &path,
                      std::int64_t &archiveSize, std::int64_t &mtimeNs) {
  std::error_code error;
  const bool regularFile = std::filesystem::is_regular_file(path, error);
  if (error || !regularFile) {
    return false;
  }
  error.clear();
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  error.clear();
  const auto mtime = std::filesystem::last_write_time(path, error);
  if (error) {
    return false;
  }
  archiveSize =
      clampScanInteger(static_cast<std::uint64_t>(std::min<std::uintmax_t>(
          size, std::numeric_limits<std::uint64_t>::max())));
  mtimeNs = fileTimeToSqlNs(mtime);
  return true;
}

std::string checkpointPathTextForDb(const std::filesystem::path &path) {
  return chart_storage_identity::StoredPathText(path);
}

std::string checkpointInnerPathText(const std::filesystem::path &path) {
  return path.lexically_normal().generic_string();
}

void fnv1aAppend(std::uint64_t &hash, const std::string &text) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= kPrime;
  }
  hash ^= 0xffu;
  hash *= kPrime;
}

std::string stableHashHex(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[i] = kHex[value & 0xfu];
    value >>= 4u;
  }
  return text;
}

std::optional<archive_file::SourcePreference>
archiveBatchSourcePreference(const std::filesystem::path &archivePath,
                             std::optional<bool> solidHint) {
  if (!archive_file::hasSupportedArchiveExtension(archivePath)) {
    return std::nullopt;
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(archivePath, error);
  if (error) {
    return archive_file::SourcePreference{.priority = 3, .archiveSize = 0};
  }
  return archive_file::SourcePreference{
      .priority = solidHint.value_or(false) ? 2 : 1,
      .archiveSize = static_cast<std::uint64_t>(std::min<std::uintmax_t>(
          size, std::numeric_limits<std::uint64_t>::max())),
  };
}

} // namespace

int ChartLibraryScanner::Scan(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  if (stopRequested(stopToken)) {
    return 0;
  }
  auto pauseIfNeeded = [&]() {
    return pauseCallback == nullptr || pauseCallback();
  };
  auto shouldStop = [&]() {
    return stopRequested(stopToken) || !pauseIfNeeded();
  };

  auto reportProgress = [&](int current, int total,
                            ChartScanProgressStage stage) {
    if (progressCallback != nullptr) {
      progressCallback(ChartScanProgress{
          .current = current,
          .total = total,
          .stage = stage,
      });
    }
  };

  reportProgress(0, static_cast<int>(std::max<std::size_t>(roots.size(), 1)),
                 ChartScanProgressStage::Preparing);
  if (shouldStop()) {
    return 0;
  }

  ChartScanSnapshot scanSnapshot = session.LoadScanSnapshot();
  const ChartScanCheckpoint checkpoint =
      scanSnapshot.checkpoint.value_or(ChartScanCheckpoint{});
  std::vector<bms_parser::ChartMeta> chartMetas =
      std::move(scanSnapshot.charts);

  struct ScanDiff {
    std::filesystem::path path;
    bool deleted = false;
    bool parseAttempted = false;
    std::optional<bms_parser::ChartMeta> preparedMeta;
  };
  std::vector<ScanDiff> diffs;
  std::vector<std::filesystem::path> sourcePreferenceRefreshPaths;
  struct CachedSourcePreferenceUpdate {
    std::filesystem::path path;
    int priority = 0;
    std::uint64_t archiveSize = 0;
  };
  std::vector<CachedSourcePreferenceUpdate> cachedSourcePreferenceUpdates;
  struct SolidArchiveDiff {
    std::filesystem::path path;
    bool solid = false;
    std::uint64_t uncompressedSize = 0;
    int fileCount = 0;
  };
  struct ArchiveCacheDiff {
    std::filesystem::path path;
    bool solid = false;
    std::uint64_t uncompressedSize = 0;
    int fileCount = 0;
    int chartCount = 0;
  };
  std::vector<SolidArchiveDiff> solidArchiveDiffs;
  std::vector<ArchiveCacheDiff> archiveCacheDiffs;
  std::unordered_map<path_t, ArchiveCacheDiff> pendingArchiveCacheDiffs;
  std::vector<std::filesystem::path> staleSolidArchives;
  std::vector<std::filesystem::path> reindexedArchives;
  std::unordered_set<path_t> knownChartPaths;
  std::unordered_map<path_t, int> knownArchiveChartCounts;
  std::unordered_map<path_t, int> storedArchiveChartCounts;
  std::unordered_set<path_t> scannedArchivePaths;
  diffs.reserve(chartMetas.size());
  sourcePreferenceRefreshPaths.reserve(chartMetas.size());
  cachedSourcePreferenceUpdates.reserve(chartMetas.size());

  auto archiveScanKey = [](const std::filesystem::path &archivePath) {
    return fspath_to_path_t(archivePath.lexically_normal());
  };

  std::unordered_map<path_t, const ArchiveScanCacheRecord *> archiveCacheByPath;
  archiveCacheByPath.reserve(scanSnapshot.archiveCache.size());
  for (const auto &record : scanSnapshot.archiveCache) {
    archiveCacheByPath.emplace(archiveScanKey(record.path), &record);
  }

  struct ArchiveKnownState {
    bool checked = false;
    bool fileAvailable = false;
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    bool cacheFound = false;
    ArchiveScanCacheRecord cache;
  };
  std::unordered_map<path_t, ArchiveKnownState> archiveStates;
  auto archiveStateForPath =
      [&](const std::filesystem::path &archivePath) -> ArchiveKnownState & {
    const path_t archiveKey = archiveScanKey(archivePath);
    auto [it, inserted] =
        archiveStates.emplace(archiveKey, ArchiveKnownState{});
    ArchiveKnownState &state = it->second;
    if (!state.checked) {
      state.checked = true;
      state.fileAvailable =
          archiveFileState(archivePath, state.archiveSize, state.mtimeNs);
      if (state.fileAvailable) {
        if (const auto cache = archiveCacheByPath.find(archiveKey);
            cache != archiveCacheByPath.end()) {
          state.cacheFound = true;
          state.cache = *cache->second;
        }
      }
    }
    return state;
  };

  for (const auto &chartMeta : chartMetas) {
    if (shouldStop()) {
      return 0;
    }
    if (!parsedChartMetaHasStableIdentity(chartMeta)) {
      diffs.push_back({.path = chartMeta.BmsPath, .deleted = true});
      continue;
    }

    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    const bool liveArchivePath = archive_file::splitVirtualPath(
        chartMeta.BmsPath, archivePath, innerPath);
    std::filesystem::path storedArchivePathValue;
    std::filesystem::path storedInnerPath;
    const bool storedArchivePath = splitStoredArchiveVirtualPath(
        chartMeta.BmsPath, storedArchivePathValue, storedInnerPath);
    if (storedArchivePath) {
      ++storedArchiveChartCounts[archiveScanKey(storedArchivePathValue)];
    }
    if (!liveArchivePath && storedArchivePath) {
      archivePath = storedArchivePathValue;
      innerPath = storedInnerPath;
    }
    if (liveArchivePath || storedArchivePath) {
      const ArchiveKnownState &archiveState = archiveStateForPath(archivePath);
      if (liveArchivePath || archiveState.fileAvailable) {
        const path_t archiveKey = archiveScanKey(archivePath);
        ++knownArchiveChartCounts[archiveKey];
        if (archiveState.fileAvailable && archiveState.cacheFound &&
            archiveState.cache.archiveSize == archiveState.archiveSize &&
            archiveState.cache.mtimeNs == archiveState.mtimeNs &&
            archiveState.cache.chartCount >= 0) {
          knownChartPaths.insert(fspath_to_path_t(chartMeta.BmsPath));
          cachedSourcePreferenceUpdates.push_back({
              .path = chartMeta.BmsPath,
              .priority = archiveState.cache.solid ? 2 : 1,
              .archiveSize = static_cast<std::uint64_t>(
                  std::max<std::int64_t>(0, archiveState.archiveSize)),
          });
        }
        continue;
      }
    }

    if (archive_file::exists(chartMeta.BmsPath)) {
      knownChartPaths.insert(fspath_to_path_t(chartMeta.BmsPath));
      sourcePreferenceRefreshPaths.push_back(chartMeta.BmsPath);
    } else {
      diffs.push_back({.path = chartMeta.BmsPath, .deleted = true});
    }
  }

  for (const auto &solidArchive : scanSnapshot.solidArchives) {
    if (shouldStop()) {
      return 0;
    }
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    if (!archiveFileState(solidArchive.path, archiveSize, mtimeNs)) {
      staleSolidArchives.push_back(solidArchive.path);
    }
  }

  auto parseChartMeta = [&](const std::filesystem::path &path,
                            const std::vector<unsigned char> *bytes)
      -> std::optional<bms_parser::ChartMeta> {
    const std::string chartText = fspath_to_utf8(path);
    bms_parser::Parser parser;
    bms_parser::Chart *rawChart = nullptr;
    std::unique_ptr<bms_parser::Chart> chart;
    std::atomic_bool cancelled(false);
    try {
      if (bytes != nullptr) {
        parser.Parse(*bytes, &rawChart, false, true, cancelled);
        chart.reset(rawChart);
        rawChart = nullptr;
        if (chart != nullptr) {
          chart->Meta.BmsPath = path;
          std::filesystem::path archivePath;
          std::filesystem::path innerPath;
          if (archive_file::splitVirtualPath(path, archivePath, innerPath)) {
            chart->Meta.Folder = archive_file::makeVirtualPath(
                archivePath, innerPath.parent_path());
          }
        }
      } else {
        archive_file::parseChart(parser, path, &rawChart, false, true,
                                 cancelled);
        chart.reset(rawChart);
        rawChart = nullptr;
      }
    } catch (const std::exception &e) {
      if (chart == nullptr && rawChart != nullptr) {
        chart.reset(rawChart);
        rawChart = nullptr;
      }
      SDL_Log("Error parsing %s: %s", chartText.c_str(), e.what());
      archive_file::appendDebugLogLine("DB parse failed: " + chartText + ": " +
                                       e.what());
      return std::nullopt;
    }

    if (chart == nullptr) {
      archive_file::appendDebugLogLine("DB parse returned null: " + chartText);
      return std::nullopt;
    }
    if (!parsedChartMetaLooksInsertable(chart->Meta)) {
      SDL_Log("Skipping chart without notes or stable identity: %s",
              chartText.c_str());
      archive_file::appendDebugLogLine(
          "DB skipped chart: " + chartText +
          " notes=" + std::to_string(chart->Meta.TotalNotes) +
          " landmines=" + std::to_string(chart->Meta.TotalLandmineNotes) +
          " md5=" + chart->Meta.MD5 + " sha256=" + chart->Meta.SHA256);
      return std::nullopt;
    }
    return chart->Meta;
  };

  struct ArchiveParseBatch {
    std::filesystem::path archivePath;
    std::vector<std::filesystem::path> innerPaths;
    std::vector<bool> parseAttempted;
    std::vector<std::optional<bms_parser::ChartMeta>> preparedMetas;
  };

  struct ArchiveParsedChart {
    std::filesystem::path innerPath;
    std::filesystem::path chartPath;
    std::optional<bms_parser::ChartMeta> meta;
  };

  struct ArchiveParseJobResult {
    std::size_t archiveIndex = 0;
    std::size_t innerStart = 0;
    std::vector<std::filesystem::path> pendingInnerPaths;
    std::optional<std::vector<ArchiveParsedChart>> parsedCharts;
    bool complete = false;
    bool incremental = false;
    std::string errorMessage;
  };

  struct ArchiveParseJobState {
    std::map<std::size_t, std::vector<ArchiveParsedChart>> chunks;
    std::optional<ArchiveParseJobResult> terminalResult;
  };

  struct ArchiveParseJobEvent {
    std::vector<ArchiveParsedChart> parsedCharts;
    std::optional<ArchiveParseJobResult> terminalResult;
  };

  std::mutex archiveParseResultMutex;
  std::condition_variable archiveParseResultCv;

  auto storeArchiveParseStateResult =
      [&](const std::shared_ptr<ArchiveParseJobState> &state,
          ArchiveParseJobResult result) {
        {
          std::lock_guard lock(archiveParseResultMutex);
          state->terminalResult = std::move(result);
        }
        archiveParseResultCv.notify_all();
      };
  auto storeArchiveParseStateChunk =
      [&](const std::shared_ptr<ArchiveParseJobState> &state,
          std::size_t firstInnerIndex,
          std::vector<ArchiveParsedChart> parsedCharts) {
        {
          std::lock_guard lock(archiveParseResultMutex);
          state->chunks.emplace(firstInnerIndex, std::move(parsedCharts));
        }
        archiveParseResultCv.notify_all();
      };

  struct PreparedOrdinaryChart {
    std::filesystem::path path;
    bool parseAttempted = false;
    std::optional<bms_parser::ChartMeta> meta;
  };
  struct PreparedArchive {
    std::filesystem::path path;
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    const ArchiveScanCacheRecord *cache = nullptr;
    bool cacheAccepted = false;
    std::optional<ArchiveScanResult> scan;
    std::string chartParseError;
  };
  using PreparedEntity = std::variant<PreparedOrdinaryChart, PreparedArchive>;

  std::mutex preparedEntityMutex;
  std::condition_variable preparedEntityCv;
  std::vector<std::optional<PreparedEntity>> preparedEntities;
  std::unordered_set<path_t> discoveredOrdinaryChartPaths;
  const std::size_t entityWorkerCount =
      chart_scan::recommendedWorkerCount(kIndividualParseBatchSize);
  const std::size_t archiveIoLimit =
      entityWorkerCount > 1 ? entityWorkerCount - 1 : std::size_t{1};

  auto reservePreparedEntity = [&]() {
    std::lock_guard lock(preparedEntityMutex);
    const std::size_t sequence = preparedEntities.size();
    preparedEntities.emplace_back(std::nullopt);
    return sequence;
  };

  auto storePreparedEntity = [&](std::size_t sequence,
                                 PreparedEntity prepared) {
    {
      std::lock_guard lock(preparedEntityMutex);
      if (sequence < preparedEntities.size()) {
        preparedEntities[sequence] = std::move(prepared);
      }
    }
    preparedEntityCv.notify_all();
  };
  auto storePreparedEntityIfMissing = [&](std::size_t sequence,
                                          PreparedEntity prepared) {
    {
      std::lock_guard lock(preparedEntityMutex);
      if (sequence < preparedEntities.size() &&
          !preparedEntities[sequence].has_value()) {
        preparedEntities[sequence] = std::move(prepared);
      }
    }
    preparedEntityCv.notify_all();
  };

  chart_scan::WorkScheduler entityScheduler(entityWorkerCount, archiveIoLimit);
  int scheduledArchiveIndexCount = 0;

  struct IndexedArchivePrefetch {
    path_t archiveKey;
    std::filesystem::path archivePath;
    std::vector<std::filesystem::path> innerPaths;
    std::shared_ptr<ArchiveParseJobState> state;
  };
  std::mutex indexedArchivePrefetchMutex;
  std::optional<IndexedArchivePrefetch> heldLargeArchivePrefetch;
  bool multiArchivePrefetchActive = false;
  std::unordered_map<path_t, std::shared_ptr<ArchiveParseJobState>>
      prefetchedArchiveStates;

  struct ArchiveChartPipelineResult {
    std::vector<std::filesystem::path> innerPaths;
    std::optional<std::vector<ArchiveParsedChart>> parsedCharts;
    bool complete = false;
    std::string errorMessage;
  };

  using ArchiveChartPipelineCompletion =
      std::function<void(ArchiveChartPipelineResult)>;
  using ArchiveChartPipelineChunkCompletion =
      std::function<void(std::size_t, std::vector<ArchiveParsedChart>)>;

  struct ArchiveChartPipelineState {
    std::mutex mutex;
    std::condition_variable spaceCv;
    std::filesystem::path archivePath;
    std::vector<std::filesystem::path> innerPaths;
    std::vector<std::optional<ArchiveParsedChart>> parsedSlots;
    ArchiveChartPipelineCompletion completion;
    ArchiveChartPipelineChunkCompletion chunkCompletion;
    std::size_t nextPublished = 0;
    std::size_t pendingTasks = 0;
    std::size_t inFlightFiles = 0;
    std::uint64_t inFlightBytes = 0;
    bool readDone = false;
    bool readOk = false;
    bool published = false;
    std::string errorMessage;
  };

  auto runArchiveChartPipeline = [&](chart_scan::WorkScheduler &scheduler,
                                     std::filesystem::path archivePath,
                                     std::vector<std::filesystem::path>
                                         innerPaths,
                                     ArchiveChartPipelineCompletion completion,
                                     ArchiveChartPipelineChunkCompletion
                                         chunkCompletion) {
    if (innerPaths.empty()) {
      completion({
          .innerPaths = {},
          .parsedCharts = std::vector<ArchiveParsedChart>{},
          .complete = true,
      });
      return;
    }

    std::unordered_map<std::string, std::size_t> sequenceByInnerPath;
    sequenceByInnerPath.reserve(innerPaths.size());
    for (std::size_t index = 0; index < innerPaths.size(); ++index) {
      sequenceByInnerPath.emplace(checkpointInnerPathText(innerPaths[index]),
                                  index);
    }

    if (innerPaths.size() == 1 || entityWorkerCount <= 1) {
      ArchiveChartPipelineResult immediateResult;
      std::vector<std::optional<ArchiveParsedChart>> parsedSlots(
          innerPaths.size());
      const bool readOk = archive_file::readArchiveEntriesStreaming(
          archivePath, innerPaths,
          [&](archive_file::FileData &&file) {
            const auto sequenceIt =
                sequenceByInnerPath.find(checkpointInnerPathText(file.path));
            if (sequenceIt == sequenceByInnerPath.end()) {
              immediateResult.errorMessage =
                  "Archive entry was not in the requested chart batch.";
              return false;
            }
            ArchiveParsedChart parsed{
                .innerPath = file.path,
                .chartPath =
                    archive_file::makeVirtualPath(archivePath, file.path),
            };
            try {
              parsed.meta = parseChartMeta(parsed.chartPath, &file.bytes);
            } catch (const std::exception &e) {
              immediateResult.errorMessage = e.what();
            } catch (...) {
              immediateResult.errorMessage =
                  "Unknown archive chart parse error.";
            }
            parsedSlots[sequenceIt->second] = std::move(parsed);
            return !shouldStop();
          },
          &immediateResult.errorMessage, pauseCallback);
      const bool complete =
          readOk && !shouldStop() &&
          std::all_of(parsedSlots.begin(), parsedSlots.end(),
                      [](const auto &slot) { return slot.has_value(); });
      if (complete) {
        std::vector<ArchiveParsedChart> parsedCharts;
        parsedCharts.reserve(parsedSlots.size());
        for (auto &slot : parsedSlots) {
          parsedCharts.push_back(std::move(*slot));
        }
        if (chunkCompletion) {
          chunkCompletion(0, std::move(parsedCharts));
        } else {
          immediateResult.parsedCharts = std::move(parsedCharts);
        }
      }
      immediateResult.complete = complete;
      immediateResult.innerPaths = std::move(innerPaths);
      completion(std::move(immediateResult));
      return;
    }

    auto state = std::make_shared<ArchiveChartPipelineState>();
    state->archivePath = std::move(archivePath);
    state->innerPaths = std::move(innerPaths);
    state->parsedSlots.resize(state->innerPaths.size());
    state->completion = std::move(completion);
    state->chunkCompletion = std::move(chunkCompletion);

    auto publishReady = [&, state]() {
      std::optional<std::pair<std::size_t, std::vector<ArchiveParsedChart>>>
          readyChunk;
      ArchiveChartPipelineChunkCompletion publishChunk;
      std::optional<ArchiveChartPipelineResult> completed;
      ArchiveChartPipelineCompletion publish;
      {
        std::lock_guard lock(state->mutex);
        const bool terminal = state->readDone && state->pendingTasks == 0;
        if (state->chunkCompletion) {
          std::size_t readyEnd = state->nextPublished;
          while (readyEnd < state->parsedSlots.size() &&
                 state->parsedSlots[readyEnd].has_value()) {
            ++readyEnd;
          }
          const std::size_t readyCount = readyEnd - state->nextPublished;
          if (readyCount >= kArchiveParseResultChunkSize ||
              (terminal && readyCount > 0)) {
            std::vector<ArchiveParsedChart> parsedCharts;
            parsedCharts.reserve(readyCount);
            const std::size_t firstIndex = state->nextPublished;
            for (std::size_t index = firstIndex; index < readyEnd; ++index) {
              parsedCharts.push_back(std::move(*state->parsedSlots[index]));
              state->parsedSlots[index].reset();
            }
            state->nextPublished = readyEnd;
            readyChunk.emplace(firstIndex, std::move(parsedCharts));
            publishChunk = state->chunkCompletion;
          }
        }

        if (!state->published && terminal) {
          state->published = true;
          const bool complete =
              state->readOk && !shouldStop() &&
              (state->chunkCompletion
                   ? state->nextPublished == state->parsedSlots.size()
                   : std::all_of(
                         state->parsedSlots.begin(), state->parsedSlots.end(),
                         [](const auto &slot) { return slot.has_value(); }));
          ArchiveChartPipelineResult result{
              .innerPaths = std::move(state->innerPaths),
              .complete = complete,
              .errorMessage = std::move(state->errorMessage),
          };
          if (complete && !state->chunkCompletion) {
            std::vector<ArchiveParsedChart> parsedCharts;
            parsedCharts.reserve(state->parsedSlots.size());
            for (auto &slot : state->parsedSlots) {
              parsedCharts.push_back(std::move(*slot));
            }
            result.parsedCharts = std::move(parsedCharts);
          }
          completed = std::move(result);
          publish = std::move(state->completion);
        }
      }
      if (readyChunk.has_value()) {
        publishChunk(readyChunk->first, std::move(readyChunk->second));
      }
      if (completed.has_value()) {
        publish(std::move(*completed));
      }
    };

    std::string readError;
    const bool readOk = archive_file::readArchiveEntriesStreaming(
        state->archivePath, state->innerPaths,
        [&, state, publishReady](archive_file::FileData &&file) mutable {
          const auto sequenceIt =
              sequenceByInnerPath.find(checkpointInnerPathText(file.path));
          if (sequenceIt == sequenceByInnerPath.end()) {
            std::lock_guard lock(state->mutex);
            state->errorMessage =
                "Archive entry was not in the requested chart batch.";
            return false;
          }

          const std::size_t chartIndex = sequenceIt->second;
          const std::uint64_t fileBytes =
              static_cast<std::uint64_t>(file.bytes.size());
          {
            std::unique_lock lock(state->mutex);
            while (!shouldStop()) {
              const bool fileSlotAvailable =
                  state->inFlightFiles < kArchiveParseMaxInFlightFiles;
              const bool byteSlotAvailable = state->inFlightFiles == 0 ||
                                             state->inFlightBytes + fileBytes <=
                                                 kArchiveParseMaxInFlightBytes;
              if (fileSlotAvailable && byteSlotAvailable) {
                break;
              }
              state->spaceCv.wait_for(lock, std::chrono::milliseconds(20));
            }
            if (shouldStop()) {
              return false;
            }
            ++state->pendingTasks;
            ++state->inFlightFiles;
            state->inFlightBytes += fileBytes;
          }

          const bool accepted =
              scheduler.enqueue([&, state, publishReady, chartIndex, fileBytes,
                                 file = std::move(file)]() mutable {
                ArchiveParsedChart parsed{
                    .innerPath = file.path,
                    .chartPath = archive_file::makeVirtualPath(
                        state->archivePath, file.path),
                };
                try {
                  if (!shouldStop()) {
                    parsed.meta = parseChartMeta(parsed.chartPath, &file.bytes);
                  }
                } catch (const std::exception &e) {
                  std::lock_guard lock(state->mutex);
                  state->errorMessage = e.what();
                } catch (...) {
                  std::lock_guard lock(state->mutex);
                  state->errorMessage = "Unknown chart parse error.";
                }

                {
                  std::lock_guard lock(state->mutex);
                  state->parsedSlots[chartIndex] = std::move(parsed);
                  if (state->pendingTasks > 0) {
                    --state->pendingTasks;
                  }
                  if (state->inFlightFiles > 0) {
                    --state->inFlightFiles;
                  }
                  state->inFlightBytes = fileBytes > state->inFlightBytes
                                             ? 0
                                             : state->inFlightBytes - fileBytes;
                }
                state->spaceCv.notify_all();
                publishReady();
              });
          if (!accepted) {
            std::lock_guard lock(state->mutex);
            if (state->pendingTasks > 0) {
              --state->pendingTasks;
            }
            if (state->inFlightFiles > 0) {
              --state->inFlightFiles;
            }
            state->inFlightBytes = fileBytes > state->inFlightBytes
                                       ? 0
                                       : state->inFlightBytes - fileBytes;
            state->errorMessage =
                "Chart scan scheduler rejected an archive entry.";
            state->spaceCv.notify_all();
            return false;
          }
          return true;
        },
        &readError, pauseCallback);
    {
      std::lock_guard lock(state->mutex);
      state->readOk = readOk;
      state->readDone = true;
      if (!readError.empty()) {
        state->errorMessage = std::move(readError);
      }
    }
    publishReady();
  };

  auto activateIndexedArchivePrefetch = [&](IndexedArchivePrefetch prefetch) {
    prefetch.state = std::make_shared<ArchiveParseJobState>();
    prefetchedArchiveStates[prefetch.archiveKey] = prefetch.state;
    return prefetch;
  };

  auto runIndexedArchivePrefetch = [&](IndexedArchivePrefetch prefetch) {
    archive_file::appendDebugLogLine(
        "Prefetching indexed archive chart batch: " +
        fspath_to_utf8(prefetch.archivePath) +
        " requested=" + std::to_string(prefetch.innerPaths.size()));
    auto state = prefetch.state;
    std::vector<std::filesystem::path> errorPaths = prefetch.innerPaths;
    try {
      runArchiveChartPipeline(
          entityScheduler, std::move(prefetch.archivePath),
          std::move(prefetch.innerPaths),
          [&, state](ArchiveChartPipelineResult result) {
            storeArchiveParseStateResult(
                state, ArchiveParseJobResult{
                           .innerStart = 0,
                           .pendingInnerPaths = std::move(result.innerPaths),
                           .parsedCharts = std::move(result.parsedCharts),
                           .complete = result.complete,
                           .incremental = true,
                           .errorMessage = std::move(result.errorMessage),
                       });
          },
          [&, state](std::size_t firstIndex,
                     std::vector<ArchiveParsedChart> parsedCharts) {
            storeArchiveParseStateChunk(state, firstIndex,
                                        std::move(parsedCharts));
          });
    } catch (const std::exception &error) {
      storeArchiveParseStateResult(
          state, ArchiveParseJobResult{
                     .innerStart = 0,
                     .pendingInnerPaths = std::move(errorPaths),
                     .incremental = true,
                     .errorMessage = error.what(),
                 });
      throw;
    } catch (...) {
      storeArchiveParseStateResult(
          state, ArchiveParseJobResult{
                     .innerStart = 0,
                     .pendingInnerPaths = std::move(errorPaths),
                     .incremental = true,
                     .errorMessage = "Unknown indexed archive parse error.",
                 });
      throw;
    }
  };

  auto queueIndexedArchivePrefetch = [&](IndexedArchivePrefetch prefetch) {
    auto queuedPrefetch =
        std::make_shared<IndexedArchivePrefetch>(std::move(prefetch));
    const auto workClass =
        queuedPrefetch->innerPaths.size() >= kArchiveDirectConcurrentMinCharts
            ? chart_scan::WorkClass::ArchiveReadHeavy
            : chart_scan::WorkClass::ArchiveRead;
    if (entityScheduler.enqueue(
            [&, queuedPrefetch] {
              runIndexedArchivePrefetch(std::move(*queuedPrefetch));
            },
            workClass)) {
      return;
    }
    storeArchiveParseStateResult(
        queuedPrefetch->state,
        ArchiveParseJobResult{
            .innerStart = 0,
            .pendingInnerPaths = std::move(queuedPrefetch->innerPaths),
            .incremental = true,
            .errorMessage = "Indexed archive prefetch was not queued.",
        });
  };

  auto prepareArchiveCharts =
      [&](std::size_t sequence,
          std::shared_ptr<PreparedArchive> preparedArchive) {
        if (!preparedArchive->scan.has_value() ||
            preparedArchive->scan->solid ||
            preparedArchive->scan->chartPaths.empty() || checkpoint.found) {
          storePreparedEntity(sequence, std::move(*preparedArchive));
          return;
        }
        std::vector<std::filesystem::path> innerPaths;
        innerPaths.reserve(preparedArchive->scan->chartPaths.size());
        for (const auto &chartPath : preparedArchive->scan->chartPaths) {
          std::filesystem::path parsedArchivePath;
          std::filesystem::path innerPath;
          if (!archive_file::splitVirtualPath(chartPath, parsedArchivePath,
                                              innerPath)) {
            preparedArchive->chartParseError =
                "Indexed archive chart path was not virtual.";
            storePreparedEntity(sequence, std::move(*preparedArchive));
            return;
          }
          innerPaths.push_back(std::move(innerPath));
        }

        IndexedArchivePrefetch current{
            .archiveKey = archiveScanKey(preparedArchive->path),
            .archivePath = preparedArchive->path,
            .innerPaths = std::move(innerPaths),
        };
        if (entityWorkerCount > 1 && preparedArchive->scan->chartPaths.size() >=
                                         kArchiveDirectConcurrentMinCharts) {
          {
            std::lock_guard lock(indexedArchivePrefetchMutex);
            if (!multiArchivePrefetchActive &&
                !heldLargeArchivePrefetch.has_value()) {
              heldLargeArchivePrefetch = std::move(current);
            } else {
              if (!multiArchivePrefetchActive) {
                multiArchivePrefetchActive = true;
                queueIndexedArchivePrefetch(activateIndexedArchivePrefetch(
                    std::move(*heldLargeArchivePrefetch)));
                heldLargeArchivePrefetch.reset();
              }
              queueIndexedArchivePrefetch(
                  activateIndexedArchivePrefetch(std::move(current)));
            }
          }

          storePreparedEntity(sequence, std::move(*preparedArchive));
          return;
        }

        {
          std::lock_guard lock(indexedArchivePrefetchMutex);
          queueIndexedArchivePrefetch(
              activateIndexedArchivePrefetch(std::move(current)));
        }
        storePreparedEntity(sequence, std::move(*preparedArchive));
      };

  auto scheduleOrdinaryChart = [&](const std::filesystem::path &path) {
    const path_t key = fspath_to_path_t(path);
    if (knownChartPaths.contains(key) ||
        !discoveredOrdinaryChartPaths.insert(key).second) {
      return;
    }

    const std::size_t sequence = reservePreparedEntity();
    if (checkpoint.found) {
      storePreparedEntity(sequence, PreparedOrdinaryChart{.path = path});
      return;
    }

    if (!entityScheduler.enqueue([&, sequence, path] {
          if (shouldStop()) {
            return;
          }
          try {
            PreparedOrdinaryChart prepared{
                .path = path,
                .parseAttempted = true,
                .meta = parseChartMeta(path, nullptr),
            };
            storePreparedEntity(sequence, std::move(prepared));
          } catch (...) {
            storePreparedEntityIfMissing(sequence,
                                         PreparedOrdinaryChart{.path = path});
            throw;
          }
        })) {
      storePreparedEntity(sequence, PreparedOrdinaryChart{.path = path});
    }
  };

  auto scheduleArchivePath = [&](const std::filesystem::path &archivePath) {
    if (shouldStop()) {
      return;
    }
    const path_t archiveKey = archiveScanKey(archivePath);
    if (scannedArchivePaths.find(archiveKey) != scannedArchivePaths.end()) {
      return;
    }
    scannedArchivePaths.insert(archiveKey);

    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    if (!archiveFileState(archivePath, archiveSize, mtimeNs)) {
      return;
    }

    const std::string archiveText = fspath_to_utf8(archivePath);
    const auto cacheIt = archiveCacheByPath.find(archiveKey);
    const ArchiveScanCacheRecord *cache =
        cacheIt != archiveCacheByPath.end() ? cacheIt->second : nullptr;
    if (cache != nullptr && cache->archiveSize == archiveSize &&
        cache->mtimeNs == mtimeNs && cache->chartCount >= 0) {
      int knownChartCount = knownArchiveChartCounts.contains(archiveKey)
                                ? knownArchiveChartCounts.at(archiveKey)
                                : 0;
      const int storedChartCount = storedArchiveChartCounts.contains(archiveKey)
                                       ? storedArchiveChartCounts.at(archiveKey)
                                       : 0;
      if (!cache->solid && knownChartCount < cache->chartCount &&
          storedChartCount > knownChartCount) {
        archive_file::appendDebugLogLine(
            "Recovered archive DB chart count by stored path prefix: " +
            archiveText + " splitRows=" + std::to_string(knownChartCount) +
            " storedRows=" + std::to_string(storedChartCount) +
            " cachedCharts=" + std::to_string(cache->chartCount));
        knownChartCount = storedChartCount;
      }
      if (!cache->solid && knownChartCount < cache->chartCount) {
        archive_file::appendDebugLogLine(
            "Archive scan cache incomplete; rescanning: " + archiveText +
            " cachedCharts=" + std::to_string(cache->chartCount) +
            " dbCharts=" + std::to_string(knownChartCount));
      } else {
        const std::size_t sequence = reservePreparedEntity();
        storePreparedEntity(sequence, PreparedArchive{
                                          .path = archivePath,
                                          .archiveSize = archiveSize,
                                          .mtimeNs = mtimeNs,
                                          .cache = cache,
                                          .cacheAccepted = true,
                                      });
        return;
      }
    }
    if (cache != nullptr) {
      archive_file::appendDebugLogLine("Archive scan cache invalidated: " +
                                       archiveText);
    }

    const std::size_t sequence = reservePreparedEntity();
    if (entityScheduler.enqueue(
            [&, sequence, archivePath, archiveSize, mtimeNs, cache] {
              if (shouldStop()) {
                return;
              }
              try {
                auto prepared =
                    std::make_shared<PreparedArchive>(PreparedArchive{
                        .path = archivePath,
                        .archiveSize = archiveSize,
                        .mtimeNs = mtimeNs,
                        .cache = cache,
                        .scan = scanArchiveForChartsOrSolid(archivePath,
                                                            pauseCallback),
                    });
                prepareArchiveCharts(sequence, std::move(prepared));
              } catch (...) {
                storePreparedEntityIfMissing(sequence,
                                             PreparedArchive{
                                                 .path = archivePath,
                                                 .archiveSize = archiveSize,
                                                 .mtimeNs = mtimeNs,
                                                 .cache = cache,
                                             });
                throw;
              }
            },
            chart_scan::WorkClass::ArchiveIndex)) {
      ++scheduledArchiveIndexCount;
    } else {
      storePreparedEntity(sequence, PreparedArchive{
                                        .path = archivePath,
                                        .archiveSize = archiveSize,
                                        .mtimeNs = mtimeNs,
                                        .cache = cache,
                                    });
    }
  };

  int scannedRootCount = 0;
  const int rootCount =
      static_cast<int>(std::max<std::size_t>(roots.size(), 1));
  for (const auto &root : roots) {
    if (shouldStop() || root.empty()) {
      continue;
    }
    reportProgress(scannedRootCount, rootCount,
                   ChartScanProgressStage::ScanningRoots);
#if TARGET_OS_ANDROID
    if (IsAndroidTreePath(root)) {
      std::vector<std::filesystem::path> androidChartPaths;
      std::string androidError;
      if (!ListAndroidTreeChartFiles(root, androidChartPaths, androidError,
                                     stopToken)) {
        if (!androidError.empty()) {
          SDL_Log("Failed while scanning Android chart folder %s: %s",
                  fspath_to_utf8(root).c_str(), androidError.c_str());
        }
        ++scannedRootCount;
        continue;
      }
      for (const auto &path : androidChartPaths) {
        if (shouldStop()) {
          entityScheduler.cancel();
          return 0;
        }
        if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
          scheduleOrdinaryChart(path);
          continue;
        }
        if (archive_file::hasSupportedArchiveExtension(path)) {
          archive_file::appendDebugLogLine(
              "Skipping Android SAF archive during library scan: " +
              fspath_to_utf8(path));
        }
      }
      ++scannedRootCount;
      continue;
    }
#endif
    std::error_code error;
    const bool rootExists = std::filesystem::exists(root, error);
    if (error) {
      SDL_Log("Failed to check chart folder %s: %s",
              fspath_to_utf8(root).c_str(), error.message().c_str());
      ++scannedRootCount;
      continue;
    }
    if (!rootExists) {
      ++scannedRootCount;
      continue;
    }

    std::error_code rootTypeError;
    if (std::filesystem::is_regular_file(root, rootTypeError) &&
        !rootTypeError) {
      if (asobmshow::bms_chart_file::isBmsChartPath(root)) {
        scheduleOrdinaryChart(root);
      } else if (archive_file::hasSupportedArchiveExtension(root)) {
        scheduleArchivePath(root);
      }
      ++scannedRootCount;
      continue;
    }

    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied,
        error);
    for (const auto end = std::filesystem::recursive_directory_iterator();
         !error && iterator != end; iterator.increment(error)) {
      if (shouldStop()) {
        entityScheduler.cancel();
        return 0;
      }
      std::error_code typeError;
      if (!iterator->is_regular_file(typeError) || typeError) {
        continue;
      }
      const std::filesystem::path path = iterator->path();
      if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
        scheduleOrdinaryChart(path);
        continue;
      }
      if (archive_file::hasSupportedArchiveExtension(path)) {
        scheduleArchivePath(path);
      }
    }
    if (error) {
      SDL_Log("Failed while scanning chart folder %s: %s",
              fspath_to_utf8(root).c_str(), error.message().c_str());
    }
    ++scannedRootCount;
  }
  if (scheduledArchiveIndexCount > 0) {
    reportProgress(0, scheduledArchiveIndexCount,
                   ChartScanProgressStage::IndexingArchives);
  }
  if (shouldStop()) {
    entityScheduler.cancel();
  } else {
    std::unique_lock lock(preparedEntityMutex);
    while (!shouldStop() &&
           !std::all_of(preparedEntities.begin(), preparedEntities.end(),
                        [](const auto &slot) { return slot.has_value(); })) {
      preparedEntityCv.wait_for(lock, std::chrono::milliseconds(20));
    }
    lock.unlock();
    if (!shouldStop() && scheduledArchiveIndexCount > 0) {
      reportProgress(scheduledArchiveIndexCount, scheduledArchiveIndexCount,
                     ChartScanProgressStage::IndexingArchives);
    }
    if (shouldStop()) {
      entityScheduler.cancel();
    }
  }

  for (auto &preparedSlot : preparedEntities) {
    if (!preparedSlot.has_value()) {
      continue;
    }
    if (auto *ordinary = std::get_if<PreparedOrdinaryChart>(&*preparedSlot)) {
      knownChartPaths.insert(fspath_to_path_t(ordinary->path));
      diffs.push_back({
          .path = std::move(ordinary->path),
          .deleted = false,
          .parseAttempted = ordinary->parseAttempted,
          .preparedMeta = std::move(ordinary->meta),
      });
      continue;
    }

    auto &prepared = std::get<PreparedArchive>(*preparedSlot);
    const path_t archiveKey = archiveScanKey(prepared.path);
    if (!prepared.scan.has_value()) {
      if (prepared.cacheAccepted && prepared.cache != nullptr &&
          prepared.cache->archiveSize == prepared.archiveSize &&
          prepared.cache->mtimeNs == prepared.mtimeNs &&
          prepared.cache->chartCount >= 0) {
        archive_file::appendDebugLogLine(
            "Using cached archive scan: " + fspath_to_utf8(prepared.path) +
            " files=" + std::to_string(prepared.cache->fileCount) +
            " charts=" + std::to_string(prepared.cache->chartCount) +
            " solid=" + std::string(prepared.cache->solid ? "yes" : "no") +
            " estimatedUnpacked=" +
            std::to_string(prepared.cache->uncompressedSize));
        solidArchiveDiffs.push_back({
            .path = prepared.path,
            .solid = prepared.cache->solid,
            .uncompressedSize = prepared.cache->uncompressedSize,
            .fileCount = prepared.cache->fileCount,
        });
      }
      continue;
    }

    ArchiveScanResult &archiveScan = *prepared.scan;
    if (!archiveScan.readable) {
      continue;
    }
    if (!prepared.chartParseError.empty()) {
      archive_file::appendDebugLogLine(
          "Deferred archive chart parse after pipeline error: " +
          fspath_to_utf8(prepared.path) + ": " + prepared.chartParseError);
    }
    reindexedArchives.push_back(prepared.path);
    ArchiveCacheDiff cacheDiff{
        .path = prepared.path,
        .solid = archiveScan.solid,
        .uncompressedSize = archiveScan.uncompressedSize,
        .fileCount = archiveScan.fileCount,
        .chartCount = static_cast<int>(archiveScan.chartPaths.size()),
    };
    if (archiveScan.solid) {
      archiveCacheDiffs.push_back(cacheDiff);
      solidArchiveDiffs.push_back({
          .path = prepared.path,
          .solid = true,
          .uncompressedSize = archiveScan.uncompressedSize,
          .fileCount = archiveScan.fileCount,
      });
      continue;
    }

    solidArchiveDiffs.push_back({
        .path = prepared.path,
        .solid = false,
    });
    if (archiveScan.chartPaths.empty()) {
      archiveCacheDiffs.push_back(cacheDiff);
    } else {
      pendingArchiveCacheDiffs[archiveKey] = cacheDiff;
    }
    for (std::size_t chartIndex = 0; chartIndex < archiveScan.chartPaths.size();
         ++chartIndex) {
      auto &chartPath = archiveScan.chartPaths[chartIndex];
      knownChartPaths.insert(fspath_to_path_t(chartPath));
      ScanDiff diff{
          .path = std::move(chartPath),
          .deleted = false,
      };
      diffs.push_back(std::move(diff));
    }
  }

  reportProgress(rootCount, rootCount,
                 ChartScanProgressStage::PreparingUpdates);

  const bool noScanWork =
      diffs.empty() && sourcePreferenceRefreshPaths.empty() &&
      cachedSourcePreferenceUpdates.empty() && solidArchiveDiffs.empty() &&
      archiveCacheDiffs.empty() && pendingArchiveCacheDiffs.empty() &&
      staleSolidArchives.empty() && reindexedArchives.empty();
  if (noScanWork) {
    entityScheduler.finish();
    session.ClearScanCheckpoint();
    session.ClearChartMetadataRebuildRequired();
    return 0;
  }
  if (shouldStop()) {
    return 0;
  }

  std::vector<ScanDiff> individualDiffs;
  std::vector<path_t> archiveBatchOrder;
  std::unordered_map<path_t, ArchiveParseBatch> archiveBatches;
  individualDiffs.reserve(diffs.size());
  for (auto &diff : diffs) {
    if (diff.deleted) {
      individualDiffs.push_back(std::move(diff));
      continue;
    }
    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    if (!archive_file::splitVirtualPath(diff.path, archivePath, innerPath)) {
      individualDiffs.push_back(std::move(diff));
      continue;
    }

    const path_t archiveKey = archiveScanKey(archivePath);
    auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt == archiveBatches.end()) {
      archiveBatchOrder.push_back(archiveKey);
      batchIt = archiveBatches
                    .emplace(archiveKey,
                             ArchiveParseBatch{
                                 .archivePath = archivePath,
                                 .innerPaths = {},
                             })
                    .first;
    }
    batchIt->second.innerPaths.push_back(innerPath);
    batchIt->second.parseAttempted.push_back(diff.parseAttempted);
    batchIt->second.preparedMetas.push_back(std::move(diff.preparedMeta));
  }

  int parseTotal = static_cast<int>(individualDiffs.size());
  for (const auto &archiveKey : archiveBatchOrder) {
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt != archiveBatches.end()) {
      parseTotal += static_cast<int>(batchIt->second.innerPaths.size());
    }
  }
  parseTotal = std::max(parseTotal, 1);
  int parseCurrent = 0;

  auto computeScanSignature = [&](std::size_t individualStart,
                                  std::size_t archiveStart) {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    std::uint64_t hash = kOffset;
    fnv1aAppend(hash, "chart-scan-v1");

    std::vector<std::string> rootKeys;
    rootKeys.reserve(roots.size());
    for (const auto &root : roots) {
      std::string rootKey = checkpointPathTextForDb(root);
      std::int64_t size = 0;
      std::int64_t mtimeNs = 0;
      if (archiveFileState(root, size, mtimeNs)) {
        rootKey += "|file|";
        rootKey += std::to_string(size);
        rootKey += "|";
        rootKey += std::to_string(mtimeNs);
      } else {
        std::error_code error;
        const bool directory = std::filesystem::is_directory(root, error);
        if (error) {
          rootKey += "|unknown";
        } else {
          rootKey += directory ? "|dir" : "|missing";
        }
      }
      rootKeys.push_back(std::move(rootKey));
    }
    std::sort(rootKeys.begin(), rootKeys.end());
    fnv1aAppend(hash, "roots");
    fnv1aAppend(hash, std::to_string(rootKeys.size()));
    for (const auto &rootKey : rootKeys) {
      fnv1aAppend(hash, rootKey);
    }

    fnv1aAppend(hash, "individual");
    individualStart = std::min(individualStart, individualDiffs.size());
    fnv1aAppend(hash, std::to_string(individualDiffs.size() - individualStart));
    for (std::size_t i = individualStart; i < individualDiffs.size(); ++i) {
      const auto &diff = individualDiffs[i];
      fnv1aAppend(hash, diff.deleted ? "d" : "u");
      fnv1aAppend(hash, checkpointPathTextForDb(diff.path));
    }

    fnv1aAppend(hash, "archives");
    archiveStart = std::min(archiveStart, archiveBatchOrder.size());
    fnv1aAppend(hash, std::to_string(archiveBatchOrder.size() - archiveStart));
    for (std::size_t i = archiveStart; i < archiveBatchOrder.size(); ++i) {
      const auto &archiveKey = archiveBatchOrder[i];
      const auto batchIt = archiveBatches.find(archiveKey);
      if (batchIt == archiveBatches.end()) {
        continue;
      }
      const ArchiveParseBatch &batch = batchIt->second;
      fnv1aAppend(hash, checkpointPathTextForDb(batch.archivePath));
      std::int64_t archiveSize = 0;
      std::int64_t mtimeNs = 0;
      if (archiveFileState(batch.archivePath, archiveSize, mtimeNs)) {
        fnv1aAppend(hash, std::to_string(archiveSize));
        fnv1aAppend(hash, std::to_string(mtimeNs));
      } else {
        fnv1aAppend(hash, "missing");
      }
      fnv1aAppend(hash, std::to_string(batch.innerPaths.size()));
      for (const auto &innerPath : batch.innerPaths) {
        fnv1aAppend(hash, checkpointInnerPathText(innerPath));
      }
    }
    return stableHashHex(hash);
  };

  const std::string scanSignature = computeScanSignature(0, 0);
  struct ResumePlan {
    bool valid = false;
    bool archivePhase = false;
    std::size_t individualStart = 0;
    std::size_t archiveStart = 0;
    std::size_t archiveSubStart = 0;
    std::unordered_set<path_t> protectedArchiveKeys;
  };
  ResumePlan resumePlan;

  auto archiveBatchInnerCountBefore = [&](std::size_t archiveIndex) {
    std::size_t count = 0;
    const std::size_t limit = std::min(archiveIndex, archiveBatchOrder.size());
    for (std::size_t i = 0; i < limit; ++i) {
      const auto batchIt = archiveBatches.find(archiveBatchOrder[i]);
      if (batchIt != archiveBatches.end()) {
        count += batchIt->second.innerPaths.size();
      }
    }
    return count;
  };

  auto validateIndividualCheckpoint = [&]() {
    const std::size_t nextIndex =
        static_cast<std::size_t>(checkpoint.nextIndex);
    if (nextIndex > individualDiffs.size() || checkpoint.subIndex != 0) {
      return false;
    }
    if (nextIndex > 0 &&
        checkpointPathTextForDb(individualDiffs[nextIndex - 1].path) !=
            checkpointPathTextForDb(checkpoint.lastPath)) {
      return false;
    }
    resumePlan.valid = true;
    resumePlan.archivePhase = false;
    resumePlan.individualStart = nextIndex;
    parseCurrent = static_cast<int>(
        std::min<std::size_t>(nextIndex, static_cast<std::size_t>(parseTotal)));
    return true;
  };

  auto validateArchiveCheckpoint = [&]() {
    const std::size_t nextIndex =
        static_cast<std::size_t>(checkpoint.nextIndex);
    const std::size_t subIndex = static_cast<std::size_t>(checkpoint.subIndex);
    if (nextIndex > archiveBatchOrder.size()) {
      return false;
    }
    if (nextIndex == 0 && subIndex == 0) {
      resumePlan.valid = true;
      resumePlan.archivePhase = true;
      resumePlan.individualStart = individualDiffs.size();
      resumePlan.archiveStart = 0;
      resumePlan.archiveSubStart = 0;
      parseCurrent = static_cast<int>(std::min<std::size_t>(
          individualDiffs.size(), static_cast<std::size_t>(parseTotal)));
      return true;
    }

    if (subIndex == 0) {
      if (nextIndex == 0) {
        return false;
      }
      const auto previousBatchIt =
          archiveBatches.find(archiveBatchOrder[nextIndex - 1]);
      if (previousBatchIt == archiveBatches.end()) {
        return false;
      }
      const ArchiveParseBatch &previousBatch = previousBatchIt->second;
      if (checkpointPathTextForDb(previousBatch.archivePath) !=
          checkpointPathTextForDb(checkpoint.archivePath)) {
        return false;
      }
      std::int64_t archiveSize = 0;
      std::int64_t mtimeNs = 0;
      if (!archiveFileState(previousBatch.archivePath, archiveSize, mtimeNs) ||
          archiveSize != checkpoint.archiveSize ||
          mtimeNs != checkpoint.archiveMtimeNs) {
        return false;
      }
      if (!previousBatch.innerPaths.empty() &&
          checkpointInnerPathText(previousBatch.innerPaths.back()) !=
              checkpoint.lastInnerPath) {
        return false;
      }
    } else {
      if (nextIndex >= archiveBatchOrder.size()) {
        return false;
      }
      const auto batchIt = archiveBatches.find(archiveBatchOrder[nextIndex]);
      if (batchIt == archiveBatches.end()) {
        return false;
      }
      const ArchiveParseBatch &batch = batchIt->second;
      if (subIndex > batch.innerPaths.size()) {
        return false;
      }
      if (checkpointPathTextForDb(batch.archivePath) !=
          checkpointPathTextForDb(checkpoint.archivePath)) {
        return false;
      }
      std::int64_t archiveSize = 0;
      std::int64_t mtimeNs = 0;
      if (!archiveFileState(batch.archivePath, archiveSize, mtimeNs) ||
          archiveSize != checkpoint.archiveSize ||
          mtimeNs != checkpoint.archiveMtimeNs) {
        return false;
      }
      if (checkpointInnerPathText(batch.innerPaths[subIndex - 1]) !=
          checkpoint.lastInnerPath) {
        return false;
      }
    }

    resumePlan.valid = true;
    resumePlan.archivePhase = true;
    resumePlan.individualStart = individualDiffs.size();
    resumePlan.archiveStart = nextIndex;
    resumePlan.archiveSubStart = subIndex;
    for (std::size_t i = 0; i < nextIndex && i < archiveBatchOrder.size();
         ++i) {
      resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[i]);
    }
    if (subIndex > 0 && nextIndex < archiveBatchOrder.size()) {
      resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[nextIndex]);
    }
    const std::size_t resumedCount = individualDiffs.size() +
                                     archiveBatchInnerCountBefore(nextIndex) +
                                     subIndex;
    parseCurrent = static_cast<int>(std::min<std::size_t>(
        resumedCount, static_cast<std::size_t>(parseTotal)));
    return true;
  };

  if (checkpoint.found && checkpoint.scanSignature == scanSignature &&
      ((checkpoint.phase == kScanCheckpointPhaseIndividual &&
        validateIndividualCheckpoint()) ||
       (checkpoint.phase == kScanCheckpointPhaseArchive &&
        validateArchiveCheckpoint()))) {
    archive_file::appendDebugLogLine(
        "Continuing chart scan from checkpoint: phase=" + checkpoint.phase +
        " nextIndex=" + std::to_string(checkpoint.nextIndex) +
        " subIndex=" + std::to_string(checkpoint.subIndex));
  } else if (checkpoint.found) {
    session.ClearScanCheckpoint();
    archive_file::appendDebugLogLine(
        "Discarded stale chart scan checkpoint before parsing.");
  }

  auto scanBatch = session.BeginScanBatch();
  if (!scanBatch.has_value()) {
    entityScheduler.cancel();
    return 0;
  }
  std::uint64_t completedFlushRequest = 0;

  auto makeCheckpoint = [&](const std::string &signature,
                            const std::string &phase, std::size_t nextIndex,
                            std::size_t subIndex,
                            const std::filesystem::path &lastPath,
                            const std::filesystem::path &archivePath,
                            const std::string &lastInnerPath) {
    ChartScanCheckpoint nextCheckpoint;
    nextCheckpoint.found = true;
    nextCheckpoint.scanSignature = signature;
    nextCheckpoint.phase = phase;
    nextCheckpoint.nextIndex = static_cast<int>(std::min<std::size_t>(
        nextIndex, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    nextCheckpoint.subIndex = static_cast<int>(std::min<std::size_t>(
        subIndex, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    nextCheckpoint.lastPath = lastPath;
    nextCheckpoint.archivePath = archivePath;
    nextCheckpoint.lastInnerPath = lastInnerPath;
    if (!archivePath.empty()) {
      archiveFileState(archivePath, nextCheckpoint.archiveSize,
                       nextCheckpoint.archiveMtimeNs);
    }
    return nextCheckpoint;
  };

  auto saveCheckpoint = [&](const ChartScanCheckpoint &nextCheckpoint) {
    if (!scanBatch->CheckpointAndContinue(nextCheckpoint)) {
      archive_file::appendDebugLogLine(
          "Failed to save chart scan checkpoint: phase=" +
          nextCheckpoint.phase +
          " nextIndex=" + std::to_string(nextCheckpoint.nextIndex) +
          " subIndex=" + std::to_string(nextCheckpoint.subIndex));
    }
  };

  auto pendingFlushRequest = [&]() -> std::uint64_t {
    if (flushRequestCallback == nullptr) {
      return 0;
    }
    return flushRequestCallback();
  };

  auto acknowledgeFlushRequest = [&](std::uint64_t request) {
    if (request == 0 || request <= completedFlushRequest) {
      return;
    }
    completedFlushRequest = request;
    if (flushCompleteCallback != nullptr) {
      flushCompleteCallback(request);
    }
  };

  auto checkpointSaveRequest = [&](bool force) -> std::optional<std::uint64_t> {
    const std::uint64_t request = pendingFlushRequest();
    if (!force && request <= completedFlushRequest) {
      return std::nullopt;
    }
    return request;
  };

  auto saveCheckpointForFlush = [&](const ChartScanCheckpoint &nextCheckpoint,
                                    std::uint64_t request) {
    saveCheckpoint(nextCheckpoint);
    acknowledgeFlushRequest(request);
  };

  auto archiveDeleteProtectedByCheckpoint =
      [&](const std::filesystem::path &archivePath) {
        if (!resumePlan.valid || !resumePlan.archivePhase) {
          return false;
        }
        const path_t archiveKey = archiveScanKey(archivePath);
        return resumePlan.protectedArchiveKeys.find(archiveKey) !=
               resumePlan.protectedArchiveKeys.end();
      };

  for (const auto &path : sourcePreferenceRefreshPaths) {
    if (shouldStop()) {
      break;
    }
    const auto preference = archive_file::sourcePreferenceForPath(path);
    scanBatch->UpdateSourcePreference({
        .path = path,
        .priority = preference.priority,
        .archiveSize = preference.archiveSize,
    });
  }

  for (const auto &update : cachedSourcePreferenceUpdates) {
    if (shouldStop()) {
      break;
    }
    scanBatch->UpdateSourcePreference({
        .path = update.path,
        .priority = update.priority,
        .archiveSize = update.archiveSize,
    });
  }

  for (const auto &path : staleSolidArchives) {
    if (shouldStop()) {
      break;
    }
    scanBatch->DeleteSolidArchive(path);
    scanBatch->DeleteArchiveCache(path);
  }

  for (const auto &path : reindexedArchives) {
    if (shouldStop()) {
      break;
    }
    if (archiveDeleteProtectedByCheckpoint(path)) {
      archive_file::appendDebugLogLine(
          "Skipping archive chart delete for checkpoint-protected archive: " +
          checkpointPathTextForDb(path));
      continue;
    }
    scanBatch->DeleteChartsInArchive(path);
  }

  for (const auto &diff : archiveCacheDiffs) {
    if (shouldStop()) {
      break;
    }
    scanBatch->UpsertArchiveCache({
        .path = diff.path,
        .solid = diff.solid,
        .uncompressedSize = diff.uncompressedSize,
        .fileCount = diff.fileCount,
        .chartCount = diff.chartCount,
    });
  }

  for (const auto &diff : solidArchiveDiffs) {
    if (shouldStop()) {
      break;
    }
    if (diff.solid) {
      scanBatch->UpsertSolidArchive({
          .path = diff.path,
          .uncompressedSize = diff.uncompressedSize,
          .fileCount = diff.fileCount,
      });
      scanBatch->DeleteChartsInArchive(diff.path);
    } else {
      scanBatch->DeleteSolidArchive(diff.path);
    }
  }

  auto insertIndividualChartMeta = [&](bms_parser::ChartMeta &meta) -> bool {
    return scanBatch->UpsertChart(meta, std::nullopt);
  };

  auto individualParseWorkerCount = [](std::size_t fileCount) {
    return static_cast<std::size_t>(parallel_worker_count(fileCount));
  };

  auto parseIndividualChartBatch = [&](std::size_t begin, std::size_t end)
      -> std::vector<std::optional<bms_parser::ChartMeta>> {
    using Clock = std::chrono::steady_clock;
    const std::size_t count = end > begin ? end - begin : 0;
    std::vector<std::optional<bms_parser::ChartMeta>> parsedMetas(count);
    if (count == 0) {
      return parsedMetas;
    }

    const std::size_t requiredParseCount =
        static_cast<std::size_t>(std::count_if(
            individualDiffs.begin() +
                static_cast<std::vector<ScanDiff>::difference_type>(begin),
            individualDiffs.begin() +
                static_cast<std::vector<ScanDiff>::difference_type>(end),
            [](const ScanDiff &diff) { return !diff.parseAttempted; }));
    const std::size_t workerCount = std::max<std::size_t>(
        1, individualParseWorkerCount(requiredParseCount));
    const auto parseStart = Clock::now();
    if (workerCount > 1) {
      archive_file::appendDebugLogLine(
          "Starting concurrent DB individual chart parse: files=" +
          std::to_string(count) + " workers=" + std::to_string(workerCount));
    }

    auto parseOne = [&](std::size_t offset) {
      if (shouldStop()) {
        return;
      }
      ScanDiff &diff = individualDiffs[begin + offset];
      if (diff.parseAttempted) {
        parsedMetas[offset] = std::move(diff.preparedMeta);
      } else {
        parsedMetas[offset] = parseChartMeta(diff.path, nullptr);
      }
    };

    if (workerCount <= 1) {
      for (std::size_t offset = 0; offset < count; ++offset) {
        parseOne(offset);
      }
    } else {
      std::atomic_size_t nextOffset{0};
      std::vector<std::thread> workers;
      workers.reserve(workerCount);
      for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
          for (;;) {
            if (shouldStop()) {
              return;
            }
            const std::size_t offset =
                nextOffset.fetch_add(1, std::memory_order_relaxed);
            if (offset >= count) {
              return;
            }
            parseOne(offset);
          }
        });
      }
      for (auto &worker : workers) {
        if (worker.joinable()) {
          worker.join();
        }
      }
    }

    if (workerCount > 1) {
      const auto parseMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                parseStart)
              .count();
      const std::size_t parsedCount = static_cast<std::size_t>(
          std::count_if(parsedMetas.begin(), parsedMetas.end(),
                        [](const auto &meta) { return meta.has_value(); }));
      archive_file::appendDebugLogLine(
          "Finished concurrent DB individual chart parse: files=" +
          std::to_string(count) + " parsed=" + std::to_string(parsedCount) +
          " workers=" + std::to_string(workerCount) +
          " parseMs=" + std::to_string(parseMs));
    }
    return parsedMetas;
  };

  const std::size_t individualStartIndex =
      resumePlan.valid ? resumePlan.individualStart : 0;
  for (std::size_t diffIndex = individualStartIndex;
       diffIndex < individualDiffs.size();) {
    const auto &diff = individualDiffs[diffIndex];
    if (shouldStop()) {
      break;
    }
    if (diff.deleted) {
      reportProgress(parseCurrent, parseTotal,
                     ChartScanProgressStage::RemovingDeleted);
      scanBatch->DeleteChart(diff.path);
      ++parseCurrent;
      const std::size_t nextIndex = diffIndex + 1;
      const auto checkpointRequest = checkpointSaveRequest(
          nextIndex % kIndividualParseCheckpointInterval == 0);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(
            makeCheckpoint(computeScanSignature(nextIndex, 0),
                           kScanCheckpointPhaseIndividual, 0, 0, {}, {}, ""),
            *checkpointRequest);
      }
      diffIndex = nextIndex;
      continue;
    }

    const std::size_t batchStart = diffIndex;
    std::size_t batchEnd = batchStart;
    while (batchEnd < individualDiffs.size() &&
           !individualDiffs[batchEnd].deleted &&
           batchEnd - batchStart < kIndividualParseBatchSize) {
      ++batchEnd;
    }

    reportProgress(parseCurrent, parseTotal,
                   ChartScanProgressStage::ParsingCharts);
    auto parsedMetas = parseIndividualChartBatch(batchStart, batchEnd);
    for (std::size_t offset = 0; offset < parsedMetas.size(); ++offset) {
      if (shouldStop()) {
        break;
      }
      const std::size_t currentIndex = batchStart + offset;
      reportProgress(parseCurrent, parseTotal,
                     ChartScanProgressStage::ParsingCharts);
      if (parsedMetas[offset].has_value()) {
        insertIndividualChartMeta(*parsedMetas[offset]);
      }
      ++parseCurrent;
      const std::size_t nextIndex = currentIndex + 1;
      const auto checkpointRequest = checkpointSaveRequest(
          nextIndex % kIndividualParseCheckpointInterval == 0);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(
            makeCheckpoint(computeScanSignature(nextIndex, 0),
                           kScanCheckpointPhaseIndividual, 0, 0, {}, {}, ""),
            *checkpointRequest);
      }
    }
    diffIndex = batchEnd;
  }

  if (!shouldStop() && !archiveBatchOrder.empty() &&
      (!resumePlan.valid || !resumePlan.archivePhase)) {
    const std::filesystem::path lastPath = individualDiffs.empty()
                                               ? std::filesystem::path()
                                               : individualDiffs.back().path;
    const auto checkpointRequest = checkpointSaveRequest(true);
    if (checkpointRequest.has_value()) {
      saveCheckpointForFlush(
          makeCheckpoint(computeScanSignature(individualDiffs.size(), 0),
                         kScanCheckpointPhaseArchive, 0, 0, lastPath, {}, ""),
          *checkpointRequest);
    }
  }

  const std::size_t archiveStartIndex =
      resumePlan.valid && resumePlan.archivePhase ? resumePlan.archiveStart : 0;
  const std::size_t archiveSubStartIndex =
      resumePlan.valid && resumePlan.archivePhase ? resumePlan.archiveSubStart
                                                  : 0;
  auto writePendingArchiveCache = [&](const ArchiveParseBatch &batch,
                                      int parsedChartCount) {
    const auto cacheIt =
        pendingArchiveCacheDiffs.find(archiveScanKey(batch.archivePath));
    if (cacheIt == pendingArchiveCacheDiffs.end()) {
      return;
    }
    const ArchiveCacheDiff &diff = cacheIt->second;
    if (parsedChartCount != diff.chartCount) {
      archive_file::appendDebugLogLine(
          "Writing archive scan cache with parsed chart count: " +
          fspath_to_utf8(diff.path) +
          " candidates=" + std::to_string(diff.chartCount) +
          " dbCharts=" + std::to_string(parsedChartCount));
    }
    scanBatch->UpsertArchiveCache({
        .path = diff.path,
        .solid = diff.solid,
        .uncompressedSize = diff.uncompressedSize,
        .fileCount = diff.fileCount,
        .chartCount = parsedChartCount,
    });
  };

  auto archiveBatchForIndex =
      [&](std::size_t archiveIndex) -> const ArchiveParseBatch * {
    if (archiveIndex >= archiveBatchOrder.size()) {
      return nullptr;
    }
    const auto batchIt = archiveBatches.find(archiveBatchOrder[archiveIndex]);
    if (batchIt == archiveBatches.end()) {
      return nullptr;
    }
    return &batchIt->second;
  };

  auto archiveInnerStartForIndex = [&](std::size_t archiveIndex) {
    return archiveIndex == archiveStartIndex ? archiveSubStartIndex
                                             : std::size_t{0};
  };

  auto archiveBatchIsPrepared = [&](std::size_t archiveIndex) {
    const ArchiveParseBatch *batch = archiveBatchForIndex(archiveIndex);
    if (batch == nullptr ||
        batch->parseAttempted.size() != batch->innerPaths.size() ||
        batch->preparedMetas.size() != batch->innerPaths.size()) {
      return false;
    }
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (innerStart > batch->innerPaths.size()) {
      return false;
    }
    return std::all_of(
        batch->parseAttempted.begin() +
            static_cast<std::vector<bool>::difference_type>(innerStart),
        batch->parseAttempted.end(), [](bool attempted) { return attempted; });
  };

  auto preparedArchiveParseResult = [&](std::size_t archiveIndex) {
    ArchiveParseJobResult result;
    result.archiveIndex = archiveIndex;
    const ArchiveParseBatch *batch = archiveBatchForIndex(archiveIndex);
    if (batch == nullptr || !archiveBatchIsPrepared(archiveIndex)) {
      result.errorMessage = "Archive parse result was not prepared.";
      return result;
    }
    result.innerStart = archiveInnerStartForIndex(archiveIndex);
    result.pendingInnerPaths.assign(
        batch->innerPaths.begin() +
            static_cast<std::vector<std::filesystem::path>::difference_type>(
                result.innerStart),
        batch->innerPaths.end());
    std::vector<ArchiveParsedChart> parsedCharts;
    parsedCharts.reserve(result.pendingInnerPaths.size());
    for (std::size_t index = result.innerStart;
         index < batch->innerPaths.size(); ++index) {
      parsedCharts.push_back({
          .innerPath = batch->innerPaths[index],
          .chartPath = archive_file::makeVirtualPath(batch->archivePath,
                                                     batch->innerPaths[index]),
          .meta = batch->preparedMetas[index],
      });
    }
    result.parsedCharts = std::move(parsedCharts);
    result.complete = true;
    return result;
  };

  std::vector<std::shared_ptr<ArchiveParseJobState>> archiveParseResults;
  archiveParseResults.reserve(archiveBatchOrder.size());
  std::unordered_set<std::size_t> prefetchedArchiveIndexes;
  {
    std::lock_guard lock(indexedArchivePrefetchMutex);
    if (!multiArchivePrefetchActive) {
      heldLargeArchivePrefetch.reset();
    }
    for (std::size_t archiveIndex = 0; archiveIndex < archiveBatchOrder.size();
         ++archiveIndex) {
      if (const auto prefetchIt =
              prefetchedArchiveStates.find(archiveBatchOrder[archiveIndex]);
          prefetchIt != prefetchedArchiveStates.end()) {
        archiveParseResults.push_back(prefetchIt->second);
        prefetchedArchiveIndexes.insert(archiveIndex);
      } else {
        archiveParseResults.push_back(std::make_shared<ArchiveParseJobState>());
      }
    }
  }
  auto storeArchiveParseResult = [&](ArchiveParseJobResult result) {
    if (result.archiveIndex < archiveParseResults.size()) {
      storeArchiveParseStateResult(archiveParseResults[result.archiveIndex],
                                   std::move(result));
    }
  };
  auto storeArchiveParseChunk =
      [&](std::size_t archiveIndex, std::size_t firstInnerIndex,
          std::vector<ArchiveParsedChart> parsedCharts) {
        if (archiveIndex < archiveParseResults.size()) {
          storeArchiveParseStateChunk(archiveParseResults[archiveIndex],
                                      firstInnerIndex, std::move(parsedCharts));
        }
      };
  auto waitTakeArchiveParseEvent = [&](std::size_t archiveIndex,
                                       std::size_t nextInnerIndex) {
    ArchiveParseJobEvent event;
    for (;;) {
      {
        std::unique_lock lock(archiveParseResultMutex);
        archiveParseResultCv.wait_for(lock, std::chrono::milliseconds(20), [&] {
          if (archiveIndex >= archiveParseResults.size()) {
            return false;
          }
          const auto &state = *archiveParseResults[archiveIndex];
          if (state.chunks.contains(nextInnerIndex)) {
            return true;
          }
          if (!state.terminalResult.has_value()) {
            return false;
          }
          const auto &terminal = *state.terminalResult;
          return !terminal.incremental || !terminal.complete ||
                 nextInnerIndex >=
                     terminal.innerStart + terminal.pendingInnerPaths.size();
        });
        if (archiveIndex < archiveParseResults.size()) {
          auto &state = *archiveParseResults[archiveIndex];
          if (state.terminalResult.has_value() &&
              (!state.terminalResult->incremental ||
               !state.terminalResult->complete ||
               nextInnerIndex >=
                   state.terminalResult->innerStart +
                       state.terminalResult->pendingInnerPaths.size())) {
            event.terminalResult = std::move(state.terminalResult);
            state.terminalResult.reset();
            return event;
          }
          if (auto chunkIt = state.chunks.find(nextInnerIndex);
              chunkIt != state.chunks.end()) {
            event.parsedCharts = std::move(chunkIt->second);
            state.chunks.erase(chunkIt);
            return event;
          }
        }
      }
      if (shouldStop()) {
        event.terminalResult = ArchiveParseJobResult{
            .archiveIndex = archiveIndex,
            .innerStart = archiveInnerStartForIndex(archiveIndex),
            .errorMessage = "Archive parse cancelled.",
        };
        return event;
      }
    }
  };

  auto tryReadSingleArchiveConcurrently = [&](std::size_t archiveIndex,
                                              const ArchiveParseBatch &batch,
                                              std::size_t innerStart) {
    std::vector<std::filesystem::path> pendingInnerPaths(
        batch.innerPaths.begin() +
            static_cast<std::vector<std::filesystem::path>::difference_type>(
                innerStart),
        batch.innerPaths.end());
    if (entityWorkerCount <= 1 ||
        pendingInnerPaths.size() < kArchiveDirectConcurrentMinCharts) {
      return false;
    }

    std::unordered_map<std::string, std::size_t> sequenceByInnerPath;
    sequenceByInnerPath.reserve(pendingInnerPaths.size());
    for (std::size_t index = 0; index < pendingInnerPaths.size(); ++index) {
      sequenceByInnerPath.emplace(
          checkpointInnerPathText(pendingInnerPaths[index]), index);
    }

    std::mutex resultMutex;
    std::vector<std::optional<ArchiveParsedChart>> parsedSlots(
        pendingInnerPaths.size());
    std::string callbackError;
    std::string readError;
    const std::size_t workerCount =
        std::min(entityWorkerCount, pendingInnerPaths.size());
    const bool readOk = archive_file::readArchiveEntriesConcurrently(
        batch.archivePath, pendingInnerPaths,
        [&](archive_file::FileData &&file) {
          const auto sequenceIt =
              sequenceByInnerPath.find(checkpointInnerPathText(file.path));
          if (sequenceIt == sequenceByInnerPath.end()) {
            std::lock_guard lock(resultMutex);
            callbackError =
                "Archive entry was not in the requested chart batch.";
            return false;
          }

          ArchiveParsedChart parsed{
              .innerPath = file.path,
              .chartPath =
                  archive_file::makeVirtualPath(batch.archivePath, file.path),
          };
          try {
            if (!shouldStop()) {
              parsed.meta = parseChartMeta(parsed.chartPath, &file.bytes);
            }
          } catch (const std::exception &e) {
            std::lock_guard lock(resultMutex);
            callbackError = e.what();
          } catch (...) {
            std::lock_guard lock(resultMutex);
            callbackError = "Unknown archive chart parse error.";
          }
          {
            std::lock_guard lock(resultMutex);
            parsedSlots[sequenceIt->second] = std::move(parsed);
          }
          return !shouldStop();
        },
        workerCount, kArchiveParseMaxInFlightBytes, &readError, pauseCallback);

    std::vector<ArchiveParsedChart> parsedCharts;
    {
      std::lock_guard lock(resultMutex);
      if (!readOk || shouldStop() ||
          !std::all_of(parsedSlots.begin(), parsedSlots.end(),
                       [](const auto &slot) { return slot.has_value(); })) {
        if (!callbackError.empty()) {
          readError = callbackError;
        }
        archive_file::appendDebugLogLine(
            "Single archive concurrent read unavailable; using bounded "
            "pipeline: " +
            fspath_to_utf8(batch.archivePath) +
            (readError.empty() ? std::string() : ": " + readError));
        return false;
      }
      parsedCharts.reserve(parsedSlots.size());
      for (auto &slot : parsedSlots) {
        parsedCharts.push_back(std::move(*slot));
      }
    }

    storeArchiveParseResult({
        .archiveIndex = archiveIndex,
        .innerStart = innerStart,
        .pendingInnerPaths = std::move(pendingInnerPaths),
        .parsedCharts = std::move(parsedCharts),
        .complete = true,
        .errorMessage = std::move(callbackError),
    });
    archive_file::appendDebugLogLine(
        "Finished single archive concurrent chart parse: " +
        fspath_to_utf8(batch.archivePath) +
        " files=" + std::to_string(parsedSlots.size()) +
        " workers=" + std::to_string(workerCount));
    return true;
  };

  std::vector<std::size_t> unpreparedArchiveIndexes;
  for (std::size_t archiveIndex = archiveStartIndex;
       archiveIndex < archiveBatchOrder.size(); ++archiveIndex) {
    const ArchiveParseBatch *batch = archiveBatchForIndex(archiveIndex);
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (batch != nullptr && innerStart < batch->innerPaths.size() &&
        !archiveBatchIsPrepared(archiveIndex) &&
        !prefetchedArchiveIndexes.contains(archiveIndex)) {
      unpreparedArchiveIndexes.push_back(archiveIndex);
    }
  }

  std::optional<std::size_t> directConcurrentArchiveIndex;
  if (unpreparedArchiveIndexes.size() == 1 && !shouldStop()) {
    const std::size_t archiveIndex = unpreparedArchiveIndexes.front();
    const ArchiveParseBatch *batch = archiveBatchForIndex(archiveIndex);
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (batch != nullptr && batch->innerPaths.size() - innerStart >=
                                kArchiveDirectConcurrentMinCharts) {
      reportProgress(parseCurrent, parseTotal,
                     ChartScanProgressStage::ReadingArchive);
      if (tryReadSingleArchiveConcurrently(archiveIndex, *batch, innerStart)) {
        directConcurrentArchiveIndex = archiveIndex;
      }
    }
  }

  chart_scan::WorkScheduler *archiveParseScheduler = &entityScheduler;

  for (const std::size_t archiveIndex : unpreparedArchiveIndexes) {
    if (shouldStop()) {
      break;
    }
    if (directConcurrentArchiveIndex == archiveIndex) {
      continue;
    }
    const ArchiveParseBatch *batch = archiveBatchForIndex(archiveIndex);
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (batch == nullptr || innerStart >= batch->innerPaths.size() ||
        archiveBatchIsPrepared(archiveIndex)) {
      continue;
    }
    std::vector<std::filesystem::path> pendingInnerPaths(
        batch->innerPaths.begin() +
            static_cast<std::vector<std::filesystem::path>::difference_type>(
                innerStart),
        batch->innerPaths.end());
    reportProgress(parseCurrent, parseTotal,
                   ChartScanProgressStage::ReadingArchive);
    archive_file::appendDebugLogLine(
        "Queued bounded DB archive chart batch parse: " +
        fspath_to_utf8(batch->archivePath) +
        " requested=" + std::to_string(pendingInnerPaths.size()) +
        " archiveReaders=" + std::to_string(archiveIoLimit) +
        " workers=" + std::to_string(entityWorkerCount));
    const auto archiveReadWorkClass =
        pendingInnerPaths.size() >= kArchiveDirectConcurrentMinCharts
            ? chart_scan::WorkClass::ArchiveReadHeavy
            : chart_scan::WorkClass::ArchiveRead;
    if (!archiveParseScheduler->enqueue(
            [&, archiveIndex, innerStart, archivePath = batch->archivePath,
             pendingInnerPaths = std::move(pendingInnerPaths)]() mutable {
              const auto publishErrorResult = [&](std::string message) {
                std::vector<std::filesystem::path> errorPaths;
                if (const auto *errorBatch = archiveBatchForIndex(archiveIndex);
                    errorBatch != nullptr &&
                    innerStart <= errorBatch->innerPaths.size()) {
                  errorPaths.assign(
                      errorBatch->innerPaths.begin() +
                          static_cast<std::vector<
                              std::filesystem::path>::difference_type>(
                              innerStart),
                      errorBatch->innerPaths.end());
                }
                storeArchiveParseResult({
                    .archiveIndex = archiveIndex,
                    .innerStart = innerStart,
                    .pendingInnerPaths = std::move(errorPaths),
                    .incremental = true,
                    .errorMessage = std::move(message),
                });
              };
              try {
                runArchiveChartPipeline(
                    *archiveParseScheduler, std::move(archivePath),
                    std::move(pendingInnerPaths),
                    [&, archiveIndex,
                     innerStart](ArchiveChartPipelineResult result) {
                      storeArchiveParseResult({
                          .archiveIndex = archiveIndex,
                          .innerStart = innerStart,
                          .pendingInnerPaths = std::move(result.innerPaths),
                          .parsedCharts = std::move(result.parsedCharts),
                          .complete = result.complete,
                          .incremental = true,
                          .errorMessage = std::move(result.errorMessage),
                      });
                    },
                    [&, archiveIndex,
                     innerStart](std::size_t firstIndex,
                                 std::vector<ArchiveParsedChart> parsedCharts) {
                      storeArchiveParseChunk(archiveIndex,
                                             innerStart + firstIndex,
                                             std::move(parsedCharts));
                    });
              } catch (const std::exception &error) {
                publishErrorResult(error.what());
                throw;
              } catch (...) {
                publishErrorResult("Unknown archive parse error.");
                throw;
              }
            },
            archiveReadWorkClass)) {
      ArchiveParseJobResult rejected{
          .archiveIndex = archiveIndex,
          .innerStart = innerStart,
          .errorMessage = "Archive parse job was not queued.",
      };
      rejected.pendingInnerPaths.assign(
          batch->innerPaths.begin() +
              static_cast<std::vector<std::filesystem::path>::difference_type>(
                  innerStart),
          batch->innerPaths.end());
      storeArchiveParseResult(std::move(rejected));
    }
  }
  for (std::size_t archiveIndex = archiveStartIndex;
       archiveIndex < archiveBatchOrder.size(); ++archiveIndex) {
    if (shouldStop()) {
      break;
    }
    const auto &archiveKey = archiveBatchOrder[archiveIndex];
    const ArchiveParseBatch *batchPtr = archiveBatchForIndex(archiveIndex);
    if (batchPtr == nullptr) {
      continue;
    }
    const ArchiveParseBatch &batch = *batchPtr;
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (innerStart >= batch.innerPaths.size()) {
      const int storedChartCount =
          innerStart > 0 ? scanBatch->CountChartsInArchive(batch.archivePath)
                         : 0;
      writePendingArchiveCache(batch, storedChartCount);
      const std::filesystem::path lastPath =
          batch.innerPaths.empty()
              ? std::filesystem::path()
              : archive_file::makeVirtualPath(batch.archivePath,
                                              batch.innerPaths.back());
      const std::string lastInnerPath =
          batch.innerPaths.empty()
              ? ""
              : checkpointInnerPathText(batch.innerPaths.back());
      const auto checkpointRequest = checkpointSaveRequest(true);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(
            makeCheckpoint(
                computeScanSignature(individualDiffs.size(), archiveIndex + 1),
                kScanCheckpointPhaseArchive, 0, 0, lastPath, batch.archivePath,
                lastInnerPath),
            *checkpointRequest);
      }
      continue;
    }

    const std::string archiveText = fspath_to_utf8(batch.archivePath);
    const std::size_t requestedCharts = batch.innerPaths.size() - innerStart;
    const bool archiveInsertStmtReady = true;
    std::optional<bool> archiveSolidHint;
    if (const auto cacheIt = pendingArchiveCacheDiffs.find(archiveKey);
        cacheIt != pendingArchiveCacheDiffs.end()) {
      archiveSolidHint = cacheIt->second.solid;
    }
    const auto archiveSourcePreference =
        archiveBatchSourcePreference(batch.archivePath, archiveSolidHint);
    std::optional<ChartSourcePreference> storedArchiveSourcePreference;
    if (archiveSourcePreference.has_value()) {
      storedArchiveSourcePreference = ChartSourcePreference{
          .priority = archiveSourcePreference->priority,
          .archiveSize = archiveSourcePreference->archiveSize,
      };
    }

    std::size_t insertedCharts = 0;
    std::size_t deliveredCharts = 0;
    int storedChartCount =
        innerStart > 0 ? scanBatch->CountChartsInArchive(batch.archivePath) : 0;
    std::chrono::steady_clock::duration insertDuration{};
    bool insertionStarted = false;
    bool checkpointOrderReliable = true;
    std::size_t parsedInBatch = innerStart;
    auto applyParsedCharts = [&](std::vector<ArchiveParsedChart> parsedCharts) {
      if (parsedCharts.empty() || shouldStop()) {
        return;
      }
      if (!insertionStarted) {
        insertionStarted = true;
        archive_file::appendDebugLogLine(
            "Inserting streamed DB chart batch: " + archiveText +
            " requested=" + std::to_string(requestedCharts) +
            " files=" + std::to_string(parsedCharts.size()));
      }
      const auto chunkInsertStart = std::chrono::steady_clock::now();
      for (auto &parsed : parsedCharts) {
        if (shouldStop()) {
          break;
        }
        reportProgress(parseCurrent, parseTotal,
                       ChartScanProgressStage::ParsingCharts);
        if (parsed.meta.has_value()) {
          if (scanBatch->UpsertChart(*parsed.meta,
                                     storedArchiveSourcePreference)) {
            ++insertedCharts;
            ++storedChartCount;
          }
        }
        ++deliveredCharts;
        ++parseCurrent;
        if (parsedInBatch >= batch.innerPaths.size() ||
            checkpointInnerPathText(batch.innerPaths[parsedInBatch]) !=
                checkpointInnerPathText(parsed.innerPath)) {
          checkpointOrderReliable = false;
        }
        ++parsedInBatch;
        if (checkpointOrderReliable) {
          const auto checkpointRequest = checkpointSaveRequest(
              parsedInBatch % kArchiveParseCheckpointInterval == 0);
          if (checkpointRequest.has_value()) {
            saveCheckpointForFlush(
                makeCheckpoint(
                    computeScanSignature(individualDiffs.size(), archiveIndex),
                    kScanCheckpointPhaseArchive, 0, parsedInBatch,
                    parsed.chartPath, batch.archivePath,
                    checkpointInnerPathText(parsed.innerPath)),
                *checkpointRequest);
          }
        }
      }
      insertDuration += std::chrono::steady_clock::now() - chunkInsertStart;
    };

    std::optional<ArchiveParseJobResult> terminalResult;
    if (archiveBatchIsPrepared(archiveIndex)) {
      terminalResult = preparedArchiveParseResult(archiveIndex);
    } else {
      while (!terminalResult.has_value() && !shouldStop()) {
        ArchiveParseJobEvent event =
            waitTakeArchiveParseEvent(archiveIndex, parsedInBatch);
        if (event.terminalResult.has_value()) {
          terminalResult = std::move(event.terminalResult);
        } else {
          applyParsedCharts(std::move(event.parsedCharts));
        }
      }
    }
    if (terminalResult.has_value() &&
        terminalResult->parsedCharts.has_value()) {
      applyParsedCharts(std::move(*terminalResult->parsedCharts));
    }

    const bool parsedFullBatch =
        terminalResult.has_value() && terminalResult->complete &&
        parsedInBatch == batch.innerPaths.size() && !shouldStop();
    if (terminalResult.has_value() && !terminalResult->complete) {
      if (!terminalResult->errorMessage.empty()) {
        SDL_Log("Failed to read charts from archive %s: %s",
                archiveText.c_str(), terminalResult->errorMessage.c_str());
        archive_file::appendDebugLogLine(
            "Failed to stream DB chart batch: " + archiveText + ": " +
            terminalResult->errorMessage);
      }
      if (!shouldStop()) {
        parseCurrent +=
            static_cast<int>(batch.innerPaths.size() - parsedInBatch);
        if (insertedCharts > 0 &&
            !scanBatch->DeleteChartsInArchive(batch.archivePath)) {
          archive_file::appendDebugLogLine(
              "Failed to remove partially streamed DB chart batch: " +
              archiveText);
        }
      }
    }

    if (insertionStarted) {
      const auto insertMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(insertDuration)
              .count();
      archive_file::appendDebugLogLine(
          "Finished streamed DB chart batch insert: " + archiveText +
          " requested=" + std::to_string(requestedCharts) +
          " files=" + std::to_string(deliveredCharts) +
          " inserted=" + std::to_string(insertedCharts) +
          " insertMs=" + std::to_string(insertMs) + " reusedStatement=" +
          std::string(archiveInsertStmtReady ? "true" : "false") +
          " sourcePreferenceHint=" +
          std::string(archiveSourcePreference.has_value() ? "true" : "false") +
          " solidHint=" +
          (archiveSolidHint.has_value()
               ? std::string(*archiveSolidHint ? "true" : "false")
               : std::string("unknown")));
    }
    if (parsedFullBatch && !stopRequested(stopToken)) {
      writePendingArchiveCache(batch, storedChartCount);
      const std::filesystem::path lastPath =
          batch.innerPaths.empty()
              ? std::filesystem::path()
              : archive_file::makeVirtualPath(batch.archivePath,
                                              batch.innerPaths.back());
      const std::string lastInnerPath =
          batch.innerPaths.empty()
              ? ""
              : checkpointInnerPathText(batch.innerPaths.back());
      const auto checkpointRequest = checkpointSaveRequest(true);
      if (checkpointRequest.has_value()) {
        saveCheckpointForFlush(
            makeCheckpoint(
                computeScanSignature(individualDiffs.size(), archiveIndex + 1),
                kScanCheckpointPhaseArchive, 0, 0, lastPath, batch.archivePath,
                lastInnerPath),
            *checkpointRequest);
      }
    } else {
      archive_file::appendDebugLogLine(
          "Skipped archive scan cache write because chart batch did not "
          "complete: " +
          archiveText + " requested=" + std::to_string(requestedCharts) +
          " files=" + std::to_string(deliveredCharts));
    }
  }
  if (shouldStop()) {
    archiveParseScheduler->cancel();
  } else {
    archiveParseScheduler->finish();
  }
  for (const auto &exception : archiveParseScheduler->takeExceptions()) {
    try {
      if (exception != nullptr) {
        std::rethrow_exception(exception);
      }
    } catch (const std::exception &e) {
      archive_file::appendDebugLogLine("Chart scan worker failed: " +
                                       std::string(e.what()));
    } catch (...) {
      archive_file::appendDebugLogLine(
          "Chart scan worker failed with an unknown error.");
    }
  }
  const int changedCount = scanBatch->ChangedCount();
  if (!scanBatch->Commit()) {
    archive_file::appendDebugLogLine(
        "Failed to commit final chart scan batch.");
  }
  acknowledgeFlushRequest(pendingFlushRequest());
  if (!stopRequested(stopToken)) {
    session.ClearScanCheckpoint();
    session.ClearChartMetadataRebuildRequired();
  }
  return changedCount;
}
