#pragma once

#include "ChartLibraryTaskTypes.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>

namespace chart_library_tasks {

using TaskProgressCallback =
    std::function<void(const ChartScanProgress &, std::string_view)>;
using TaskPauseCallback = std::function<bool()>;
using TaskRunner = std::function<TaskRunResult(
    const TaskRequest &, const std::stop_token &, TaskProgressCallback,
    TaskPauseCallback)>;

class ChartLibraryTaskService final {
public:
  explicit ChartLibraryTaskService(TaskRunner runner);
  ~ChartLibraryTaskService();

  ChartLibraryTaskService(const ChartLibraryTaskService &) = delete;
  ChartLibraryTaskService &operator=(const ChartLibraryTaskService &) = delete;

  void start();
  void shutdown() noexcept;
  void setGameplayPaused(bool paused);
  std::uint64_t enqueue(TaskRequest request);
  std::uint64_t reserve(std::string title, std::string detail);
  bool enqueueReserved(std::uint64_t id, TaskRequest request);
  bool failReserved(std::uint64_t id, std::string detail);
  [[nodiscard]] Snapshot snapshot() const;
  std::vector<DownloadedIndexCompletion> takeDownloadedIndexCompletions();
  [[nodiscard]] bool active() const noexcept;

private:
  static bool isPauseable(TaskStatus status) noexcept;
  static bool isActive(TaskStatus status) noexcept;
  void run(const std::stop_token &stopToken);
  bool waitForResume(std::uint64_t id, const std::stop_token &stopToken);
  void publishProgress(std::uint64_t id, const ChartScanProgress &progress,
                       std::string_view detail);
  void setTaskStateLocked(std::uint64_t id, TaskStatus status, double fraction,
                          int current, int total, std::string detail);
  TaskInfo *findTaskLocked(std::uint64_t id);
  void bumpRevisionLocked();
  void trimHistoryLocked();

  TaskRunner runner_;
  mutable std::mutex stateMutex_;
  mutable std::mutex lifecycleMutex_;
  std::condition_variable_any workAvailable_;
  std::condition_variable_any pauseChanged_;
  std::deque<TaskRequest> queue_;
  std::vector<TaskInfo> tasks_;
  std::vector<DownloadedIndexCompletion> downloadedIndexCompletions_;
  std::optional<std::uint64_t> activeTaskId_;
  ProgressSnapshot progress_;
  std::uint64_t nextTaskId_ = 1;
  std::uint64_t revision_ = 0;
  std::uint64_t progressRevision_ = 0;
  bool gameplayPaused_ = false;
  std::jthread worker_;
};

} // namespace chart_library_tasks
