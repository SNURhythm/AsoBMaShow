#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace chart_scan {

enum class WorkClass {
  Cpu,
  ArchiveIndex,
  ArchiveRead,
  ArchiveReadHeavy,
};

class WorkScheduler {
public:
  using Work = std::function<void()>;

  explicit WorkScheduler(std::size_t workerCount,
                         std::size_t archiveIoLimit = 4);
  ~WorkScheduler();
  WorkScheduler(const WorkScheduler &) = delete;
  WorkScheduler &operator=(const WorkScheduler &) = delete;

  bool enqueue(
      Work work, WorkClass workClass = WorkClass::Cpu,
      std::size_t archiveOrder = std::numeric_limits<std::size_t>::max(),
      std::size_t archiveReadCost = 0);
  void finish();
  void cancel();
  std::vector<std::exception_ptr> takeExceptions();

private:
  struct WorkItem {
    Work work;
    WorkClass workClass = WorkClass::Cpu;
    std::size_t archiveOrder = std::numeric_limits<std::size_t>::max();
  };

  struct ArchiveReadWork {
    Work work;
    std::size_t archiveOrder = std::numeric_limits<std::size_t>::max();
    std::size_t archiveReadCost = 0;
    std::uint64_t enqueueSequence = 0;
  };

  using ArchiveReadWorkPtr = std::shared_ptr<ArchiveReadWork>;
  using ArchiveReadOrderKey = std::pair<std::size_t, std::uint64_t>;
  using ArchiveReadCostKey =
      std::tuple<std::size_t, std::size_t, std::uint64_t>;

  struct ArchiveReadCostKeyLess {
    bool operator()(const ArchiveReadCostKey &left,
                    const ArchiveReadCostKey &right) const;
  };

  bool hasPendingWorkLocked() const;
  bool hasEligibleWorkLocked() const;
  bool popNextWorkLocked(WorkItem &item);
  bool popArchiveReadLocked(WorkItem &item);
  void eraseArchiveReadLocked(const ArchiveReadWorkPtr &read);
  void workerLoop();
  void joinWorkers();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Work> cpuQueue_;
  std::deque<Work> archiveIndexQueue_;
  std::map<ArchiveReadOrderKey, ArchiveReadWorkPtr> archiveReadsByOrder_;
  std::map<ArchiveReadCostKey, ArchiveReadWorkPtr, ArchiveReadCostKeyLess>
      archiveReadsByCost_;
  std::multiset<std::size_t> activeArchiveReadOrders_;
  std::vector<std::thread> workers_;
  std::vector<std::exception_ptr> exceptions_;
  std::uint64_t nextArchiveReadEnqueueSequence_ = 0;
  std::size_t archiveIoLimit_ = 1;
  std::size_t activeTasks_ = 0;
  std::size_t activeArchiveIndexes_ = 0;
  std::size_t activeArchiveReads_ = 0;
  bool finishing_ = false;
  bool closed_ = false;
  bool cancelled_ = false;
};

} // namespace chart_scan
