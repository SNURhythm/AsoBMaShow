#include "ChartLibraryScanner.h"

#include "ArchiveFile.h"
#include "BmsChartFile.h"
#include "BmsMetadataText.h"
#include "CanonicalDigest.h"
#include "ChartScanWorkScheduler.h"
#include "ThreadCompat.h"
#include "Utils.h"
#include "bms_search/DownloadStorageIdentity.h"
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
constexpr std::size_t kIndividualParseBatchSize = 512;
constexpr std::size_t kIndividualCommitInterval = 1000;
constexpr std::size_t kArchiveParseMaxInFlightFiles = 12;
constexpr std::size_t kArchiveParseResultChunkSize =
    kArchiveParseMaxInFlightFiles;
constexpr std::uint64_t kArchiveParseMaxInFlightBytes =
    16ull * 1024ull * 1024ull;
constexpr std::size_t kArchiveDirectConcurrentMinCharts = 16;
constexpr std::size_t kArchiveClassificationPauseInterval = 256;
constexpr const char *kScanCheckpointPhaseIndividual = "individual";
constexpr const char *kScanCheckpointPhaseArchive = "archive";

bool isFindBmsPrivateStorageDirectory(const std::filesystem::path &path) {
  if (path.empty() || path.parent_path().filename() != "BMSSEARCH") {
    return false;
  }
  return fspath_to_utf8(path.filename()) ==
         asobmshow::bms_search::kFindBmsTransactionDirectoryName;
}

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

bool reconcileIdentityHasStableIdentity(
    const ChartScanReconcileIdentity &identity) {
  return !identity.path.empty() &&
         canonical_digest::isCanonicalLowerHex(identity.md5, 32) &&
         canonical_digest::isCanonicalLowerHex(identity.sha256, 64);
}

bool parsedChartMetaLooksInsertable(const bms_parser::ChartMeta &meta) {
  return parsedChartMetaHasStableIdentity(meta) &&
         (meta.TotalNotes > 0 || meta.TotalLandmineNotes > 0);
}

struct ParsedChartMetadata {
  bms_parser::ChartMeta meta;
  ChartSequenceFeatures sequenceFeatures;
};

ChartSequenceFeatures sequenceFeatures(const bms_parser::Chart &chart) {
  ChartSequenceFeatures result;
  // SongData#setBMSModel uses BMSModel#getBgaList, which the pinned decoder
  // fills from declared #BMP resources rather than from BGA timeline use.
  result.hasBga = !chart.BmpTable.empty();
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) continue;
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) continue;
      // SongData.setBMSModel at the pinned commit checks these exact values.
      result.hasBpmStop = result.hasBpmStop || timeline->StopLength > 0;
      result.hasScrollChange =
          result.hasScrollChange || timeline->Scroll != 1.0;
    }
  }
  return result;
}

bool hasTextDocumentExtension(const std::filesystem::path &path) {
  std::string filename = fspath_to_utf8(path.filename());
  if (filename.size() < 4) {
    return false;
  }
  for (char &character : filename) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return filename.ends_with(".txt");
}

bool folderContainsTextDocument(const std::filesystem::path &folder) {
  // SQLiteSongDatabaseAccessor.BMSFolder.addFile() sets SongData's
  // CONTENT_TEXT flag from any immediate, non-directory child whose
  // Locale.ROOT-lowercased name ends in ".txt". This is deliberately not a
  // BMS-header or recursive-file check.
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      folder, std::filesystem::directory_options::skip_permission_denied,
      error);
  for (const auto end = std::filesystem::directory_iterator();
       !error && iterator != end; iterator.increment(error)) {
    std::error_code typeError;
    if (iterator->is_directory(typeError) || typeError) {
      continue;
    }
    if (hasTextDocumentExtension(iterator->path())) {
      return true;
    }
  }
  return false;
}

