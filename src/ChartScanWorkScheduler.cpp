#include "ChartScanWorkScheduler.h"

#include <algorithm>
#include <utility>

namespace chart_scan {

WorkScheduler::WorkScheduler(std::size_t workerCount,
                             std::size_t archiveIoLimit) {
  const std::size_t actualWorkerCount = std::max<std::size_t>(1, workerCount);
  archiveIoLimit_ =
      std::clamp<std::size_t>(archiveIoLimit, 1, actualWorkerCount);
  workers_.reserve(actualWorkerCount);
  for (std::size_t index = 0; index < actualWorkerCount; ++index) {
    workers_.emplace_back([this] { workerLoop(); });
  }
}

WorkScheduler::~WorkScheduler() { cancel(); }

bool WorkScheduler::enqueue(Work work, WorkClass workClass) {
  if (!work) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) {
      return false;
    }
    queue_.push_back(WorkItem{
        .work = std::move(work),
        .workClass = workClass,
    });
  }
  cv_.notify_one();
  return true;
}

void WorkScheduler::finish() {
  {
    std::lock_guard lock(mutex_);
    if (!closed_ && !cancelled_) {
      finishing_ = true;
    }
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

bool WorkScheduler::hasEligibleWorkLocked() const {
  return std::any_of(queue_.begin(), queue_.end(), [this](const auto &item) {
    return item.workClass != WorkClass::ArchiveIo ||
           activeArchiveIo_ < archiveIoLimit_;
  });
}

std::deque<WorkScheduler::WorkItem>::iterator
WorkScheduler::firstEligibleWorkLocked() {
  return std::find_if(queue_.begin(), queue_.end(), [this](const auto &item) {
    return item.workClass != WorkClass::ArchiveIo ||
           activeArchiveIo_ < archiveIoLimit_;
  });
}

void WorkScheduler::workerLoop() {
  for (;;) {
    WorkItem item;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return cancelled_ || closed_ || hasEligibleWorkLocked() ||
               (finishing_ && queue_.empty() && activeTasks_ == 0);
      });
      if (cancelled_ || closed_) {
        return;
      }
      const auto workIt = firstEligibleWorkLocked();
      if (workIt == queue_.end()) {
        if (finishing_ && queue_.empty() && activeTasks_ == 0) {
          closed_ = true;
          cv_.notify_all();
          return;
        }
        continue;
      }
      item = std::move(*workIt);
      queue_.erase(workIt);
      ++activeTasks_;
      if (item.workClass == WorkClass::ArchiveIo) {
        ++activeArchiveIo_;
      }
    }

    std::exception_ptr exception;
    try {
      item.work();
    } catch (...) {
      exception = std::current_exception();
    }

    {
      std::lock_guard lock(mutex_);
      if (exception != nullptr) {
        exceptions_.push_back(exception);
      }
      if (item.workClass == WorkClass::ArchiveIo && activeArchiveIo_ > 0) {
        --activeArchiveIo_;
      }
      if (activeTasks_ > 0) {
        --activeTasks_;
      }
      if (finishing_ && queue_.empty() && activeTasks_ == 0) {
        closed_ = true;
      }
    }
    cv_.notify_all();
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
