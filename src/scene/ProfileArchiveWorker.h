#pragma once

#include "ProfileSettingsController.h"
#include "../ThreadCompat.h"

#include <functional>
#include <mutex>
#include <optional>
#include <thread>

struct ProfileArchiveCompletion {
  ProfileArchiveTaskKind kind = ProfileArchiveTaskKind::Export;
  std::uint64_t generation = 0;
  ProfileArchiveResult result;
};

// Owns asynchronous execution of a controller-issued, one-shot archive task.
// Lifecycle methods belong to the application thread. The optional afterExecute
// callback runs on the worker before publication, including after stop is requested.
// It must not throw; operation failures retain ProfileArchiveTask's result policy.
class ProfileArchiveWorker final {
public:
  using AfterExecute = std::function<void(ProfileArchiveResult &)>;

  ProfileArchiveWorker() = default;
  ~ProfileArchiveWorker();
  ProfileArchiveWorker(const ProfileArchiveWorker &) = delete;
  ProfileArchiveWorker &operator=(const ProfileArchiveWorker &) = delete;

  // Rejects owned work, including a completion not yet consumed. Launch failures
  // propagate to the scene, which retains controller and temporary-file recovery.
  void start(ProfileArchiveTask task, AfterExecute afterExecute = {});
  [[nodiscard]] bool hasWorker() const { return worker_.joinable(); }
  // Joins before returning a completion; controller/picker decisions stay outside.
  [[nodiscard]] std::optional<ProfileArchiveCompletion> takeCompletion();
  // Archive transactions are not interruptible. Wait for execution and cleanup,
  // then discard completion without committing any controller presentation state.
  void stopAndWait();

private:
  std::mutex mutex_;
  std::optional<ProfileArchiveCompletion> completion_;
  std::jthread worker_;
};