struct ArchiveScanResult {
  bool readable = false;
  bool solid = false;
  int fileCount = 0;
  std::uint64_t uncompressedSize = 0;
  std::vector<std::filesystem::path> chartPaths;
  std::unordered_set<path_t> documentedChartPaths;
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
  std::unordered_set<path_t> documentedFolders;
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
    if (hasTextDocumentExtension(entry.path)) {
      documentedFolders.insert(
          fspath_to_path_t(entry.path.parent_path().lexically_normal()));
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
  for (const auto &chartPath : result.chartPaths) {
    std::filesystem::path parsedArchivePath;
    std::filesystem::path innerPath;
    if (archive_file::splitVirtualPath(chartPath, parsedArchivePath, innerPath) &&
        documentedFolders.contains(
            fspath_to_path_t(innerPath.parent_path().lexically_normal()))) {
      result.documentedChartPaths.insert(fspath_to_path_t(chartPath));
    }
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

std::int64_t fileTimeToUnixSeconds(std::filesystem::file_time_type time) {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::file_clock::to_sys(time)
                               .time_since_epoch())
                           .count();
  return static_cast<std::int64_t>(seconds);
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

bool pathAtOrInsideRoot(const std::filesystem::path &path,
                        const std::filesystem::path &root) {
  if (path.empty() || root.empty()) {
    return false;
  }
  const auto normalizedPath = path.lexically_normal();
  const auto normalizedRoot = root.lexically_normal();
  if (normalizedPath == normalizedRoot) {
    return true;
  }
  const auto relative = normalizedPath.lexically_relative(normalizedRoot);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
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
  return ScanWithResult(session, roots, stopToken, std::move(progressCallback),
                        std::move(pauseCallback),
                        std::move(flushRequestCallback),
                        std::move(flushCompleteCallback))
      .changedCount;
}

int ChartLibraryScanner::ScanAdded(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  return ScanAddedWithResult(session, roots, stopToken,
                             std::move(progressCallback),
                             std::move(pauseCallback),
                             std::move(flushRequestCallback),
                             std::move(flushCompleteCallback))
      .changedCount;
}

ChartScanResult ChartLibraryScanner::ScanWithResult(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  return ScanImpl(session, roots, ReconcileMode::Full, stopToken,
                  std::move(progressCallback), std::move(pauseCallback),
                  std::move(flushRequestCallback),
                  std::move(flushCompleteCallback));
}

ChartScanResult ChartLibraryScanner::ScanAddedWithResult(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  return ScanImpl(session, roots, ReconcileMode::None, stopToken,
                  std::move(progressCallback), std::move(pauseCallback),
                  std::move(flushRequestCallback),
                  std::move(flushCompleteCallback));
}

ChartScanResult ChartLibraryScanner::ScanScopedWithResult(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  return ScanImpl(session, roots, ReconcileMode::Scoped, stopToken,
                  std::move(progressCallback), std::move(pauseCallback),
                  std::move(flushRequestCallback),
                  std::move(flushCompleteCallback));
}

ChartScanResult ChartLibraryScanner::ScanImpl(
    ChartRepository::Session &session,
    const std::vector<std::filesystem::path> &roots,
    ReconcileMode reconcileMode,
    const std::stop_token *stopToken,
    ChartScanProgressCallback progressCallback,
    ChartScanPauseCallback pauseCallback,
    ChartScanFlushRequestCallback flushRequestCallback,
    ChartScanFlushCompleteCallback flushCompleteCallback) {
  if (stopRequested(stopToken)) {
    return {};
  }
  const bool reconcileExisting = reconcileMode != ReconcileMode::None;
  std::atomic_bool interrupted{false};
  auto pauseIfNeeded = [&]() {
    return pauseCallback == nullptr || pauseCallback();
  };
  auto shouldStop = [&]() {
    if (interrupted.load(std::memory_order_relaxed)) {
      return true;
    }
    if (stopRequested(stopToken) || !pauseIfNeeded()) {
      interrupted.store(true, std::memory_order_relaxed);
      return true;
    }
    return false;
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

  using ScanClock = std::chrono::steady_clock;
  auto scanPhaseStart = []() { return ScanClock::now(); };
  auto scanPhaseEnd = [&](const char *phase, ScanClock::time_point start) {
    const auto phaseMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(ScanClock::now() -
                                                              start)
            .count();
    SDL_Log("Chart scan phase: %s took %lld ms", phase,
            static_cast<long long>(phaseMillis));
    archive_file::appendDebugLogLine(std::string("Chart scan phase: ") +
                                     phase + " took " +
                                     std::to_string(phaseMillis) + " ms");
  };
  auto scanPhase = [&](const char *phase) {
    archive_file::appendDebugLogLine(std::string("Chart scan phase begin: ") +
                                     phase);
  };

  reportProgress(0, static_cast<int>(std::max<std::size_t>(roots.size(), 1)),
                 ChartScanProgressStage::Preparing);
  if (shouldStop()) {
    return {};
  }

  const auto scanSnapshotLoadStart = scanPhaseStart();
  ChartScanSnapshot scanSnapshot = session.LoadScanSnapshot(
      reconcileExisting ? ChartScanSnapshotLoad::Reconcile
                        : ChartScanSnapshotLoad::CheckpointOnly);
  // The reconcile pass only needs each stored chart's identity (path + md5 +
  // sha256), so load those lightweight records instead of full metadata rows.
  std::vector<ChartScanReconcileIdentity> storedChartIdentities;
  if (reconcileExisting) {
    storedChartIdentities = session.LoadScanReconcileIdentities();
  }
  scanPhaseEnd("load-scan-snapshot", scanSnapshotLoadStart);
  if (reconcileMode == ReconcileMode::Scoped) {
    const auto pathOutsideScope = [&](const std::filesystem::path &path) {
      return std::ranges::none_of(roots, [&](const auto &root) {
        return pathAtOrInsideRoot(path, root);
      });
    };
    std::erase_if(storedChartIdentities, [&](const auto &identity) {
      return pathOutsideScope(identity.path);
    });
    std::erase_if(scanSnapshot.solidArchives, [&](const auto &archive) {
      return pathOutsideScope(archive.path);
    });
    std::erase_if(scanSnapshot.archiveCache, [&](const auto &archive) {
      return pathOutsideScope(archive.path);
    });
  }
  const ChartScanCheckpoint checkpoint =
      scanSnapshot.checkpoint.value_or(ChartScanCheckpoint{});

  struct ScanDiff {
    std::filesystem::path path;
    bool deleted = false;
    bool parseAttempted = false;
    bool hasDocument = false;
    std::optional<ParsedChartMetadata> preparedMeta;
  };
  std::vector<ScanDiff> diffs;
  std::vector<std::pair<std::filesystem::path, bool>> documentFlagUpdates;
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
  std::vector<std::filesystem::path> liveArchivePaths;
  std::unordered_set<path_t> knownChartPaths;
  std::unordered_map<path_t, int> knownArchiveChartCounts;
  std::unordered_map<path_t, int> storedArchiveChartCounts;
  std::unordered_set<path_t> scannedArchivePaths;
  diffs.reserve(storedChartIdentities.size());
  documentFlagUpdates.reserve(storedChartIdentities.size());
  sourcePreferenceRefreshPaths.reserve(storedChartIdentities.size());
  cachedSourcePreferenceUpdates.reserve(storedChartIdentities.size());

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

  std::map<std::filesystem::path, ChartFolderScanNode> folderScanNodes;
  std::vector<std::filesystem::path> folderScanRoots;
  const auto observeFolder = [&](const std::filesystem::path &path) {
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    const std::int64_t dateSeconds =
        error ? -1 : fileTimeToUnixSeconds(modified);
    const auto normalized = path.lexically_normal();
    folderScanNodes.insert_or_assign(
        normalized, ChartFolderScanNode{.path = fspath_to_path_t(normalized),
                                        .dateSeconds = dateSeconds});
  };

  for (const auto &identity : storedChartIdentities) {
    if (shouldStop()) {
      return {};
    }
    if (!reconcileIdentityHasStableIdentity(identity)) {
      diffs.push_back({.path = identity.path, .deleted = true});
      continue;
    }

    std::filesystem::path archivePath;
    std::filesystem::path innerPath;
    const bool liveArchivePath =
        archive_file::splitVirtualPath(identity.path, archivePath, innerPath);
    std::filesystem::path storedArchivePathValue;
    std::filesystem::path storedInnerPath;
    const bool storedArchivePath = splitStoredArchiveVirtualPath(
        identity.path, storedArchivePathValue, storedInnerPath);
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
          knownChartPaths.insert(fspath_to_path_t(identity.path));
          cachedSourcePreferenceUpdates.push_back({
              .path = identity.path,
              .priority = archiveState.cache.solid ? 2 : 1,
              .archiveSize = static_cast<std::uint64_t>(
                  std::max<std::int64_t>(0, archiveState.archiveSize)),
          });
        }
        continue;
      }
    }

    if (archive_file::exists(identity.path)) {
      knownChartPaths.insert(fspath_to_path_t(identity.path));
      sourcePreferenceRefreshPaths.push_back(identity.path);
    } else {
      diffs.push_back({.path = identity.path, .deleted = true});
    }
  }

  for (const auto &solidArchive : scanSnapshot.solidArchives) {
    if (shouldStop()) {
      return {};
    }
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    if (!archiveFileState(solidArchive.path, archiveSize, mtimeNs)) {
      staleSolidArchives.push_back(solidArchive.path);
    }
  }

  auto parseChartMeta = [&](const std::filesystem::path &path,
                            const std::vector<unsigned char> *bytes)
      -> std::optional<ParsedChartMetadata> {
    const std::string chartText = fspath_to_utf8(path);
    bms_parser::Parser parser;
    bms_parser::Chart *rawChart = nullptr;
    std::unique_ptr<bms_parser::Chart> chart;
    std::atomic_bool cancelled(false);
    try {
      if (bytes != nullptr) {
        parser.Parse(*bytes, &rawChart, false, false, cancelled);
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
        archive_file::parseChart(parser, path, &rawChart, false, false,
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
    return ParsedChartMetadata{.meta = std::move(chart->Meta),
                               .sequenceFeatures = sequenceFeatures(*chart)};
  };

  struct ArchiveParseBatch {
    std::filesystem::path archivePath;
    std::vector<std::filesystem::path> innerPaths;
    std::vector<bool> parseAttempted;
    std::vector<bool> hasDocument;
    std::vector<std::optional<ParsedChartMetadata>> preparedMetas;
  };

  struct ArchiveParsedChart {
    std::filesystem::path innerPath;
    std::filesystem::path chartPath;
    std::optional<ParsedChartMetadata> meta;
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
    bool hasDocument = false;
    std::optional<ParsedChartMetadata> meta;
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
  std::atomic_bool discoveryHealthy{true};
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
              },
              chart_scan::WorkClass::Cpu,
              [state, publishReady, fileBytes] {
                {
                  std::lock_guard lock(state->mutex);
                  if (state->pendingTasks > 0) {
                    --state->pendingTasks;
                  }
                  if (state->inFlightFiles > 0) {
                    --state->inFlightFiles;
                  }
                  state->inFlightBytes =
                      fileBytes > state->inFlightBytes
                          ? 0
                          : state->inFlightBytes - fileBytes;
                  if (state->errorMessage.empty()) {
                    state->errorMessage = "Archive chart parse cancelled.";
                  }
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

  std::unordered_map<path_t, bool> documentByFolder;
  auto hasDocumentForChartPath = [&](const std::filesystem::path &path) {
    const std::filesystem::path folder = path.parent_path();
    const path_t key = fspath_to_path_t(folder.lexically_normal());
    if (const auto found = documentByFolder.find(key);
        found != documentByFolder.end()) {
      return found->second;
    }
    const bool hasDocument = folderContainsTextDocument(folder);
    return documentByFolder.emplace(key, hasDocument).first->second;
  };

  auto scheduleOrdinaryChart = [&](const std::filesystem::path &path,
                                   bool hasDocument) {
    const path_t key = fspath_to_path_t(path);
    if (knownChartPaths.contains(key)) {
      documentFlagUpdates.emplace_back(path, hasDocument);
      return;
    }
    if (!discoveredOrdinaryChartPaths.insert(key).second) {
      return;
    }

    const std::size_t sequence = reservePreparedEntity();
    if (checkpoint.found) {
      storePreparedEntity(sequence, PreparedOrdinaryChart{
                                        .path = path,
                                        .hasDocument = hasDocument,
                                    });
      return;
    }

    if (!entityScheduler.enqueue([&, sequence, path, hasDocument] {
          if (shouldStop()) {
            return;
          }
          try {
            PreparedOrdinaryChart prepared{
                .path = path,
                .parseAttempted = true,
                .hasDocument = hasDocument,
                .meta = parseChartMeta(path, nullptr),
            };
            storePreparedEntity(sequence, std::move(prepared));
          } catch (...) {
            discoveryHealthy.store(false, std::memory_order_relaxed);
            storePreparedEntityIfMissing(sequence,
                                         PreparedOrdinaryChart{
                                             .path = path,
                                             .hasDocument = hasDocument,
                                         });
            throw;
          }
        })) {
      discoveryHealthy.store(false, std::memory_order_relaxed);
      storePreparedEntity(sequence, PreparedOrdinaryChart{
                                        .path = path,
                                        .hasDocument = hasDocument,
                                    });
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
      discoveryHealthy.store(false, std::memory_order_relaxed);
      return;
    }
    liveArchivePaths.push_back(archivePath);

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
                discoveryHealthy.store(false, std::memory_order_relaxed);
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
      discoveryHealthy.store(false, std::memory_order_relaxed);
      storePreparedEntity(sequence, PreparedArchive{
                                        .path = archivePath,
                                        .archiveSize = archiveSize,
                                        .mtimeNs = mtimeNs,
                                        .cache = cache,
                                    });
    }
  };

  int scannedRootCount = 0;
  bool traversalHealthy = true;
  const auto folderTraversalStart = scanPhaseStart();
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
      std::vector<AndroidTreeChartFile> androidChartFiles;
      std::string androidError;
      if (!ListAndroidTreeChartFiles(root, androidChartFiles, androidError,
                                     stopToken)) {
        traversalHealthy = false;
        if (!androidError.empty()) {
          SDL_Log("Failed while scanning Android chart folder %s: %s",
                  fspath_to_utf8(root).c_str(), androidError.c_str());
        }
        ++scannedRootCount;
        continue;
      }
      for (const auto &chartFile : androidChartFiles) {
        if (shouldStop()) {
          entityScheduler.cancel();
          return {};
        }
        const std::filesystem::path &path = chartFile.path;
        if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
          scheduleOrdinaryChart(path, chartFile.hasDocument);
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
      traversalHealthy = false;
      SDL_Log("Failed to check chart folder %s: %s",
              fspath_to_utf8(root).c_str(), error.message().c_str());
      ++scannedRootCount;
      continue;
    }
    if (!rootExists) {
      if (reconcileExisting) {
        traversalHealthy = false;
        SDL_Log("Configured chart folder is unavailable: %s",
                fspath_to_utf8(root).c_str());
      }
      ++scannedRootCount;
      continue;
    }

    std::error_code rootTypeError;
    if (std::filesystem::is_regular_file(root, rootTypeError) &&
        !rootTypeError) {
      if (asobmshow::bms_chart_file::isBmsChartPath(root)) {
        scheduleOrdinaryChart(root, hasDocumentForChartPath(root));
      } else if (archive_file::hasSupportedArchiveExtension(root)) {
        scheduleArchivePath(root);
      }
      ++scannedRootCount;
      continue;
    }

    std::error_code rootDirectoryError;
    if (std::filesystem::is_directory(root, rootDirectoryError) &&
        !rootDirectoryError) {
      const auto normalizedRoot = root.lexically_normal();
      observeFolder(normalizedRoot);
      folderScanRoots.push_back(normalizedRoot);
    }

    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied,
        error);
    for (const auto end = std::filesystem::recursive_directory_iterator();
         !error && iterator != end; iterator.increment(error)) {
      if (shouldStop()) {
        entityScheduler.cancel();
        return {};
      }
      std::error_code directoryTypeError;
      if (iterator->is_directory(directoryTypeError) && !directoryTypeError) {
        observeFolder(iterator->path());
        if (isFindBmsPrivateStorageDirectory(iterator->path())) {
          iterator.disable_recursion_pending();
        }
        continue;
      }
      std::error_code typeError;
      if (!iterator->is_regular_file(typeError) || typeError) {
        continue;
      }
      const std::filesystem::path path = iterator->path();
      if (asobmshow::bms_chart_file::isBmsChartPath(path)) {
        if (const auto folder = folderScanNodes.find(
                path.parent_path().lexically_normal());
            folder != folderScanNodes.end()) {
          folder->second.containsBms = true;
        }
        scheduleOrdinaryChart(path, hasDocumentForChartPath(path));
        continue;
      }
      if (archive_file::hasSupportedArchiveExtension(path)) {
        scheduleArchivePath(path);
      }
    }
    if (error) {
      traversalHealthy = false;
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
  traversalHealthy =
      traversalHealthy && discoveryHealthy.load(std::memory_order_relaxed);
  scanPhaseEnd("folder-traversal-and-entity-prep", folderTraversalStart);
  const auto reconcileDiffBuildStart = scanPhaseStart();
  scanPhase("reconcile-diff-build");

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
          .hasDocument = ordinary->hasDocument,
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
      } else {
        traversalHealthy = false;
      }
      continue;
    }

    ArchiveScanResult &archiveScan = *prepared.scan;
    if (!archiveScan.readable) {
      traversalHealthy = false;
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
      const bool hasDocument = archiveScan.documentedChartPaths.contains(
          fspath_to_path_t(chartPath));
      ScanDiff diff{
          .path = std::move(chartPath),
          .deleted = false,
          .hasDocument = hasDocument,
      };
      diffs.push_back(std::move(diff));
    }
  }

  reportProgress(rootCount, rootCount,
                 ChartScanProgressStage::PreparingUpdates);
  scanPhaseEnd("reconcile-diff-build", reconcileDiffBuildStart);
  archive_file::appendDebugLogLine(
      "Preparing updates: diffs=" + std::to_string(diffs.size()) +
      " documentFlagUpdates=" + std::to_string(documentFlagUpdates.size()) +
      " checkpointFound=" + std::to_string(checkpoint.found) +
      " checkpointPhase=" +
      (checkpoint.found ? checkpoint.phase : std::string("(none)")));

  if (shouldStop()) {
    entityScheduler.cancel();
    return {};
  }
  const bool noScanWork =
      diffs.empty() && documentFlagUpdates.empty() &&
      sourcePreferenceRefreshPaths.empty() &&
      cachedSourcePreferenceUpdates.empty() && solidArchiveDiffs.empty() &&
      archiveCacheDiffs.empty() && pendingArchiveCacheDiffs.empty() &&
      staleSolidArchives.empty() && reindexedArchives.empty() &&
      folderScanNodes.empty();
  if (noScanWork) {
    entityScheduler.finish();
    for (const auto &exception : entityScheduler.takeExceptions()) {
      (void)exception;
      traversalHealthy = false;
    }
    bool finalized = traversalHealthy && session.ClearScanCheckpoint();
    if (finalized && reconcileMode == ReconcileMode::Full) {
      finalized = session.ClearChartMetadataRebuildRequired();
    }
    return ChartScanResult{.completed = finalized};
  }

  // Archive ordering. The scan signature hashes only roots and archive path +
  // size + mtime (not the per-archive inner paths or the individual diffs), so
  // only the archive order must be deterministic: a checkpoint survives a
  // restart only if both runs project the same archive sequence. Directory
  // iteration (especially over iOS File Provider Storage) is not stable across
  // runs, so sort the archives by path. Per-archive inner paths are collected
  // from the archive's own entry listing, which is deterministic for an
  // unchanged archive, so no per-diff or per-inner-path sorting is needed.
  const auto orderingStart = std::chrono::steady_clock::now();
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
    batchIt->second.hasDocument.push_back(diff.hasDocument);
    batchIt->second.preparedMetas.push_back(std::move(diff.preparedMeta));
  }

  // Sort the archive order by path so the checkpoint signature (which hashes
  // archives in this order) and the archive resume indexes stay deterministic.
  std::ranges::sort(archiveBatchOrder, [&](const path_t &left, const path_t &right) {
    const auto leftIt = archiveBatches.find(left);
    const auto rightIt = archiveBatches.find(right);
    const std::string leftKey =
        leftIt != archiveBatches.end()
            ? checkpointPathTextForDb(leftIt->second.archivePath)
            : checkpointPathTextForDb(left);
    const std::string rightKey =
        rightIt != archiveBatches.end()
            ? checkpointPathTextForDb(rightIt->second.archivePath)
            : checkpointPathTextForDb(right);
    return leftKey < rightKey;
  });
  const auto orderingMillis =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - orderingStart)
          .count();
  archive_file::appendDebugLogLine(
      "Deterministic ordering took " + std::to_string(orderingMillis) +
      "ms for " + std::to_string(individualDiffs.size()) + " individual diffs and " +
      std::to_string(archiveBatchOrder.size()) + " archive batches.");

