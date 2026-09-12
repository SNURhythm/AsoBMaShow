#pragma once

#include "../ChartLibraryScanner.h"

#include <mutex>

namespace archive_unzip_recovery {

struct Result {
  bool completed = false;
  bool libraryChanged = false;
};

std::timed_mutex &operationMutex();
std::unique_lock<std::timed_mutex> acquireOperationLock(
    const std::stop_token &stopToken, ChartScanPauseCallback checkpoint = nullptr);
bool deleteArchiveRecords(ChartRepository::Session &session,
                          const std::vector<std::filesystem::path> &archives);
Result recover(ChartRepository::Session &session,
               const std::stop_token &stopToken = {},
               ChartScanProgressCallback progress = nullptr,
               ChartScanPauseCallback checkpoint = nullptr);

}
