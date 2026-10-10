#pragma once

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace replay_video_export {

// Native save operations can outlive cancellation (PhotoKit cannot roll back a
// submitted save). The task must own its inputs and any eventual file cleanup;
// it must not capture the scene, renderer, or the caller's output references.
template <class Task>
bool runNativeExportOperation(std::stop_token stop, Task task,
                              std::string &errorMessage) {
  if (stop.stop_requested()) {
    errorMessage = "Replay export cancelled";
    return false;
  }
  if (!stop.stop_possible()) return task(errorMessage);

  struct Completion {
    std::mutex mutex;
    std::condition_variable_any condition;
    bool finished = false;
    bool success = false;
    std::string error;
  };
  auto completion = std::make_shared<Completion>();
  std::thread([completion, stop, task = std::move(task)]() mutable {
    std::string error;
    bool success = false;
    try {
      if (!stop.stop_requested()) success = task(error);
    } catch (const std::exception &exception) {
      error = exception.what();
    } catch (...) {
      error = "Native replay export failed";
    }
    {
      std::lock_guard lock(completion->mutex);
      completion->success = success;
      completion->error = std::move(error);
      completion->finished = true;
    }
    completion->condition.notify_all();
  }).detach();

  std::unique_lock lock(completion->mutex);
  completion->condition.wait(lock, stop, [&] { return completion->finished; });
  if (stop.stop_requested()) {
    errorMessage = "Replay export cancelled";
    return false;
  }
  errorMessage = std::move(completion->error);
  return completion->success;
}

} // namespace replay_video_export
