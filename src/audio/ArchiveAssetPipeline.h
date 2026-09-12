#pragma once

#include "../ArchiveFile.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace audio {

class ArchiveAssetPipeline {
public:
  ArchiveAssetPipeline(std::size_t workerCount, std::uint64_t maximumBytes,
                       std::atomic_bool &cancelled,
                       archive_file::FileDataCallback consumer)
      : maximumBytes_(maximumBytes),
        maximumFiles_(std::max<std::size_t>(1, workerCount) * 2),
        cancelled_(cancelled), consumer_(std::move(consumer)) {
    try {
      for (std::size_t worker = 0; worker < std::max<std::size_t>(1, workerCount);
           ++worker) {
        workers_.emplace_back([this] { consume(); });
      }
    } catch (...) {
      abort();
      throw;
    }
  }

  ~ArchiveAssetPipeline() { abort(); }

  bool push(archive_file::FileData &&file) {
    const auto bytes = std::max<std::uint64_t>(1, file.bytes.capacity());
    if (bytes > maximumBytes_) {
      return false;
    }
    std::unique_lock lock(mutex_);
    while (!stopped() && !closed_ &&
           (residentFiles_ >= maximumFiles_ ||
            residentBytes_ > maximumBytes_ - bytes)) {
      changed_.wait_for(lock, std::chrono::milliseconds(5));
    }
    if (stopped() || closed_) {
      return false;
    }
    files_.push_back(std::move(file));
    residentBytes_ += bytes;
    ++residentFiles_;
    changed_.notify_all();
    return true;
  }

  bool finish() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    changed_.notify_all();
    join();
    if (failure_) {
      std::rethrow_exception(failure_);
    }
    return !stopped();
  }

  std::uint64_t inFlightBytes() const {
    std::lock_guard lock(mutex_);
    return residentBytes_;
  }

  bool accepting() const {
    std::lock_guard lock(mutex_);
    return !closed_ && !stopped();
  }

private:
  bool stopped() const {
    return aborted_ || failure_ || cancelled_.load(std::memory_order_relaxed);
  }

  void consume() {
    for (;;) {
      archive_file::FileData file;
      std::uint64_t bytes = 0;
      {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stopped() || closed_ || !files_.empty(); });
        if (stopped()) {
          for (const auto &pending : files_) {
            residentBytes_ -= std::max<std::uint64_t>(1, pending.bytes.capacity());
          }
          residentFiles_ -= files_.size();
          files_.clear();
          changed_.notify_all();
          return;
        }
        if (files_.empty()) {
          return;
        }
        file = std::move(files_.front());
        files_.pop_front();
        bytes = std::max<std::uint64_t>(1, file.bytes.capacity());
      }
      try {
        if (!consumer_(std::move(file))) {
          std::lock_guard lock(mutex_);
          aborted_ = true;
        }
      } catch (...) {
        std::lock_guard lock(mutex_);
        if (!failure_) {
          failure_ = std::current_exception();
        }
      }
      file = {};
      {
        std::lock_guard lock(mutex_);
        residentBytes_ -= bytes;
        --residentFiles_;
      }
      changed_.notify_all();
    }
  }

  void join() {
    for (auto &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void abort() {
    {
      std::lock_guard lock(mutex_);
      aborted_ = true;
    }
    changed_.notify_all();
    join();
  }

  const std::uint64_t maximumBytes_;
  const std::size_t maximumFiles_;
  std::atomic_bool &cancelled_;
  archive_file::FileDataCallback consumer_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<archive_file::FileData> files_;
  std::uint64_t residentBytes_ = 0;
  std::size_t residentFiles_ = 0;
  bool closed_ = false;
  bool aborted_ = false;
  std::exception_ptr failure_;
  std::vector<std::thread> workers_;
};

}
