#include "ChartLibraryTaskService.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace chart_library_tasks {

namespace {
constexpr std::size_t kMaximumTaskHistory = 24;
}

ChartLibraryTaskService::ChartLibraryTaskService(TaskRunner runner)
    : runner_(std::move(runner)) {}

ChartLibraryTaskService::~ChartLibraryTaskService() { shutdown(); }

bool ChartLibraryTaskService::isPauseable(TaskStatus status) noexcept {
  return status == TaskStatus::Queued || status == TaskStatus::Running;
}

bool ChartLibraryTaskService::isActive(TaskStatus status) noexcept {
  return isPauseable(status) || status == TaskStatus::Paused;
}

void ChartLibraryTaskService::start() {
  std::lock_guard lifecycleLock(lifecycleMutex_);
  if (worker_.joinable()) {
    return;
  }
  worker_ = std::jthread(
      [this](const std::stop_token &stopToken) { run(stopToken); });
}

void ChartLibraryTaskService::shutdown() noexcept {
  try {
    std::lock_guard lifecycleLock(lifecycleMutex_);
    if (!worker_.joinable()) {
      return;
    }
    worker_.request_stop();
    workAvailable_.notify_all();
    pauseChanged_.notify_all();
    worker_.join();
  } catch (...) {
  }
}

void ChartLibraryTaskService::setGameplayPaused(bool paused) {
  {
    std::lock_guard lock(stateMutex_);
    if (gameplayPaused_ == paused) {
      return;
    }
    gameplayPaused_ = paused;
    bool changed = false;
    for (auto &task : tasks_) {
      if (paused) {
        if (!isPauseable(task.status)) {
          continue;
        }
        task.status = TaskStatus::Paused;
        task.detail = "Paused";
        changed = true;
        continue;
      }
      if (task.status != TaskStatus::Paused) {
        continue;
      }
      if (activeTaskId_ && *activeTaskId_ == task.id) {
        task.status = TaskStatus::Running;
        task.detail = "Resuming";
      } else {
        const auto queued = std::find_if(
            queue_.begin(), queue_.end(), [&task](const TaskRequest &request) {
              return request.id == task.id;
            });
        if (queued == queue_.end()) {
          continue;
        }
        task.status = TaskStatus::Queued;
        task.detail = "Waiting";
      }
      changed = true;
    }
    if (changed) {
      bumpRevisionLocked();
    }
  }
  pauseChanged_.notify_all();
  workAvailable_.notify_all();
}

std::uint64_t ChartLibraryTaskService::enqueue(TaskRequest request) {
  std::uint64_t id = 0;
  {
    std::lock_guard lock(stateMutex_);
    request.id = nextTaskId_++;
    id = request.id;
    const std::string title = request.title;
    queue_.push_back(std::move(request));
    tasks_.push_back(TaskInfo{
        .id = id,
        .title = title,
        .status = gameplayPaused_ ? TaskStatus::Paused : TaskStatus::Queued,
        .detail = gameplayPaused_ ? "Paused" : "Waiting",
    });
    trimHistoryLocked();
    bumpRevisionLocked();
  }
  start();
  workAvailable_.notify_one();
  return id;
}

Snapshot ChartLibraryTaskService::snapshot() const {
  std::lock_guard lock(stateMutex_);
  return Snapshot{.revision = revision_,
                  .activeCount = static_cast<int>(std::count_if(
                      tasks_.begin(), tasks_.end(), [](const TaskInfo &task) {
                        return isActive(task.status);
                      })),
                  .tasks = tasks_,
                  .progress = progress_};
}

std::vector<DownloadedIndexCompletion>
ChartLibraryTaskService::takeDownloadedIndexCompletions() {
  std::lock_guard lock(stateMutex_);
  auto completions = std::move(downloadedIndexCompletions_);
  downloadedIndexCompletions_.clear();
  return completions;
}

bool ChartLibraryTaskService::active() const noexcept {
  try {
    std::lock_guard lock(lifecycleMutex_);
    return worker_.joinable();
  } catch (...) {
    return false;
  }
}

