#include "ChartScanWorkScheduler.h"

#include <algorithm>
#include <utility>

namespace chart_scan {

WorkScheduler::WorkScheduler(std::size_t workerCount,
                             std::size_t archiveIoLimit) {
  const std::size_t actualWorkerCount = std::max<std::size_t>(1, workerCount);
  const std::size_t archiveIoCeiling =
      actualWorkerCount > 1 ? actualWorkerCount - 1 : std::size_t{1};
  archiveIoLimit_ =
      std::clamp<std::size_t>(archiveIoLimit, 1, archiveIoCeiling);
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
    auto &queue =
        workClass == WorkClass::ArchiveIo ? archiveIoQueue_ : cpuQueue_;
    queue.push_back(std::move(work));
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
    cpuQueue_.clear();
    archiveIoQueue_.clear();
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

bool WorkScheduler::hasPendingWorkLocked() const {
  return !cpuQueue_.empty() || !archiveIoQueue_.empty();
}

bool WorkScheduler::hasEligibleWorkLocked() const {
  const std::size_t archiveIoAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  return !cpuQueue_.empty() || (!archiveIoQueue_.empty() &&
                                activeArchiveIo_ < archiveIoAdmissionLimit);
}

bool WorkScheduler::popNextWorkLocked(WorkItem &item) {
  const std::size_t archiveIoAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  const bool archiveEligible =
      !archiveIoQueue_.empty() && activeArchiveIo_ < archiveIoAdmissionLimit;
  if (archiveEligible && (activeArchiveIo_ == 0 || cpuQueue_.empty())) {
    item.work = std::move(archiveIoQueue_.front());
    item.workClass = WorkClass::ArchiveIo;
    archiveIoQueue_.pop_front();
    return true;
  }
  if (!cpuQueue_.empty()) {
    item.work = std::move(cpuQueue_.front());
    item.workClass = WorkClass::Cpu;
    cpuQueue_.pop_front();
    return true;
  }
  if (!archiveEligible) {
    return false;
  }
  item.work = std::move(archiveIoQueue_.front());
  item.workClass = WorkClass::ArchiveIo;
  archiveIoQueue_.pop_front();
  return true;
}

void WorkScheduler::workerLoop() {
  for (;;) {
    WorkItem item;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return cancelled_ || closed_ || hasEligibleWorkLocked() ||
               (finishing_ && !hasPendingWorkLocked() && activeTasks_ == 0);
      });
      if (cancelled_ || closed_) {
        return;
      }
      if (!popNextWorkLocked(item)) {
        if (finishing_ && !hasPendingWorkLocked() && activeTasks_ == 0) {
          closed_ = true;
          cv_.notify_all();
          return;
        }
        continue;
      }
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
      if (finishing_ && !hasPendingWorkLocked() && activeTasks_ == 0) {
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
