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
};
