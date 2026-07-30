#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace chart_scan {

std::size_t recommendedWorkerCount(
    std::size_t workItemCount,
    unsigned int hardwareThreadCount = std::thread::hardware_concurrency());

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
                         std::size_t archiveIoLimit =
                             std::numeric_limits<std::size_t>::max());
  ~WorkScheduler();
  WorkScheduler(const WorkScheduler &) = delete;
  WorkScheduler &operator=(const WorkScheduler &) = delete;

  bool enqueue(Work work, WorkClass workClass = WorkClass::Cpu);
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
  std::deque<Work> archiveReadQueue_;
  std::deque<Work> heavyArchiveReadQueue_;
  std::vector<std::thread> workers_;
  std::vector<std::exception_ptr> exceptions_;
  std::size_t archiveIoLimit_ = 1;
  std::size_t activeTasks_ = 0;
  std::size_t activeArchiveIndexes_ = 0;
  std::size_t activeArchiveReads_ = 0;
  bool finishing_ = false;
  bool closed_ = false;
  bool cancelled_ = false;
};

} // namespace chart_scan
