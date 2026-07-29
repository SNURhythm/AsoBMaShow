#include "ChartScanWorkScheduler.h"

#include <algorithm>
#include <utility>

namespace chart_scan {

WorkScheduler::WorkScheduler(std::size_t workerCount) {
  workers_.reserve(std::max<std::size_t>(1, workerCount));
  for (std::size_t index = 0;
       index < std::max<std::size_t>(1, workerCount); ++index) {
    workers_.emplace_back([this] { workerLoop(); });
  }
}

WorkScheduler::~WorkScheduler() { cancel(); }

bool WorkScheduler::enqueue(Work work) {
  if (!work) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) {
      return false;
    }
    queue_.push_back(std::move(work));
  }
  cv_.notify_one();
  return true;
}

void WorkScheduler::finish() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
  joinWorkers();
}

void WorkScheduler::cancel() {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cancelled_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  joinWorkers();
}

std::vector<std::exception_ptr> WorkScheduler::takeExceptions() {
  std::lock_guard lock(mutex_);
  std::vector<std::exception_ptr> result = std::move(exceptions_);
  exceptions_.clear();
  return result;
}

void WorkScheduler::workerLoop() {
  for (;;) {
    Work work;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock,
               [this] { return cancelled_ || closed_ || !queue_.empty(); });
      if (cancelled_) {
        return;
      }
      if (queue_.empty()) {
        if (closed_) {
          return;
        }
        continue;
      }
      work = std::move(queue_.front());
      queue_.pop_front();
    }

    try {
      work();
    } catch (...) {
      std::lock_guard lock(mutex_);
      exceptions_.push_back(std::current_exception());
    }
  }
}

void WorkScheduler::joinWorkers() {
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

} // namespace chart_scan
