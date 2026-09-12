#pragma once

#include "ArchiveAssetPipeline.h"

#include <unordered_set>

namespace audio {

template <
    typename ConcurrentReader = decltype(&archive_file::readArchiveEntriesConcurrently),
    typename StreamingReader = decltype(&archive_file::readArchiveEntriesStreamingBounded)>
bool ConsumeArchiveAssetBatch(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    std::size_t workerCount, std::uint64_t maximumBytes,
    archive_file::FileDataCallback consume, std::string *errorMessage,
    std::atomic_bool &isCancelled,
    ConcurrentReader readConcurrently = archive_file::readArchiveEntriesConcurrently,
    StreamingReader readStreaming = archive_file::readArchiveEntriesStreamingBounded) {
  auto checkpoint = [&] { return !isCancelled.load(std::memory_order_relaxed); };
  if (!checkpoint()) {
    return false;
  }
  if (innerPaths.empty()) {
    return true;
  }
  auto reportMemoryLimit = [&] {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive asset exceeds the encoded memory limit.";
    }
  };
  if (maximumBytes == 0) {
    reportMemoryLimit();
    return false;
  }
  if (innerPaths.size() == 1 || workerCount <= 1 || maximumBytes == 1) {
    std::unordered_set<path_t> acceptedPaths;
    bool rejected = false;
    auto continueReading = [&] { return checkpoint() && !rejected; };
    auto onFile = [&](archive_file::FileData &&file) {
      if (!continueReading()) {
        return false;
      }
      if (file.bytes.capacity() > maximumBytes) {
        rejected = true;
        reportMemoryLimit();
        return false;
      }
      if (acceptedPaths.insert(fspath_to_path_t(file.path)).second &&
          !consume(std::move(file))) {
        rejected = true;
      }
      return continueReading();
    };
    const bool read = readStreaming(archivePath, innerPaths, onFile, maximumBytes,
                                   errorMessage, continueReading);
    return read && continueReading() && acceptedPaths.size() == innerPaths.size();
  }
  const std::size_t extractWorkers = workerCount >= 4 ? 2 : 0;
  const auto extractionBytes = maximumBytes / 2;
  const auto pipelineBytes = maximumBytes - extractionBytes;
  ArchiveAssetPipeline pipeline(
      workerCount - std::max<std::size_t>(1, extractWorkers),
      pipelineBytes, isCancelled, std::move(consume));
  std::unordered_set<path_t> acceptedPaths;
  std::mutex acceptedMutex;
  std::atomic_bool rejected = false;
  std::atomic_bool exceededMemory = false;
  auto continueReading = [&] {
    return checkpoint() && !rejected && pipeline.accepting();
  };
  auto onFile = [&](archive_file::FileData &&file) {
    if (file.bytes.capacity() > extractionBytes) {
      exceededMemory = true;
      rejected = true;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(acceptedMutex);
      if (!acceptedPaths.insert(fspath_to_path_t(file.path)).second) {
        return continueReading() && !rejected;
      }
    }
    if (!pipeline.push(std::move(file))) {
      rejected = true;
      return false;
    }
    return true;
  };
  bool read = false;
  if (extractWorkers > 1) {
    read = readConcurrently(archivePath, innerPaths, onFile, extractWorkers,
                           extractionBytes, errorMessage, continueReading);
  }
  if ((!read || acceptedPaths.size() != innerPaths.size()) &&
      continueReading() && !rejected) {
    std::vector<std::filesystem::path> missing;
    for (const auto &path : innerPaths) {
      if (!acceptedPaths.contains(fspath_to_path_t(path))) {
        missing.push_back(path);
      }
    }
    read = missing.empty() ||
           readStreaming(archivePath, missing, onFile, extractionBytes,
                         errorMessage, continueReading);
  }
  const bool consumed = pipeline.finish();
  if (exceededMemory) {
    reportMemoryLimit();
  }
  if (read && consumed && !rejected && acceptedPaths.size() == innerPaths.size()) {
    if (errorMessage != nullptr) {
      errorMessage->clear();
    }
    return true;
  }
  return false;
}

}
