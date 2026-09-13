#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace archive_file {

// Coordinates builders only; cache contents, source freshness, and retry policy
// stay with the archive facade. Each waiter retains the generation it joined.
// The coordinator must outlive its leases. Separate leases may run concurrently;
// ownership of an individual lease is transferred or used by one thread.
class IndexBuildCoordinator final {
  struct BuildState;

public:
  enum class WaitOutcome { Succeeded, Failed, Cancelled };
  using Checkpoint = std::function<bool()>;

  class Lease final {
  public:
    ~Lease();
    Lease(Lease &&other) noexcept;
    Lease &operator=(Lease &&other) noexcept;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;

    bool isBuilder() const noexcept { return builder_; }
    // Builders must publish cache data before completion. Abandoning an owned
    // build (including unwinding) completes it with failure and wakes waiters.
    void complete(bool success);
    // Waiter-only. Polls every 20 ms; checkpoints run outside the coordinator
    // lock, including once after wakeup even if the build is already complete.
    // Returning false cancels this waiter only; callback exceptions propagate.
    WaitOutcome wait(const Checkpoint &checkpoint);

  private:
    friend class IndexBuildCoordinator;
    Lease(IndexBuildCoordinator &owner, std::shared_ptr<BuildState> state,
          bool builder);
    IndexBuildCoordinator *owner_ = nullptr;
    std::shared_ptr<BuildState> state_;
    bool builder_ = false;
  };

  // Exactly one builder is admitted per active key. Completed state is removed
  // from admission immediately; existing waiters keep its outcome independently
  // of a later request's build. The caller rechecks its cache after admission.
  Lease acquire(const std::string &key);

private:
  void complete(const std::shared_ptr<BuildState> &state, bool success);

  std::mutex mutex_;
  std::condition_variable changed_;
  std::unordered_map<std::string, std::shared_ptr<BuildState>> active_;
};

} // namespace archive_file
