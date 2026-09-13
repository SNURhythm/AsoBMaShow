#include "scene/FindBmsTask.h"

#include <cassert>
#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace {
struct Gate {
  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  void block() {
    entered.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
  }
  void wait() { assert(entered.get_future().wait_for(5s) == std::future_status::ready); }
};
FindBmsTask::Updates waitForResult(FindBmsTask &task) {
  FindBmsTask::Updates combined;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto updates = task.takeUpdates();
    for (auto &event : updates.progress) { combined.progress.push_back(std::move(event)); }
    if (updates.result) {
      combined.result = std::move(updates.result);
      return combined;
    }
    std::this_thread::yield();
  }
  assert(false && "Find BMS result was not delivered");
  return combined;
}

struct WorkDestroyed {
  std::promise<void> done;
  ~WorkDestroyed() { done.set_value(); }
};
// Thread callable captures are released after result publication. This gives
// the test an exact completed-but-unconsumed boundary without timing sleeps.
std::future<void> startObserved(FindBmsTask &task, FindBmsTask::Work work) {
  auto signal = std::make_shared<WorkDestroyed>();
  auto done = signal->done.get_future();
  assert(task.start([signal, work = std::move(work)](auto &cancelled, auto progress) {
    return work(cancelled, std::move(progress));
  }));
  return done;
}

void testProgressRetainsLast160EventsInOrderAndResultIsTakenOnce() {
  FindBmsTask task;
  Gate gate;
  assert(task.start([&](auto &cancelled, auto progress) {
    assert(!cancelled.load());
    for (std::uint64_t i = 0; i < 200; ++i) {
      progress({"progress-" + std::to_string(i), i, 200});
    }
    gate.block();
    return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded,
                           .outputPath = "downloaded.bms"};
  }));
  gate.wait();
  assert(!task.start([](auto &, auto) { assert(false); return BmsSearchResult{}; }));
  auto updates = task.takeUpdates();
  assert(!updates.result && updates.progress.size() == 160);
  for (std::size_t i = 0; i < updates.progress.size(); ++i) {
    assert(updates.progress[i].downloadedBytes == i + 40);
    assert(updates.progress[i].message == "progress-" + std::to_string(i + 40));
  }
  assert(task.takeUpdates().progress.empty());
  gate.release.set_value();
  updates = waitForResult(task);
  assert(updates.result && updates.result->outputPath == "downloaded.bms");
  assert(updates.result->status == BmsSearchResult::Status::Downloaded);
  assert(!task.takeUpdates().result);
}

void testCancellationReturnsImmediatelyAndStillDeliversServiceResult() {
  FindBmsTask task;
  Gate gate;
  assert(task.start([&](auto &cancelled, auto progress) {
    gate.block();
    assert(cancelled.load());
    progress({"Cancelled", 4, 8});
    return BmsSearchResult{.status = BmsSearchResult::Status::DownloadFailed,
                           .message = "Cancelled with files to review",
                           .pendingArtifact = BmsSearchPendingArtifact{}};
  }));
  gate.wait();
  task.requestCancel();
  assert(task.running());
  task.requestCancel();
  gate.release.set_value();
  const auto updates = waitForResult(task);
  assert(updates.progress.size() == 1 && updates.result);
  assert(updates.result->message == "Cancelled with files to review");
  assert(updates.result->pendingArtifact);
}

void testStopJoinsArtifactResolutionAndReplacementDiscardsOldData() {
  FindBmsTask task;
  Gate gate;
  std::atomic_bool finished = false;
  assert(task.start([&](auto &, auto progress) {
    progress({"Keeping files", 0, 0});
    gate.block(); // Artifact resolution intentionally ignores cancellation.
    finished = true;
    return BmsSearchResult{.message = "Kept files"};
  }));
  gate.wait();
  auto stopped = std::async(std::launch::async, [&] { task.stopAndWait(); });
  assert(stopped.wait_for(50ms) == std::future_status::timeout);
  gate.release.set_value();
  assert(stopped.wait_for(5s) == std::future_status::ready);
  stopped.get();
  assert(finished && !task.running());
  auto empty = task.takeUpdates();
  assert(empty.progress.empty() && !empty.result);
  task.stopAndWait();
  auto completed = startObserved(task, [](auto &cancelled, auto progress) {
    assert(!cancelled.load());
    progress({"Old completed progress", 0, 0});
    return BmsSearchResult{.message = "Old completed result"};
  });
  assert(completed.wait_for(5s) == std::future_status::ready);
  assert(task.running()); // Result handoff is part of the operation.
  assert(!task.start([](auto &, auto) { return BmsSearchResult{}; }));
  task.stopAndWait(); // Replacement explicitly discards the old completion.

  Gate next;
  assert(task.start([&](auto &cancelled, auto) {
    assert(!cancelled.load()); next.block(); return BmsSearchResult{.message = "New"};
  }));
  next.wait();
  empty = task.takeUpdates();
  assert(empty.progress.empty() && !empty.result);
  next.release.set_value();
  assert(waitForResult(task).result->message == "New");
}

void testDestructionCancelsAndJoinsBeforeReleasingWorkCaptures() {
  auto task = std::make_unique<FindBmsTask>();
  auto resource = std::make_shared<int>(42);
  std::weak_ptr<int> observed = resource;
  std::promise<void> entered;
  assert(task->start([&, resource](auto &cancelled, auto) {
    entered.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    assert(cancelled.load() && *resource == 42);
    return BmsSearchResult{};
  }));
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  resource.reset();
  task.reset();
  assert(observed.expired());
}
} // namespace

#include "find_bms_scene_fixture.h"

int main() {
  testProgressRetainsLast160EventsInOrderAndResultIsTakenOnce();
  testCancellationReturnsImmediatelyAndStillDeliversServiceResult();
  testStopJoinsArtifactResolutionAndReplacementDiscardsOldData();
  testDestructionCancelsAndJoinsBeforeReleasingWorkCaptures();
  testImmediateArtifactCompletionKeepsActionsGatedUntilHandoff();
  testSceneLookupProgressAndIndexHandoff();
  testSceneCancellationKeepsPendingArtifactVisible();
  testSceneCandidateAndPendingArtifactDecisions();
}
