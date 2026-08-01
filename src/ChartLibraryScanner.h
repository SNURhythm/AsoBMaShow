#pragma once

#include "ThreadCompat.h"
#include "repositories/ChartRepository.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

enum class ChartScanProgressStage {
  Preparing,
  ScanningRoots,
  IndexingArchives,
  PreparingUpdates,
  RemovingDeleted,
  ParsingCharts,
  ReadingArchive,
};

struct ChartScanProgress {
  int current = 0;
  int total = 0;
  ChartScanProgressStage stage = ChartScanProgressStage::Preparing;
};

struct ChartScanResult {
  int changedCount = 0;
  // The requested traversal finished without cancellation or a storage error.
  bool completed = false;
  // Every observed batch write and the final transaction commit succeeded.
  bool committed = false;
  // Chart paths successfully upserted by this invocation and committed.
  std::vector<std::filesystem::path> upsertedChartPaths;
};

using ChartScanProgressCallback =
    std::function<void(const ChartScanProgress &)>;
using ChartScanPauseCallback = std::function<bool()>;
using ChartScanFlushRequestCallback = std::function<std::uint64_t()>;
using ChartScanFlushCompleteCallback = std::function<void(std::uint64_t)>;

class ChartLibraryScanner {
public:
  int Scan(ChartRepository::Session &session,
           const std::vector<std::filesystem::path> &roots,
           const std::stop_token *stopToken = nullptr,
           ChartScanProgressCallback progressCallback = nullptr,
           ChartScanPauseCallback pauseCallback = nullptr,
           ChartScanFlushRequestCallback flushRequestCallback = nullptr,
           ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);

  int ScanAdded(
      ChartRepository::Session &session,
      const std::vector<std::filesystem::path> &roots,
      const std::stop_token *stopToken = nullptr,
      ChartScanProgressCallback progressCallback = nullptr,
      ChartScanPauseCallback pauseCallback = nullptr,
      ChartScanFlushRequestCallback flushRequestCallback = nullptr,
      ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);

  ChartScanResult ScanWithResult(
      ChartRepository::Session &session,
      const std::vector<std::filesystem::path> &roots,
      const std::stop_token *stopToken = nullptr,
      ChartScanProgressCallback progressCallback = nullptr,
      ChartScanPauseCallback pauseCallback = nullptr,
      ChartScanFlushRequestCallback flushRequestCallback = nullptr,
      ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);

  ChartScanResult ScanAddedWithResult(
      ChartRepository::Session &session,
      const std::vector<std::filesystem::path> &roots,
      const std::stop_token *stopToken = nullptr,
      ChartScanProgressCallback progressCallback = nullptr,
      ChartScanPauseCallback pauseCallback = nullptr,
      ChartScanFlushRequestCallback flushRequestCallback = nullptr,
      ChartScanFlushCompleteCallback flushCompleteCallback = nullptr);

private:
  ChartScanResult ScanImpl(
      ChartRepository::Session &session,
      const std::vector<std::filesystem::path> &roots,
      bool reconcileExisting,
      const std::stop_token *stopToken,
      ChartScanProgressCallback progressCallback,
      ChartScanPauseCallback pauseCallback,
      ChartScanFlushRequestCallback flushRequestCallback,
      ChartScanFlushCompleteCallback flushCompleteCallback);
};
