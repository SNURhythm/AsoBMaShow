#pragma once

#include "ParallelWork.h"

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>

// One control thread submits synchronous batches. Workers persist between
// batches so progressive loading does not create threads for every time window.
class ParallelWorkBatch {
public:
  explicit ParallelWorkBatch(std::size_t maximumTasks)
      : workerCount_(parallel_worker_count(maximumTasks)) {}
  ~ParallelWorkBatch() { stop(); }

  template <typename Func>
  void run(std::size_t count, Func &&work) {
    if (count == 0) return;
    if (count == 1 || workerCount_ <= 1) {
      for (std::size_t index = 0; index < count; ++index) work(index);
      return;
    }
    if (threads_.empty()) {
      try {
        for (unsigned int i = 0; i < workerCount_; ++i)
          threads_.emplace_back([this] { worker(); });
      } catch (...) {
        stop();
        throw;
      }
    }
    std::unique_lock lock(mutex_);
    work_ = [&](std::size_t index) { work(index); };
    next_ = 0;
    count_ = count;
    failure_ = nullptr;
    pending_ = threads_.size();
    ++generation_;
    changed_.notify_all();
    finished_.wait(lock, [&] { return pending_ == 0; });
    work_ = {};
    if (failure_) std::rethrow_exception(failure_);
  }

private:
  void worker() {
    std::size_t observed = 0;
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [&] { return stopping_ || generation_ != observed; });
      if (stopping_) return;
      observed = generation_;
      while (next_ < count_ && !failure_) {
        const auto index = next_++;
        lock.unlock();
        std::exception_ptr failure;
        try {
          work_(index);
        } catch (...) {
          failure = std::current_exception();
        }
        lock.lock();
        if (failure && !failure_) failure_ = failure;
      }
      if (--pending_ == 0) finished_.notify_one();
    }
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    changed_.notify_all();
    for (auto &thread : threads_)
      if (thread.joinable()) thread.join();
  }

  const unsigned int workerCount_;
  std::mutex mutex_;
  std::condition_variable changed_, finished_;
  std::function<void(std::size_t)> work_;
  std::size_t next_ = 0, count_ = 0, pending_ = 0, generation_ = 0;
  std::exception_ptr failure_;
  bool stopping_ = false;
  std::vector<std::jthread> threads_;
};
