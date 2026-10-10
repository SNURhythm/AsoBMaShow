#pragma once

#include "../ReplayVideoExportTypes.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>

namespace replay {

// The scene reserves a job before handing off preview resources, then starts
// it. Only the application thread drives the lifecycle and consumes updates;
// the worker may publish progress. Work must finish before scene dependencies
// are destroyed, so scene cleanup calls cancelAndWait().
class ReplayExportJob {
public:
  using Work =
      std::function<ReplayVideoExportResult(const ReplayVideoExportOptions &, std::atomic_bool &)>;

  // Executor must enqueue onto the lifecycle owner and never invoke inline.
  // Only that owner may run callbacks, cancel, or destroy this job.
  using OwnerExecutor = std::function<void(std::function<void()>)>;
  ReplayExportJob();
  explicit ReplayExportJob(OwnerExecutor executor) : ownerExecutor_(std::move(executor)) {}
  ~ReplayExportJob() { cancelAndWait(); }
  ReplayExportJob(const ReplayExportJob &) = delete;
  ReplayExportJob &operator=(const ReplayExportJob &) = delete;

  [[nodiscard]] bool tryBegin();
  [[nodiscard]] bool inProgress() const { return active_.load(); }
  [[nodiscard]] bool hasWorker() const { return pending_ || worker_.joinable(); }

  // Call after successful reservation and scene-specific preview/UI handoff.
  // Worker startup, preparation, and export failures use takeResult().
  void start(ReplayVideoExportOptions options, Work work);
  void cancelAndWait();
  void reset();

  [[nodiscard]] std::optional<ReplayVideoExportProgress> takeProgress();
  // Joins completed work before releasing the reservation. Merely finishing
  // on the worker must not admit another export before UI result delivery.
  [[nodiscard]] std::optional<ReplayVideoExportResult> takeResult();

private:
  void execute(ReplayVideoExportOptions options, Work work, std::stop_token stop);
  OwnerExecutor ownerExecutor_;
  std::shared_ptr<int> pending_;
  std::stop_source ownerStop_;
  void publishProgress(const ReplayVideoExportProgress &progress);
  void publishResult(ReplayVideoExportResult result);
  std::atomic_bool active_ = false;
  std::atomic_bool cancelled_ = false;
  std::mutex progressMutex_;
  std::optional<ReplayVideoExportProgress> progress_;
  std::mutex resultMutex_;
  std::optional<ReplayVideoExportResult> result_;
  // Destruction stops/joins the worker before its mailboxes are destroyed.
  std::jthread worker_;
};

} // namespace replay
