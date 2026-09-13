#pragma once

#include "../BmsSearchService.h"
#include "../ThreadCompat.h"

#include <deque>
#include <mutex>

// Owns one Find BMS operation and its bounded progress/result handoff. All
// lifecycle methods and consumption belong to the application thread. Work
// and its progress callbacks must finish together; neither may retain the
// cancellation reference or progress callback after Work returns.
class FindBmsTask final {
public:
  using Work = std::function<BmsSearchResult(
      std::atomic_bool &, BmsSearchDownloadProgressCallback)>;
  struct Updates {
    std::deque<BmsSearchDownloadProgress> progress;
    std::optional<BmsSearchResult> result;
  };

  FindBmsTask() = default;
  ~FindBmsTask();
  FindBmsTask(const FindBmsTask &) = delete;
  FindBmsTask &operator=(const FindBmsTask &) = delete;

  // Rejects overlap and unconsumed results. Joins completed work and resets
  // cancellation before starting another lookup/download/artifact action.
  // Use stopAndWait to replace an operation without consuming its result.
  bool start(Work work);
  // Remains true after worker completion until its result is consumed, keeping
  // stale artifact actions disabled through the application-thread handoff.
  bool running() const;
  Updates takeUpdates();
  // Nonblocking: cancellation results still reach the dialog, including any
  // pending artifact that requires a keep/delete decision.
  void requestCancel();
  // Joins even uncancellable artifact work and discards its queued updates.
  void stopAndWait();

private:
  static constexpr std::size_t kMaxPendingProgressEvents = 160;
  mutable std::mutex mutex_;
  bool running_ = false;
  Updates pending_;
  std::atomic_bool cancelled_ = false;
  std::jthread worker_;
};
