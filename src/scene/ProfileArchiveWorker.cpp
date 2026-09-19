#include "ProfileArchiveWorker.h"

#include <stdexcept>
#include <utility>

ProfileArchiveWorker::~ProfileArchiveWorker() { stopAndWait(); }

void ProfileArchiveWorker::start(ProfileArchiveTask task,
                                 AfterExecute afterExecute) {
  if (worker_.joinable()) {
    throw std::logic_error("A profile archive worker already owns work.");
  }
  worker_ = std::jthread(
      [this, task = std::move(task), afterExecute = std::move(afterExecute)](
          const std::stop_token &stopToken) mutable {
        ProfileArchiveCompletion completion{.kind = task.kind(),
                                            .generation = task.generation(),
                                            .result = task.execute()};
        if (afterExecute) afterExecute(completion.result);
        if (stopToken.stop_requested()) return;
        std::lock_guard lock(mutex_);
        completion_ = std::move(completion);
      });
}

std::optional<ProfileArchiveCompletion> ProfileArchiveWorker::takeCompletion() {
  std::optional<ProfileArchiveCompletion> completion;
  {
    std::lock_guard lock(mutex_);
    completion.swap(completion_);
  }
  if (completion && worker_.joinable()) worker_.join();
  return completion;
}

void ProfileArchiveWorker::stopAndWait() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
  std::lock_guard lock(mutex_);
  completion_.reset();
}
