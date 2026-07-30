#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
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
      std::size_t archiveOrder = std::numeric_limits<std::size_t>::max());
  void finish();
  void cancel();
  std::vector<std::exception_ptr> takeExceptions();

private:
  struct WorkItem {
    Work work;
    WorkClass workClass = WorkClass::Cpu;
  };

  bool hasPendingWorkLocked() const;
  bool hasEligibleWorkLocked() const;
  bool popNextWorkLocked(WorkItem &item);
  void workerLoop();
  void joinWorkers();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Work> cpuQueue_;
  std::deque<Work> archiveIndexQueue_;
  using ArchiveReadKey = std::pair<std::size_t, std::uint64_t>;
  std::map<ArchiveReadKey, Work> archiveReadQueue_;
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
