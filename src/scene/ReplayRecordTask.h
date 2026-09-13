#pragma once

#include "../ThreadCompat.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Owns one replay/Records preparation worker and its deferred scene completion.
// start, takeCompletion, cancelAndWait, and destruction belong to the scene
// thread. Work may call publish, but must handle its action-specific failures.
class ReplayRecordTask final {
public:
  using Work = std::function<void(std::shared_ptr<std::atomic_bool>)>;
  using Completion = std::function<void()>;

  ReplayRecordTask() = default;
  ~ReplayRecordTask();
  ReplayRecordTask(const ReplayRecordTask &) = delete;
  ReplayRecordTask &operator=(const ReplayRecordTask &) = delete;

  void start(Work work);
  void publish(Completion completion);
  // Completed work without a published action yields a no-op notification.
  [[nodiscard]] Completion takeCompletion();
  void cancelAndWait();
  [[nodiscard]] bool active() const noexcept;

private:
  std::atomic_bool active_{false};
  std::shared_ptr<std::atomic_bool> cancelled_;
  std::mutex completionMutex_;
  Completion pendingCompletion_;
  bool completionPublished_ = false;
  std::jthread worker_;
};
