#include "ChartScanWorkScheduler.h"

#include <algorithm>
#include <utility>

namespace chart_scan {

std::size_t recommendedWorkerCount(std::size_t workItemCount,
                                   unsigned int hardwareThreadCount) {
  if (workItemCount == 0) {
    return 0;
  }

  constexpr std::size_t fallbackWorkerCount = 4;
  const std::size_t availableThreads =
      hardwareThreadCount == 0 ? fallbackWorkerCount : hardwareThreadCount;
  return std::min(workItemCount, availableThreads);
}

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

bool WorkScheduler::enqueue(Work work, WorkClass workClass, Work onCancel) {
  if (!work) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) {
      return false;
    }
    WorkItem item{
        .work = std::move(work),
        .onCancel = std::move(onCancel),
        .workClass = workClass,
    };
    switch (workClass) {
    case WorkClass::Cpu:
      cpuQueue_.push_back(std::move(item));
      break;
    case WorkClass::ArchiveIndex:
      archiveIndexQueue_.push_back(std::move(item));
      break;
    case WorkClass::ArchiveRead:
      archiveReadQueue_.push_back(std::move(item));
      break;
    case WorkClass::ArchiveReadHeavy:
      heavyArchiveReadQueue_.push_back(std::move(item));
      break;
    }
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
  std::vector<Work> cancellationCallbacks;
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cancelled_ = true;
    auto collectCallbacks = [&](std::deque<WorkItem> &queue) {
      while (!queue.empty()) {
        if (queue.front().onCancel) {
          cancellationCallbacks.push_back(
              std::move(queue.front().onCancel));
        }
        queue.pop_front();
      }
    };
    collectCallbacks(cpuQueue_);
    collectCallbacks(archiveIndexQueue_);
    collectCallbacks(archiveReadQueue_);
    collectCallbacks(heavyArchiveReadQueue_);
  }
  for (auto &callback : cancellationCallbacks) {
    try {
      callback();
    } catch (...) {
      std::lock_guard lock(mutex_);
      exceptions_.push_back(std::current_exception());
    }
  }
  cv_.notify_all();
  joinWorkers();
}

bool WorkScheduler::isIdle() {
  std::lock_guard lock(mutex_);
  return !hasPendingWorkLocked() && activeTasks_ == 0;
}

std::vector<std::exception_ptr> WorkScheduler::takeExceptions() {
  std::lock_guard lock(mutex_);
  std::vector<std::exception_ptr> result = std::move(exceptions_);
  exceptions_.clear();
  return result;
}

bool WorkScheduler::hasPendingWorkLocked() const {
  return !cpuQueue_.empty() || !archiveIndexQueue_.empty() ||
         !archiveReadQueue_.empty() || !heavyArchiveReadQueue_.empty();
}

bool WorkScheduler::hasEligibleWorkLocked() const {
  const std::size_t archiveAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  const bool indexEligible = !archiveIndexQueue_.empty() &&
                             activeArchiveIndexes_ < archiveAdmissionLimit;
  const bool readEligible = !archiveReadQueue_.empty() &&
                            activeArchiveReads_ < archiveAdmissionLimit;
  const bool heavyReadEligible =
      !heavyArchiveReadQueue_.empty() &&
      activeArchiveReads_ < archiveAdmissionLimit;
  return !cpuQueue_.empty() || indexEligible || readEligible ||
         heavyReadEligible;
}

bool WorkScheduler::popNextWorkLocked(WorkItem &item) {
  const std::size_t archiveAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  const bool indexEligible = !archiveIndexQueue_.empty() &&
                             activeArchiveIndexes_ < archiveAdmissionLimit;
  const bool readEligible = !archiveReadQueue_.empty() &&
                            activeArchiveReads_ < archiveAdmissionLimit;
  const bool heavyReadEligible =
      !heavyArchiveReadQueue_.empty() &&
      activeArchiveReads_ < archiveAdmissionLimit;
  if (indexEligible &&
      (activeArchiveIndexes_ == 0 || cpuQueue_.empty())) {
    item = std::move(archiveIndexQueue_.front());
    archiveIndexQueue_.pop_front();
    return true;
  }
  if (readEligible && (activeArchiveReads_ == 0 || cpuQueue_.empty())) {
    item = std::move(archiveReadQueue_.front());
    archiveReadQueue_.pop_front();
    return true;
  }
  if (heavyReadEligible &&
      (activeArchiveReads_ == 0 || cpuQueue_.empty())) {
    item = std::move(heavyArchiveReadQueue_.front());
    heavyArchiveReadQueue_.pop_front();
    return true;
  }
  if (!cpuQueue_.empty()) {
    item = std::move(cpuQueue_.front());
    cpuQueue_.pop_front();
    return true;
  }
  if (indexEligible) {
    item = std::move(archiveIndexQueue_.front());
    archiveIndexQueue_.pop_front();
    return true;
  }
  if (readEligible) {
    item = std::move(archiveReadQueue_.front());
    archiveReadQueue_.pop_front();
    return true;
  }
  if (heavyReadEligible) {
    item = std::move(heavyArchiveReadQueue_.front());
    heavyArchiveReadQueue_.pop_front();
    return true;
  }
  return false;
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
      if (item.workClass == WorkClass::ArchiveIndex) {
        ++activeArchiveIndexes_;
      } else if (item.workClass == WorkClass::ArchiveRead) {
        ++activeArchiveReads_;
      } else if (item.workClass == WorkClass::ArchiveReadHeavy) {
        ++activeArchiveReads_;
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
      if (item.workClass == WorkClass::ArchiveIndex &&
          activeArchiveIndexes_ > 0) {
        --activeArchiveIndexes_;
      } else if (item.workClass == WorkClass::ArchiveRead &&
                 activeArchiveReads_ > 0) {
        --activeArchiveReads_;
      } else if (item.workClass == WorkClass::ArchiveReadHeavy) {
        if (activeArchiveReads_ > 0) {
          --activeArchiveReads_;
        }
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
