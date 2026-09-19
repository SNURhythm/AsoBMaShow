#pragma once

#include "IrUploadsController.h"

#include <utility>

namespace ir_uploads {

// Owns preparation, durable-enqueue cancellation, and data-only UI handoff.
// All public methods/destruction belong to the application thread; dependencies
// must remain valid until work is joined. The existing preparation function
// retains verification, partial-failure, and durable enqueue policy.
class PreparationTask final {
public:
  struct Updates {
    std::optional<std::pair<std::size_t, std::size_t>> progress;
    std::optional<PreparationOutcome> completion;
  };

  PreparationTask() = default;
  ~PreparationTask();
  PreparationTask(const PreparationTask &) = delete;
  PreparationTask &operator=(const PreparationTask &) = delete;

  // Rejects work or completion still owned by this task. The progress callback
  // is supplied by this owner; callers provide verification/enqueue dependencies.
  bool start(std::vector<ir::IrUploadCandidate> candidates,
             PreparationDependencies dependencies);
  bool hasWorker() const noexcept;
  // Consuming completion joins before returning it to the scene controller.
  Updates takeUpdates();
  // Cancels at the gate before requesting thread stop. A durable enqueue that
  // has already begun finishes; its outcome survives shutdown for consumption.
  void stopAndWait();
  // Scene initialization explicitly discards a previous operation's updates.
  void reset();

private:
  std::shared_ptr<DurableEnqueueGate> enqueueGate_;
  std::mutex mutex_;
  Updates pending_;
  std::jthread worker_;
};

} // namespace ir_uploads
