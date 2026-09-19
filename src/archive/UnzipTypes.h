#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace archive_file {

using PauseCallback = std::function<bool()>;

struct UnzipLimits {
  std::uint64_t maximumArchiveBytes = 256ull * 1024 * 1024 * 1024;
  std::uint64_t maximumTotalBytes = 1024ull * 1024 * 1024 * 1024;
  std::uint64_t reservedFreeBytes = 512ull * 1024 * 1024;
  std::size_t maximumConcurrentArchives = 0;
  std::size_t maximumWorkers = 0;
  std::uint64_t maximumMemoryBytes = 0;
  std::uint64_t maximumArchiveEntries = 100000;
  std::uint64_t maximumTotalEntries = 1000000;
};

struct UnzipExecutionPlan {
  std::size_t archiveWorkers = 1;
  std::size_t workersPerArchive = 1;
  std::uint64_t memoryPerArchive = 0;
};

struct UnzipBudget {
  UnzipLimits limits;
  std::uint64_t writtenBytes = 0;
  std::atomic_bool exhausted = false;
  std::mutex mutex;
  std::uint64_t pendingWriteBytes = 0;
  std::string failureMessage;
  std::size_t concurrentArchives = 1;
  std::uint64_t admittedEntries = 0;
};

} // namespace archive_file