  int parseTotal = static_cast<int>(individualDiffs.size());
  for (const auto &archiveKey : archiveBatchOrder) {
    const auto batchIt = archiveBatches.find(archiveKey);
    if (batchIt != archiveBatches.end()) {
      parseTotal += static_cast<int>(batchIt->second.innerPaths.size());
    }
  }
  parseTotal = std::max(parseTotal, 1);
  int parseCurrent = 0;

  auto computeScanSignature = [&](std::size_t, std::size_t archiveStart) {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    std::uint64_t hash = kOffset;
    fnv1aAppend(hash, "chart-scan-archives-v1");

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

    // The individual (regular-file) diffs are idempotent and cheap to redo on
    // restart, so they are not checkpointed and need no signature contribution.
    // Only archives are checkpoint-resumed; the signature just needs to detect
    // whether any archive changed (path, size, or mtime), not the per-chart
    // inner-path corpus.
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

  std::size_t resumeStart = archiveBatchOrder.size();
  std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>>
      completedArchiveIdentity;
  completedArchiveIdentity.reserve(scanSnapshot.completedArchives.size());
  for (const auto &record : scanSnapshot.completedArchives) {
    // Key by the same normalized DB path text used for checkpoint identity so
    // the stored path round-trips consistently with the live archive path.
    completedArchiveIdentity[checkpointPathTextForDb(record.path)] = {
        record.size, record.mtimeNs};
  }

  std::unordered_map<path_t, bool> completedArchiveCache;
  auto archiveIdentityCompleted =
      [&](const std::filesystem::path &archivePath) -> bool {
    const path_t archiveKey = archiveScanKey(archivePath);
    if (const auto cached = completedArchiveCache.find(archiveKey);
        cached != completedArchiveCache.end()) {
      return cached->second;
    }
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    bool completed = false;
    if (archiveFileState(archivePath, archiveSize, mtimeNs)) {
      const auto identityIt = completedArchiveIdentity.find(
          checkpointPathTextForDb(archivePath));
      completed = identityIt != completedArchiveIdentity.end() &&
                  identityIt->second.first == archiveSize &&
                  identityIt->second.second == mtimeNs;
    }
    completedArchiveCache.emplace(archiveKey, completed);
    return completed;
  };

  auto archiveBatchFinished =
      [&](const ArchiveParseBatch &batch) -> std::optional<
          std::pair<std::int64_t, std::int64_t>> {
    if (!archiveIdentityCompleted(batch.archivePath)) {
      return std::nullopt;
    }
    std::int64_t archiveSize = 0;
    std::int64_t mtimeNs = 0;
    if (!archiveFileState(batch.archivePath, archiveSize, mtimeNs)) {
      return std::nullopt;
    }
    return std::pair<std::int64_t, std::int64_t>{archiveSize, mtimeNs};
  };

  auto validateArchiveCheckpoint = [&]() {
    const std::size_t subIndex = static_cast<std::size_t>(checkpoint.subIndex);
    archive_file::appendDebugLogLine(
        "Resolving archive checkpoint: subIndex=" +
        std::to_string(subIndex) +
        " archives=" + std::to_string(archiveBatchOrder.size()) +
        " completed=" + std::to_string(completedArchiveIdentity.size()) +
        " ckptArchivePath=" +
        (checkpoint.archivePath.empty()
             ? "(empty)"
             : fspath_to_utf8(checkpoint.archivePath)) +
        " ckptSize=" + std::to_string(checkpoint.archiveSize) +
        " ckptMtime=" + std::to_string(checkpoint.archiveMtimeNs));

    // Archives whose current (path, size, mtime) identity is in the completed
    // set were fully parsed and committed in an earlier (interrupted) run.
    // Their charts are already durable, so skip them regardless of where they
    // sort during this run. The first archive whose identity is not completed
    // is where parsing resumes.
    resumeStart = archiveBatchOrder.size();
    for (std::size_t i = 0; i < archiveBatchOrder.size(); ++i) {
      const auto batchIt = archiveBatches.find(archiveBatchOrder[i]);
      if (batchIt == archiveBatches.end()) {
        continue;
      }
      if (archiveBatchFinished(batchIt->second).has_value()) {
        resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[i]);
      } else {
        resumeStart = i;
        break;
      }
    }
    resumePlan.valid = true;
    resumePlan.archivePhase = true;
    resumePlan.individualStart = individualDiffs.size();
    resumePlan.archiveStart = resumeStart;
    resumePlan.archiveSubStart = 0;

    // Mid-archive continuation: the checkpoint recorded the archive being
    // streamed at interruption (its identity plus the count of charts already
    // durably inserted). If that archive is exactly where this run resumes
    // and the recorded inner path is still at that position in its (stable)
    // entry order, continue from subIndex instead of restarting the archive.
    if (resumeStart < archiveBatchOrder.size()) {
      const auto resumeBatchIt =
          archiveBatches.find(archiveBatchOrder[resumeStart]);
      if (resumeBatchIt != archiveBatches.end()) {
        const ArchiveParseBatch &batch = resumeBatchIt->second;
        const bool archivePathsMatch =
            !checkpoint.archivePath.empty() &&
            checkpointPathTextForDb(batch.archivePath) ==
                checkpointPathTextForDb(checkpoint.archivePath);
        std::int64_t archiveSize = 0;
        std::int64_t mtimeNs = 0;
        if (archivePathsMatch && subIndex > 0 &&
            subIndex <= batch.innerPaths.size() &&
            archiveFileState(batch.archivePath, archiveSize, mtimeNs) &&
            archiveSize == checkpoint.archiveSize &&
            mtimeNs == checkpoint.archiveMtimeNs &&
            checkpointInnerPathText(batch.innerPaths[subIndex - 1]) ==
                checkpoint.lastInnerPath) {
          resumePlan.archiveSubStart = subIndex;
          resumePlan.protectedArchiveKeys.insert(archiveBatchOrder[resumeStart]);
        }
      }
    }

    const std::size_t resumedCount =
        individualDiffs.size() +
        archiveBatchInnerCountBefore(resumeStart) +
        resumePlan.archiveSubStart;
    parseCurrent = static_cast<int>(std::min<std::size_t>(
        resumedCount, static_cast<std::size_t>(parseTotal)));
    return true;
  };