void ChartLibraryTaskService::run(const std::stop_token &stopToken) {
  while (!stopToken.stop_requested()) {
    TaskRequest task;
    {
      std::unique_lock lock(stateMutex_);
      workAvailable_.wait(lock, stopToken, [this] {
        return !gameplayPaused_ && !queue_.empty();
      });
      if (stopToken.stop_requested()) {
        break;
      }
      if (gameplayPaused_ || queue_.empty()) {
        continue;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
      activeTaskId_ = task.id;
      setTaskStateLocked(task.id, TaskStatus::Running, 0.0, 0, 0,
                         "Starting");
    }

    TaskRunResult result;
    try {
      result = runner_(
          task, stopToken,
          [this, id = task.id](const ChartScanProgress &progress,
                               std::string_view detail) {
            publishProgress(id, progress, detail);
          },
          [this, id = task.id, &stopToken] {
            return waitForResume(id, stopToken);
          });
    } catch (const std::exception &error) {
      result = {.disposition = TaskRunDisposition::Failed,
                .detail = error.what()};
    } catch (...) {
      result = {.disposition = TaskRunDisposition::Failed,
                .detail = "Unknown library task failure"};
    }

    {
      std::lock_guard lock(stateMutex_);
      activeTaskId_.reset();
      switch (result.disposition) {
      case TaskRunDisposition::Complete:
        setTaskStateLocked(task.id, TaskStatus::Complete, 1.0, 1, 1,
                           result.detail);
        if (result.downloadedIndex) {
          downloadedIndexCompletions_.push_back(
              std::move(*result.downloadedIndex));
        }
        break;
      case TaskRunDisposition::Failed:
        setTaskStateLocked(task.id, TaskStatus::Failed, 0.0, 0, 0,
                           result.detail);
        break;
      case TaskRunDisposition::Paused:
        setTaskStateLocked(task.id, TaskStatus::Paused, 0.0, 0, 0,
                           result.detail);
        if (!stopToken.stop_requested()) {
          queue_.push_front(std::move(task));
        }
        break;
      }
      trimHistoryLocked();
    }
  }

  std::lock_guard lock(stateMutex_);
  for (auto &task : tasks_) {
    if (isPauseable(task.status)) {
      task.status = TaskStatus::Paused;
      task.detail = "Paused";
    }
  }
  bumpRevisionLocked();
}

bool ChartLibraryTaskService::waitForResume(
    std::uint64_t id, const std::stop_token &stopToken) {
  std::unique_lock lock(stateMutex_);
  if (!gameplayPaused_) {
    return !stopToken.stop_requested();
  }
  if (auto *task = findTaskLocked(id)) {
    task->status = TaskStatus::Paused;
    task->detail = "Paused";
    bumpRevisionLocked();
  }
  pauseChanged_.wait(lock, stopToken,
                     [this] { return !gameplayPaused_; });
  if (stopToken.stop_requested()) {
    return false;
  }
  if (auto *task = findTaskLocked(id)) {
    task->status = TaskStatus::Running;
    task->detail = "Resuming";
    bumpRevisionLocked();
  }
  return true;
}

void ChartLibraryTaskService::publishProgress(
    std::uint64_t id, const ChartScanProgress &progress,
    std::string_view detail) {
  std::lock_guard lock(stateMutex_);
  if (!activeTaskId_ || *activeTaskId_ != id) {
    return;
  }
  const int total = std::max(0, progress.total);
  const int current = total > 0 ? std::clamp(progress.current, 0, total)
                                : std::max(0, progress.current);
  const int basisPoints =
      total > 0
          ? static_cast<int>((static_cast<std::int64_t>(current) * 10000) /
                             std::max(1, total))
          : 0;
  progress_ = {.valid = true,
               .revision = ++progressRevision_,
               .taskId = id,
               .current = current,
               .total = total,
               .basisPoints = std::clamp(basisPoints, 0, 10000),
               .stage = progress.stage};
  if (auto *task = findTaskLocked(id)) {
    task->fraction = static_cast<double>(basisPoints) / 10000.0;
    task->current = current;
    task->total = total;
    if (!detail.empty()) {
      task->detail = std::string(detail);
    }
    bumpRevisionLocked();
  }
}

void ChartLibraryTaskService::setTaskStateLocked(
    std::uint64_t id, TaskStatus status, double fraction, int current,
    int total, std::string detail) {
  auto *task = findTaskLocked(id);
  if (task == nullptr) {
    return;
  }
  task->status = status;
  task->fraction = std::clamp(fraction, 0.0, 1.0);
  task->current = std::max(0, current);
  task->total = std::max(0, total);
  task->detail = std::move(detail);
  bumpRevisionLocked();
}

TaskInfo *ChartLibraryTaskService::findTaskLocked(std::uint64_t id) {
  const auto found = std::find_if(tasks_.begin(), tasks_.end(),
                                  [id](const TaskInfo &task) {
                                    return task.id == id;
                                  });
  return found == tasks_.end() ? nullptr : &*found;
}

void ChartLibraryTaskService::bumpRevisionLocked() { ++revision_; }

void ChartLibraryTaskService::trimHistoryLocked() {
  while (tasks_.size() > kMaximumTaskHistory) {
    const auto terminal =
        std::find_if(tasks_.begin(), tasks_.end(), [](const TaskInfo &task) {
          return !isActive(task.status);
        });
    if (terminal == tasks_.end()) {
      return;
    }
    tasks_.erase(terminal);
  }
}

} // namespace chart_library_tasks
