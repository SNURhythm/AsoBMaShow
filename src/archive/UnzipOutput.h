#pragma once

#include "UnzipTypes.h"
#include "../ThreadCompat.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <memory>

namespace archive_file {

// Backend-independent extraction policy. Stop is checked before pause; a
// rejected checkpoint reports the same cancellation to every backend.
bool unzipCheckpoint(const std::stop_token *stopToken,
                     const PauseCallback &pauseCallback,
                     std::string *errorMessage = nullptr);

// One guard per archive. Shared budget mutation and disk-space admission are
// serialized by UnzipBudget::mutex; writeMutex_ serializes this archive's
// stream writes. The last admitted disk reservation lives until the next
// consume or guard destruction. It is not a count of durable output bytes.
class UnzipWriteGuard final {
public:
  UnzipWriteGuard(UnzipBudget &, std::filesystem::path destination);
  ~UnzipWriteGuard();

  bool admit(std::uint64_t bytes);
  bool admitEntries(std::uint64_t entries);
  bool rejectEntryLimit();
  bool consume(std::uint64_t bytes);
  bool write(std::ostream &output, const void *data, std::size_t size);
  // Inspect after backend/output work has completed.
  const std::string &error() const { return error_; }

private:
  bool check(std::uint64_t bytes);
  bool reject(std::string message);

  UnzipBudget &budget_;
  std::filesystem::path destination_;
  std::uint64_t archiveBytes_ = 0;
  std::uint64_t pendingBytes_ = 0;
  std::string error_;
  std::mutex writeMutex_;
};

// Owns bounded staging, queued output streams, and the optional writer thread.
// Backend callers own output paths and recovery markers. They must stop
// producing before destruction; the guard and stop token outlive this owner.
// Flush drains admitted output; destruction also joins before dependencies die.
class UnzipOutputPipeline final {
public:
  UnzipOutputPipeline(UnzipWriteGuard &, const UnzipExecutionPlan &,
                      const std::stop_token *, PauseCallback);
  ~UnzipOutputPipeline();

  bool enabled() const { return worker_.joinable(); }
  std::size_t bufferBudget() const { return enabled() ? capacity_ : 0; }
  bool write(const std::shared_ptr<std::ofstream> &output,
             const void *data, std::size_t size);
  bool flush();
  bool cancelled() const { return cancelled_; }

private:
  struct Chunk {
    std::shared_ptr<std::ofstream> output;
    std::unique_ptr<char[]> bytes;
    std::size_t size = 0;
    std::size_t capacity = 0;
  };
  bool enqueueStaged();
  void run();

  UnzipWriteGuard &guard_;
  const std::stop_token *stopToken_;
  PauseCallback pause_;
  std::size_t capacity_;
  std::mutex stagingMutex_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Chunk> queue_;
  std::shared_ptr<std::ofstream> stagedOutput_;
  std::unique_ptr<char[]> stagedBytes_;
  std::size_t stagedSize_ = 0;
  std::size_t pendingBytes_ = 0;
  bool stopping_ = false;
  bool failed_ = false;
  std::atomic_bool cancelled_ = false;
  std::jthread worker_;
};

} // namespace archive_file
