#include "UnzipOutput.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace archive_file {

bool unzipCheckpoint(const std::stop_token *stopToken,
                     const PauseCallback &pauseCallback,
                     std::string *errorMessage) {
  if ((stopToken == nullptr || !stopToken->stop_requested()) &&
      (!pauseCallback || pauseCallback())) {
    return true;
  }
  if (errorMessage != nullptr) *errorMessage = "Unzip cancelled";
  return false;
}

UnzipWriteGuard::UnzipWriteGuard(UnzipBudget &budget,
                                 std::filesystem::path destination)
    : budget_(budget), destination_(std::move(destination)) {}

UnzipWriteGuard::~UnzipWriteGuard() {
  std::lock_guard lock(budget_.mutex);
  budget_.pendingWriteBytes -= pendingBytes_;
}

bool UnzipWriteGuard::admit(std::uint64_t bytes) {
  std::lock_guard lock(budget_.mutex);
  return check(bytes);
}

bool UnzipWriteGuard::admitEntries(std::uint64_t entries) {
  std::lock_guard lock(budget_.mutex);
  if (budget_.exhausted || entries > budget_.limits.maximumArchiveEntries ||
      budget_.admittedEntries > budget_.limits.maximumTotalEntries ||
      entries > budget_.limits.maximumTotalEntries - budget_.admittedEntries) {
    return reject("Unzip entry-count limit exceeded. Original archive kept.");
  }
  budget_.admittedEntries += entries;
  return true;
}

bool UnzipWriteGuard::rejectEntryLimit() {
  std::lock_guard lock(budget_.mutex);
  return reject("Unzip entry-count limit exceeded. Original archive kept.");
}

bool UnzipWriteGuard::consume(std::uint64_t bytes) {
  std::lock_guard lock(budget_.mutex);
  budget_.pendingWriteBytes -= std::exchange(pendingBytes_, 0);
  if (!check(bytes)) return false;
  archiveBytes_ += bytes;
  budget_.writtenBytes += bytes;
  budget_.pendingWriteBytes += bytes;
  pendingBytes_ = bytes;
  return true;
}

bool UnzipWriteGuard::write(std::ostream &output, const void *data,
                            std::size_t size) {
  std::lock_guard lock(writeMutex_);
  if (!consume(size)) return false;
  output.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  output.flush();
  return static_cast<bool>(output);
}

bool UnzipWriteGuard::check(std::uint64_t bytes) {
  if (!error_.empty()) return false;
  if (budget_.exhausted ||
      archiveBytes_ > budget_.limits.maximumArchiveBytes ||
      bytes > budget_.limits.maximumArchiveBytes - archiveBytes_ ||
      budget_.writtenBytes > budget_.limits.maximumTotalBytes ||
      bytes > budget_.limits.maximumTotalBytes - budget_.writtenBytes) {
    return reject("Unzip expanded-byte limit exceeded. Original archive kept.");
  }
  std::error_code error;
  const auto space = std::filesystem::space(destination_, error);
  if (error || space.available == std::numeric_limits<std::uintmax_t>::max()) {
    return reject("Could not check unzip free-space. Original archive kept.");
  }
  if (space.available < budget_.limits.reservedFreeBytes ||
      budget_.pendingWriteBytes > space.available - budget_.limits.reservedFreeBytes ||
      bytes > space.available - budget_.limits.reservedFreeBytes - budget_.pendingWriteBytes) {
    return reject("Unzip reserved free-space limit reached. Original archive kept.");
  }
  return true;
}

bool UnzipWriteGuard::reject(std::string message) {
  if (budget_.failureMessage.empty()) budget_.failureMessage = std::move(message);
  budget_.exhausted = true;
  error_ = budget_.failureMessage;
  return false;
}

UnzipOutputPipeline::UnzipOutputPipeline(
    UnzipWriteGuard &guard, const UnzipExecutionPlan &plan,
    const std::stop_token *stopToken, PauseCallback pause)
    : guard_(guard), stopToken_(stopToken), pause_(std::move(pause)),
      capacity_(static_cast<std::size_t>(std::min<std::uint64_t>(
          8 * 1024 * 1024, plan.memoryPerArchive / 8))) {
  if (plan.workersPerArchive > 1 && capacity_ >= 64 * 1024) {
    worker_ = std::jthread([this] { run(); });
  }
}

UnzipOutputPipeline::~UnzipOutputPipeline() {
  flush();
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) worker_.join();
}

bool UnzipOutputPipeline::write(const std::shared_ptr<std::ofstream> &output,
                                const void *data, std::size_t size) {
  std::lock_guard stagingLock(stagingMutex_);
  if (!enabled()) return guard_.write(*output, data, size);
  if (stagedOutput_ != output && !enqueueStaged()) return false;
  const auto *bytes = static_cast<const char *>(data);
  const auto chunkSize = std::min<std::size_t>(1024 * 1024, capacity_ / 2);
  while (size > 0) {
    stagedOutput_ = output;
    const auto count = std::min(size, chunkSize - stagedSize_);
    if (!stagedBytes_) stagedBytes_.reset(new char[chunkSize]);
    std::memcpy(stagedBytes_.get() + stagedSize_, bytes, count);
    stagedSize_ += count;
    bytes += count;
    size -= count;
    if (stagedSize_ == chunkSize && !enqueueStaged()) return false;
  }
  return true;
}

bool UnzipOutputPipeline::flush() {
  std::lock_guard stagingLock(stagingMutex_);
  try {
    enqueueStaged();
  } catch (...) {
    std::lock_guard lock(mutex_);
    failed_ = true;
  }
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] { return pendingBytes_ == 0; });
  return !failed_;
}

bool UnzipOutputPipeline::enqueueStaged() {
  if (stagedSize_ == 0) return true;
  const auto chunkSize = std::min<std::size_t>(1024 * 1024, capacity_ / 2);
  std::unique_lock lock(mutex_);
  changed_.wait(lock, [&] {
    return failed_ || (pendingBytes_ + chunkSize <= capacity_ - chunkSize &&
                       queue_.size() < 128);
  });
  if (failed_) {
    stagedBytes_.reset();
    stagedSize_ = 0;
    stagedOutput_.reset();
    return false;
  }
  queue_.emplace_back();
  auto &chunk = queue_.back();
  chunk.output = std::move(stagedOutput_);
  chunk.bytes = std::move(stagedBytes_);
  chunk.size = std::exchange(stagedSize_, 0);
  chunk.capacity = chunkSize;
  pendingBytes_ += chunkSize;
  lock.unlock();
  changed_.notify_all();
  return true;
}

void UnzipOutputPipeline::run() {
  for (;;) {
    Chunk chunk;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) return;
      chunk = std::move(queue_.front());
      queue_.pop_front();
    }
    bool success = false;
    try {
      if (!unzipCheckpoint(stopToken_, pause_)) {
        cancelled_ = true;
      } else {
        success = guard_.write(*chunk.output, chunk.bytes.get(), chunk.size);
      }
    } catch (...) {
      success = false;
    }
    const auto size = chunk.capacity;
    chunk.bytes.reset();
    chunk.output.reset();
    {
      std::lock_guard lock(mutex_);
      pendingBytes_ -= size;
      if (!success) {
        failed_ = true;
        queue_.clear();
        pendingBytes_ = 0;
      }
    }
    changed_.notify_all();
  }
}

} // namespace archive_file
