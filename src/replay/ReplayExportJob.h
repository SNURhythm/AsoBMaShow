#pragma once

#include "../ReplayVideoExportTypes.h"

#include <atomic>
#include <functional>
#include <mutex>
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

  ReplayExportJob() = default;
  ~ReplayExportJob() { cancelAndWait(); }
  ReplayExportJob(const ReplayExportJob &) = delete;
  ReplayExportJob &operator=(const ReplayExportJob &) = delete;

  [[nodiscard]] bool tryBegin();
  [[nodiscard]] bool inProgress() const { return active_.load(); }
  [[nodiscard]] bool hasWorker() const { return worker_.joinable(); }

  // Call after successful reservation and scene-specific preview/UI handoff.
  // Preparation and export failures are delivered through takeResult().
  void start(ReplayVideoExportOptions options, Work work);
  void cancelAndWait();
  void reset();

  [[nodiscard]] std::optional<ReplayVideoExportProgress> takeProgress();
  // Joins completed work before releasing the reservation. Merely finishing
  // on the worker must not admit another export before UI result delivery.
  [[nodiscard]] std::optional<ReplayVideoExportResult> takeResult();

private:
  void publishProgress(const ReplayVideoExportProgress &progress);
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
