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
#include <unordered_map>
#include <utility>

namespace chart_library_tasks {

using TaskProgressCallback =
    std::function<void(const ChartScanProgress &, const i18n::Text &)>;
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
  std::uint64_t reserve(i18n::Text title, i18n::Text detail);
  bool enqueueReserved(std::uint64_t id, TaskRequest request);
  bool failReserved(std::uint64_t id, i18n::Text detail);
  bool beginAndroidImport(const std::string &token, bool folder, bool moveSource = false);
  bool updateAndroidImportProgress(const std::string &token, int copiedFiles,
                                    int totalFiles, std::uint64_t copiedBytes,
                                    std::int64_t totalBytes, const std::string &name,
                                    int phase);
  int androidImportCopyState(const std::string &token) const;
  bool finishAndroidImport(const std::string &token, bool folder,
                           const std::filesystem::path &path,
                           const std::string &error,
                           const std::string &retainedError = {},
                           const std::string &archiveUri = {},
                           bool archiveGrantAcquired = false,
                           std::shared_ptr<AndroidArchiveImportOwner> archiveOwner = {});
  void cancelAndroidImports();
  [[nodiscard]] Snapshot snapshot() const;
  std::vector<DownloadedIndexCompletion> takeDownloadedIndexCompletions();
  [[nodiscard]] bool active() const noexcept;

private:
  static bool isPauseable(TaskStatus status) noexcept;
  static bool isActive(TaskStatus status) noexcept;
  // Requires lifecycleMutex_; admission holds it through the state commit.
  void startWorkerLocked();
  void run(const std::stop_token &stopToken);
  bool waitForResume(std::uint64_t id, const std::stop_token &stopToken);
  void publishProgress(std::uint64_t id, const ChartScanProgress &progress,
                       const i18n::Text &detail);
  void setTaskStateLocked(std::uint64_t id, TaskStatus status, double fraction,
                          int current, int total, i18n::Text detail);
  TaskInfo *findTaskLocked(std::uint64_t id);
  // Requires lifecycleMutex_ and stateMutex_, acquired in that order.
  bool enqueueReservedLocked(std::uint64_t id, TaskRequest request);
  void bumpRevisionLocked();
  void trimHistoryLocked();

  TaskRunner runner_;
  mutable std::mutex stateMutex_;
  mutable std::mutex lifecycleMutex_;
  std::condition_variable_any workAvailable_;
  std::condition_variable_any pauseChanged_;
  std::deque<TaskRequest> queue_;
  std::vector<TaskInfo> tasks_;
  struct AndroidImportReservation {
    std::uint64_t id;
    bool folder;
    bool moveSource;
  };
  std::unordered_map<std::string, AndroidImportReservation> androidImports_;
  bool acceptingAndroidImports_ = true;
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