  if (checkpoint.found && checkpoint.phase == kScanCheckpointPhaseArchive &&
      validateArchiveCheckpoint()) {
    archive_file::appendDebugLogLine(
        "Continuing chart scan from archive checkpoint: subIndex=" +
        std::to_string(checkpoint.subIndex) +
        " archiveStart=" + std::to_string(resumePlan.archiveStart) +
        " archiveSubStart=" + std::to_string(resumePlan.archiveSubStart) +
        " protected=" + std::to_string(resumePlan.protectedArchiveKeys.size()));
  } else if (checkpoint.found) {
    resumeStart = 0;
    resumePlan = ResumePlan{};
    session.ClearScanCheckpoint();
    archive_file::appendDebugLogLine(
        "Discarded stale chart scan checkpoint before parsing.");
  }

  auto scanBatch = session.BeginScanBatch();
  archive_file::appendDebugLogLine(
      scanBatch.has_value()
          ? "Chart scan batch begun."
          : "Chart scan batch begin failed.");
  if (!scanBatch.has_value()) {
    entityScheduler.cancel();
    return {};
  }
  bool storageHealthy = true;
  auto recordStorageResult = [&](bool success) {
    storageHealthy = success && storageHealthy;
    return success;
  };
  std::vector<ChartFolderScanNode> scannedFolders;
  scannedFolders.reserve(folderScanNodes.size());
  for (const auto &[_, node] : folderScanNodes) {
    scannedFolders.push_back(node);
  }
  if (!folderScanRoots.empty()) {
    archive_file::appendDebugLogLine(
        "Synchronizing folders: roots=" +
        std::to_string(folderScanRoots.size()) + " nodes=" +
        std::to_string(folderScanNodes.size()));
    const auto syncStart = std::chrono::steady_clock::now();
    recordStorageResult(
        scanBatch->SynchronizeFolders(scannedFolders, folderScanRoots));
    const auto syncMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - syncStart)
            .count();
    archive_file::appendDebugLogLine(
        "Folder synchronization complete in " + std::to_string(syncMillis) +
        "ms.");
  }
  const auto reconcileLoopStart = scanPhaseStart();
  scanPhase("reconcile-update-loops");
  std::vector<std::filesystem::path> upsertedChartPaths;
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
    if (!recordStorageResult(
            scanBatch->CheckpointAndContinue(nextCheckpoint))) {
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
        if (resumePlan.protectedArchiveKeys.find(archiveKey) !=
            resumePlan.protectedArchiveKeys.end()) {
          return true;
        }
        // Protect every archive whose (path, size, mtime) identity is already
        // completed: its charts are durable and the parse loop will skip it,
        // so its rows must not be removed by the reindex deletion pass even
        // if it sorts after the resume point this run.
        return archiveIdentityCompleted(archivePath);
      };

  const auto loopSourcePrefRefreshStart = scanPhaseStart();
  for (const auto &path : sourcePreferenceRefreshPaths) {
    if (shouldStop()) {
      break;
    }
    const auto preference = archive_file::sourcePreferenceForPath(path);
    recordStorageResult(scanBatch->UpdateSourcePreference({
        .path = path,
        .priority = preference.priority,
        .archiveSize = preference.archiveSize,
    }));
  }
  scanPhaseEnd("loop-source-preference-refresh", loopSourcePrefRefreshStart);

  const auto loopCachedSourcePrefStart = scanPhaseStart();
  for (const auto &update : cachedSourcePreferenceUpdates) {
    if (shouldStop()) {
      break;
    }
    std::filesystem::path updateArchivePath;
    std::filesystem::path updateInnerPath;
    if (archive_file::splitVirtualPath(update.path, updateArchivePath,
                                       updateInnerPath) &&
        archiveIdentityCompleted(updateArchivePath)) {
      // The chart belongs to an archive that was fully parsed and committed
      // in an earlier run, and its archive file is unchanged (same size and
      // mtime), so the stored source preference is already current.
      continue;
    }
    recordStorageResult(scanBatch->UpdateSourcePreference({
        .path = update.path,
        .priority = update.priority,
        .archiveSize = update.archiveSize,
    }));
  }
  scanPhaseEnd("loop-cached-source-preference", loopCachedSourcePrefStart);

  const auto loopStaleSolidArchivesStart = scanPhaseStart();
  for (const auto &path : staleSolidArchives) {
    if (shouldStop()) {
      break;
    }
    recordStorageResult(scanBatch->DeleteSolidArchive(path));
    recordStorageResult(scanBatch->DeleteArchiveCache(path));
  }
  scanPhaseEnd("loop-stale-solid-archives", loopStaleSolidArchivesStart);

  const auto loopReindexedArchivesStart = scanPhaseStart();
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
    recordStorageResult(scanBatch->DeleteChartsInArchive(path));
  }
  scanPhaseEnd("loop-reindexed-archives", loopReindexedArchivesStart);

  const auto loopArchiveCacheDiffsStart = scanPhaseStart();
  for (const auto &diff : archiveCacheDiffs) {
    if (shouldStop()) {
      break;
    }
    recordStorageResult(scanBatch->UpsertArchiveCache({
        .path = diff.path,
        .solid = diff.solid,
        .uncompressedSize = diff.uncompressedSize,
        .fileCount = diff.fileCount,
        .chartCount = diff.chartCount,
    }));
  }
  scanPhaseEnd("loop-archive-cache-diffs", loopArchiveCacheDiffsStart);

  const auto loopSolidArchiveDiffsStart = scanPhaseStart();
  for (const auto &diff : solidArchiveDiffs) {
    if (shouldStop()) {
      break;
    }
    if (diff.solid) {
      recordStorageResult(scanBatch->UpsertSolidArchive({
          .path = diff.path,
          .uncompressedSize = diff.uncompressedSize,
          .fileCount = diff.fileCount,
      }));
      recordStorageResult(scanBatch->DeleteChartsInArchive(diff.path));
    } else {
      recordStorageResult(scanBatch->DeleteSolidArchive(diff.path));
    }
  }
  scanPhaseEnd("loop-solid-archive-diffs", loopSolidArchiveDiffsStart);

  auto insertIndividualChartMeta = [&](ParsedChartMetadata &parsed,
                                       bool hasDocument) -> bool {
    if (!recordStorageResult(
            scanBatch->UpsertChart(parsed.meta, std::nullopt, hasDocument,
                                   parsed.sequenceFeatures))) {
      return false;
    }
    if (!reconcileExisting) {
      upsertedChartPaths.push_back(parsed.meta.BmsPath);
    }
    return true;
  };

  const auto loopDocumentFlagStart = scanPhaseStart();
  for (const auto &[path, hasDocument] : documentFlagUpdates) {
    if (shouldStop()) {
      break;
    }
    std::filesystem::path flagArchivePath;
    std::filesystem::path flagInnerPath;
    if (archive_file::splitVirtualPath(path, flagArchivePath, flagInnerPath) &&
        archiveIdentityCompleted(flagArchivePath)) {
      // The chart belongs to an unchanged, previously completed archive, so
      // its stored document flag is already current.
      continue;
    }
    recordStorageResult(scanBatch->UpdateChartHasDocument(path, hasDocument));
  }
  scanPhaseEnd("loop-document-flag-updates", loopDocumentFlagStart);
  scanPhaseEnd("reconcile-update-loops", reconcileLoopStart);
  const auto individualParseStart = scanPhaseStart();
  scanPhase("individual-chart-parse");

  auto individualParseWorkerCount = [](std::size_t fileCount) {
    return static_cast<std::size_t>(parallel_worker_count(fileCount));
  };

  auto parseIndividualChartBatch = [&](std::size_t begin, std::size_t end)
      -> std::vector<std::optional<ParsedChartMetadata>> {
    using Clock = std::chrono::steady_clock;
    const std::size_t count = end > begin ? end - begin : 0;
    std::vector<std::optional<ParsedChartMetadata>> parsedMetas(count);
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
      recordStorageResult(scanBatch->DeleteChart(diff.path));
      ++parseCurrent;
      diffIndex = diffIndex + 1;
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
        insertIndividualChartMeta(*parsedMetas[offset],
                                  individualDiffs[currentIndex].hasDocument);
      }
      ++parseCurrent;
    }
    diffIndex = batchEnd;
    // Commit periodically so individually-parsed charts are durable even if
    // the app is killed mid-scan. On restart the diffs are recomputed and any
    // already-committed charts become no-ops; no resume position is needed.
    if (!shouldStop() && (diffIndex % kIndividualCommitInterval == 0)) {
      if (!recordStorageResult(scanBatch->CommitForProgress())) {
        break;
      }
    }
  }

  scanPhaseEnd("individual-chart-parse", individualParseStart);
  const auto archiveScheduleStart = scanPhaseStart();
  scanPhase("archive-schedule-and-stream");
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
    recordStorageResult(scanBatch->UpsertArchiveCache({
        .path = diff.path,
        .solid = diff.solid,
        .uncompressedSize = diff.uncompressedSize,
        .fileCount = diff.fileCount,
        .chartCount = parsedChartCount,
    }));
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
        !prefetchedArchiveIndexes.contains(archiveIndex) &&
        !archiveBatchFinished(*batch).has_value()) {
      unpreparedArchiveIndexes.push_back(archiveIndex);
    }
  }

  std::optional<std::size_t> directConcurrentArchiveIndex;
  if (unpreparedArchiveIndexes.size() == 1 && !shouldStop() &&
      entityScheduler.isIdle()) {
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
    if (archiveBatchFinished(batch).has_value()) {
      // This archive was fully parsed and committed in an earlier run; its
      // charts are already durable, so skip it even if it sorts after the
      // resume point this run.
      archive_file::appendDebugLogLine(
          "Skipping already-completed archive batch: " +
          fspath_to_utf8(batch.archivePath));
      continue;
    }
    archive_file::appendDebugLogLine(
        "Processing archive batch: index=" +
        std::to_string(archiveIndex) + "/" +
        std::to_string(archiveBatchOrder.size()) + " path=" +
        fspath_to_utf8(archiveBatchOrder[archiveIndex]) + " inner=" +
        std::to_string(batch.innerPaths.size()));
    const std::size_t innerStart = archiveInnerStartForIndex(archiveIndex);
    if (innerStart >= batch.innerPaths.size()) {
      int storedChartCount = 0;
      if (innerStart > 0) {
        const auto count =
            scanBatch->CountChartsInArchive(batch.archivePath);
        if (!recordStorageResult(count.has_value())) {
          continue;
        }
        storedChartCount = *count;
      }
      writePendingArchiveCache(batch, storedChartCount);
      std::int64_t completedArchiveSize = 0;
      std::int64_t completedArchiveMtimeNs = 0;
      if (archiveFileState(batch.archivePath, completedArchiveSize,
                           completedArchiveMtimeNs)) {
        recordStorageResult(scanBatch->RecordCompletedArchive(
            batch.archivePath, completedArchiveSize,
            completedArchiveMtimeNs));
      }
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
    std::vector<std::filesystem::path> pendingChartPaths;
    pendingChartPaths.reserve(requestedCharts);
    for (std::size_t innerIndex = innerStart;
         innerIndex < batch.innerPaths.size(); ++innerIndex) {
      pendingChartPaths.push_back(archive_file::makeVirtualPath(
          batch.archivePath, batch.innerPaths[innerIndex]));
    }
    bool archiveStorageHealthy = true;
    if (innerStart > 0 &&
        !recordStorageResult(scanBatch->DeleteCharts(pendingChartPaths))) {
      archiveStorageHealthy = false;
      archive_file::appendDebugLogLine(
          "Failed to clear uncheckpointed archive chart rows: " +
          archiveText);
    }
    int storedChartCount = 0;
    if (innerStart > 0) {
      const auto count = scanBatch->CountChartsInArchive(batch.archivePath);
      if (recordStorageResult(count.has_value())) {
        storedChartCount = *count;
      } else {
        archiveStorageHealthy = false;
      }
    }
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
          const bool hasDocument =
              parsedInBatch < batch.hasDocument.size() &&
              batch.hasDocument[parsedInBatch];
          if (recordStorageResult(scanBatch->UpsertChart(
                  parsed.meta->meta, storedArchiveSourcePreference,
                  hasDocument, parsed.meta->sequenceFeatures))) {
            ++insertedCharts;
            ++storedChartCount;
            if (!reconcileExisting) {
              upsertedChartPaths.push_back(parsed.meta->meta.BmsPath);
            }
          } else {
            archiveStorageHealthy = false;
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
      // A single archive that cannot be read (encrypted, passphrase-locked,
      // corrupt, or an unsupported entry) must not poison the whole library
      // refresh. Skip it: its charts are simply not added this scan and will
      // be discovered on a later successful pass. Only storage/DB-level
      // failures set archiveStorageHealthy=false.
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
            !recordStorageResult(
                scanBatch->DeleteCharts(pendingChartPaths))) {
          archiveStorageHealthy = false;
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
    if (parsedFullBatch && archiveStorageHealthy &&
        !stopRequested(stopToken)) {
      writePendingArchiveCache(batch, storedChartCount);
      std::int64_t completedArchiveSize = 0;
      std::int64_t completedArchiveMtimeNs = 0;
      if (archiveFileState(batch.archivePath, completedArchiveSize,
                           completedArchiveMtimeNs)) {
        recordStorageResult(scanBatch->RecordCompletedArchive(
            batch.archivePath, completedArchiveSize,
            completedArchiveMtimeNs));
      }
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
          "complete cleanly: " +
          archiveText + " requested=" + std::to_string(requestedCharts) +
          " files=" + std::to_string(deliveredCharts));
    }
  }
  scanPhaseEnd("archive-schedule-and-stream", archiveScheduleStart);
  if (shouldStop()) {
    archiveParseScheduler->cancel();
  } else {
    archiveParseScheduler->finish();
  }
  for (const auto &exception : archiveParseScheduler->takeExceptions()) {
    traversalHealthy = false;
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
  const bool commitSucceeded = traversalHealthy && scanBatch->Commit();
  if (!traversalHealthy) {
    archive_file::appendDebugLogLine(
        "Discarded chart scan batch after incomplete source traversal.");
  } else if (!commitSucceeded) {
    archive_file::appendDebugLogLine(
        "Failed to commit final chart scan batch.");
  }
  if (stopRequested(stopToken)) {
    interrupted.store(true, std::memory_order_relaxed);
  }
  acknowledgeFlushRequest(pendingFlushRequest());
  bool committed = storageHealthy && traversalHealthy && commitSucceeded;
  if (!stopRequested(stopToken) && committed) {
    bool finalized = session.ClearScanCheckpoint();
    if (finalized && reconcileMode == ReconcileMode::Full) {
      finalized = session.ClearChartMetadataRebuildRequired();
    }
    committed = finalized;
    // Drop persisted archive index files for archives that are no longer
    // present, so the disk cache does not grow with removed archives.
    if (committed) {
      const std::size_t pruned = archive_file::pruneArchiveIndexCache(
          liveArchivePaths);
      if (pruned > 0) {
        archive_file::appendDebugLogLine(
            "Pruned " + std::to_string(pruned) +
            " orphaned archive index cache files after library refresh.");
      }
    }
  }
  if (!committed) {
    upsertedChartPaths.clear();
  }
  return ChartScanResult{
      .changedCount = changedCount,
      .completed = !interrupted.load(std::memory_order_relaxed) && committed,
      .committed = committed,
      .upsertedChartPaths = std::move(upsertedChartPaths),
  };
}
