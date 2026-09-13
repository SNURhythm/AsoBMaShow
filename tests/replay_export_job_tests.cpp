#include "../src/replay/ReplayExportJob.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void wait(std::future<void> &signal) {
  require(signal.wait_for(5s) == std::future_status::ready,
          "export worker did not reach the expected checkpoint");
  signal.get();
}

ReplayVideoExportResult receive(replay::ReplayExportJob &job) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  do {
    if (auto result = job.takeResult()) {
      return std::move(*result);
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  require(false, "export worker did not deliver a result");
  return {};
}

void completionOwnsAdmissionUntilDelivered() {
  replay::ReplayExportJob job;
  require(job.tryBegin(), "first export must be accepted");
  require(!job.tryBegin(), "reservation must reject a second export before start");
  require(!job.takeResult(), "reserved work has no result yet");
  require(job.inProgress(), "polling without a result must preserve admission");

  std::promise<void> ran;
  auto finished = ran.get_future();
  auto lifetime = std::make_shared<int>(42);
  std::weak_ptr<int> observedLifetime = lifetime;
  const auto caller = std::this_thread::get_id();
  ReplayVideoExportOptions requested;
  requested.pacemakerTarget = "AAA";
  requested.renderTouchPoints = true;
  requested.renderReplayGhosts = false;
  job.start(requested, [&ran, caller, lifetime](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
    require(std::this_thread::get_id() != caller, "export must run on a worker");
    require(options.stop.stop_possible() && !options.stop.stop_requested(),
            "work must receive its live worker cancellation token");
    require(options.pacemakerTarget == "AAA" && options.renderTouchPoints &&
                !options.renderReplayGhosts, "job must preserve supplied export presentation options");
    ran.set_value();
    return ReplayVideoExportResult{true, "export.mp4", "Saved to Photos"};
  });
  lifetime.reset();
  wait(finished);
  require(!job.tryBegin(), "completed work must remain busy until result delivery");
  auto result = receive(job);
  require(result.success && result.outputPath == "export.mp4" &&
              result.message == "Saved to Photos", "result fields must survive delivery");
  require(!job.inProgress(), "result delivery must release admission");
  require(observedLifetime.expired(), "result delivery must join the worker");
  require(!job.takeResult(), "each result must be consumed once");
}

void progressIsLatestAndNewWorkClearsIt() {
  replay::ReplayExportJob job;
  require(job.tryBegin(), "progress export must be accepted");
  job.start({}, [&job](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
    options.progressCallback({0.25, "first", 1, 4});
    options.progressCallback({0.75, "latest", 3, 4});
    return ReplayVideoExportResult{false, {}, "Encoding unavailable"};
  });
  const auto result = receive(job);
  require(!result.success && result.message == "Encoding unavailable",
          "reported failures must survive delivery");
  const auto progress = job.takeProgress();
  require(progress && progress->fraction == 0.75 && progress->message == "latest",
          "progress delivery must coalesce to the latest value");
  require(!job.takeProgress(), "each progress update must be consumed once");
  // Leave a progress update unconsumed before starting another job.
  require(job.tryBegin(), "intermediate job must be accepted");
  job.start({}, [](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
    options.progressCallback({0.9, "old export"});
    return ReplayVideoExportResult{};
  });
  receive(job);
  require(job.tryBegin(), "completed job must accept another export");
  std::promise<void> release;
  auto ready = release.get_future();
  job.start({}, [&ready](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
    ready.wait();
    return ReplayVideoExportResult{true, {}, "new export"};
  });
  require(!job.takeProgress(), "new work must not display old progress");
  release.set_value();
  require(receive(job).message == "new export", "reused job must deliver fresh result");
}

void cancellationJoinsWithoutDiscardingResult() {
  replay::ReplayExportJob job;
  std::promise<void> entered;
  auto started = entered.get_future();
  bool observedStop = false;
  require(job.tryBegin(), "cancellable export must be accepted");
  job.start({}, [&](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
    entered.set_value();
    while (!options.stop.stop_requested()) std::this_thread::yield();
    require(cancelled.load(), "shutdown must cancel chart preparation too");
    observedStop = true;
    options.progressCallback({0.5, "cancelled"});
    return ReplayVideoExportResult{false, {}, "Replay export cancelled"};
  });
  wait(started);
  job.cancelAndWait();
  require(observedStop, "shutdown must deliver cancellation and join work");
  require(job.inProgress(), "shutdown alone must not consume the pending result");
  require(receive(job).message == "Replay export cancelled",
          "shutdown must retain the worker's failure result");
  job.reset();
  require(!job.inProgress() && !job.takeResult() && !job.takeProgress(),
          "scene reset must discard completed state");
}

void destructionStopsBeforeDestroyingMailboxes() {
  bool stopped = false;
  {
    std::promise<void> entered;
    auto started = entered.get_future();
    replay::ReplayExportJob job;
    require(job.tryBegin(), "destructor export must be accepted");
    job.start({}, [&](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) {
      entered.set_value();
      while (!cancelled.load()) std::this_thread::yield();
      options.progressCallback({1.0, "stopping"});
      stopped = true;
      return ReplayVideoExportResult{false, {}, "stopped"};
    });
    wait(started);
  }
  require(stopped, "job destruction must cancel and join before returning");
}

void exceptionsBecomeFailures() {
  replay::ReplayExportJob job;
  require(job.tryBegin(), "throwing export must be accepted");
  job.start({}, [](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) -> ReplayVideoExportResult {
    throw std::runtime_error("Decoder failed");
  });
  auto result = receive(job);
  require(!result.success && result.message == "Decoder failed" && !job.inProgress(),
          "standard exceptions must report failure and release the job");
  require(job.tryBegin(), "exception must not prevent another export");
  job.start({}, [](const ReplayVideoExportOptions &options, std::atomic_bool &cancelled) -> ReplayVideoExportResult { throw 1; });
  result = receive(job);
  require(!result.success && result.message == "Unexpected replay export failure",
          "unknown exceptions must report a failure instead of terminating");
}
} // namespace

int main() {
  completionOwnsAdmissionUntilDelivered();
  progressIsLatestAndNewWorkClearsIt();
  cancellationJoinsWithoutDiscardingResult();
  destructionStopsBeforeDestroyingMailboxes();
  exceptionsBecomeFailures();
  std::cout << "Replay export job tests passed\n";
}
