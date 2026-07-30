#include "ChartScanWorkScheduler.h"

#include <algorithm>
#include <utility>

namespace chart_scan {

bool WorkScheduler::ArchiveReadCostKeyLess::operator()(
    const ArchiveReadCostKey &left,
    const ArchiveReadCostKey &right) const {
  if (std::get<0>(left) != std::get<0>(right)) {
    return std::get<0>(left) > std::get<0>(right);
  }
  if (std::get<1>(left) != std::get<1>(right)) {
    return std::get<1>(left) < std::get<1>(right);
  }
  return std::get<2>(left) < std::get<2>(right);
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

bool WorkScheduler::enqueue(Work work, WorkClass workClass,
                            std::size_t archiveOrder,
                            std::size_t archiveReadCost) {
  if (!work) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_ || cancelled_) {
      return false;
    }
    switch (workClass) {
    case WorkClass::Cpu:
      cpuQueue_.push_back(std::move(work));
      break;
    case WorkClass::ArchiveIndex:
      archiveIndexQueue_.push_back(std::move(work));
      break;
    case WorkClass::ArchiveRead:
    case WorkClass::ArchiveReadHeavy: {
      const std::uint64_t sequence = nextArchiveReadEnqueueSequence_++;
      auto read = std::make_shared<ArchiveReadWork>(ArchiveReadWork{
          std::move(work), archiveOrder, archiveReadCost, sequence});
      archiveReadsByOrder_.emplace(
          ArchiveReadOrderKey{archiveOrder, sequence}, read);
      archiveReadsByCost_.emplace(
          ArchiveReadCostKey{archiveReadCost, archiveOrder, sequence},
          std::move(read));
      break;
    }
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
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
    cancelled_ = true;
    cpuQueue_.clear();
    archiveIndexQueue_.clear();
    archiveReadsByOrder_.clear();
    archiveReadsByCost_.clear();
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
  return !cpuQueue_.empty() || !archiveIndexQueue_.empty() ||
         !archiveReadsByOrder_.empty();
}

bool WorkScheduler::hasEligibleWorkLocked() const {
  const std::size_t archiveAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  const bool indexEligible = !archiveIndexQueue_.empty() &&
                             activeArchiveIndexes_ < archiveAdmissionLimit;
  const bool readEligible = !archiveReadsByOrder_.empty() &&
                            activeArchiveReads_ < archiveAdmissionLimit;
  return !cpuQueue_.empty() || indexEligible || readEligible;
}

bool WorkScheduler::popNextWorkLocked(WorkItem &item) {
  const std::size_t archiveAdmissionLimit =
      cpuQueue_.empty() ? archiveIoLimit_ : std::size_t{1};
  const bool indexEligible = !archiveIndexQueue_.empty() &&
                             activeArchiveIndexes_ < archiveAdmissionLimit;
  const bool readEligible = !archiveReadsByOrder_.empty() &&
                            activeArchiveReads_ < archiveAdmissionLimit;
  if (indexEligible &&
      (activeArchiveIndexes_ == 0 || cpuQueue_.empty())) {
    item.work = std::move(archiveIndexQueue_.front());
    item.workClass = WorkClass::ArchiveIndex;
    archiveIndexQueue_.pop_front();
    return true;
  }
  if (readEligible && (activeArchiveReads_ == 0 || cpuQueue_.empty())) {
    return popArchiveReadLocked(item);
  }
  if (!cpuQueue_.empty()) {
    item.work = std::move(cpuQueue_.front());
    item.workClass = WorkClass::Cpu;
    cpuQueue_.pop_front();
    return true;
  }
  if (indexEligible) {
    item.work = std::move(archiveIndexQueue_.front());
    item.workClass = WorkClass::ArchiveIndex;
    archiveIndexQueue_.pop_front();
    return true;
  }
  if (readEligible) {
    return popArchiveReadLocked(item);
  }
  return false;
}

bool WorkScheduler::popArchiveReadLocked(WorkItem &item) {
  if (archiveReadsByOrder_.empty()) {
    return false;
  }

  const auto earliest = archiveReadsByOrder_.begin();
  const bool needsFrontier =
      activeArchiveReadOrders_.empty() ||
      earliest->second->archiveOrder < *activeArchiveReadOrders_.begin();
  const ArchiveReadWorkPtr selected =
      needsFrontier ? earliest->second : archiveReadsByCost_.begin()->second;

  item.work = std::move(selected->work);
  item.workClass = WorkClass::ArchiveRead;
  item.archiveOrder = selected->archiveOrder;
  eraseArchiveReadLocked(selected);
  return true;
}

void WorkScheduler::eraseArchiveReadLocked(const ArchiveReadWorkPtr &read) {
  archiveReadsByOrder_.erase(
      ArchiveReadOrderKey{read->archiveOrder, read->enqueueSequence});
  archiveReadsByCost_.erase(ArchiveReadCostKey{
      read->archiveReadCost, read->archiveOrder, read->enqueueSequence});
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
        activeArchiveReadOrders_.insert(item.archiveOrder);
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
        const auto activeOrder =
            activeArchiveReadOrders_.find(item.archiveOrder);
        if (activeOrder != activeArchiveReadOrders_.end()) {
          activeArchiveReadOrders_.erase(activeOrder);
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
