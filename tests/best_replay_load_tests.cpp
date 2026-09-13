#include "scene/play/BestReplayLoad.h"
#include "scene/ReplayRecordTask.h"

#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {
using ReplayLoader = decltype(replay::BestReplayResolverDependencies::loadReplay);

replay::BestReplayResolver resolverWith(ReplayLoader loadReplay) {
  return replay::BestReplayResolver({
      .loadResult = [](std::string_view attemptId) {
        ModernChartResultRecord record;
        record.result.attemptId = std::string(attemptId);
        return ModernChartResultReadOutcome{
            .status = ModernChartResultReadStatus::Loaded,
            .record = std::move(record)};
      },
      .loadReplay = std::move(loadReplay),
  });
}

void finish(ReplayRecordTask &task) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (task.active() && std::chrono::steady_clock::now() < deadline) {
    if (auto completion = task.takeCompletion()) completion();
    std::this_thread::yield();
  }
  assert(!task.active());
}

void waitCancelled(std::atomic_bool &cancelled) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  assert(cancelled.load());
}

void testResolverRunsInWorkerAndLoadedReplayIsDeliveredOnceOnCaller() {
  ReplayRecordTask task;
  const auto caller = std::this_thread::get_id();
  std::thread::id factoryThread, resolverThread;
  std::promise<void> loaded;
  int calls = 0;
  std::weak_ptr<ReplayData> retained;
  auto resolver = resolverWith([&](const auto &record, const auto &path, auto &cancelled) {
    resolverThread = std::this_thread::get_id();
    assert(record.result.attemptId == "exact-attempt" && path == "charts/song.bms");
    assert(!cancelled.load());
    auto data = std::make_shared<ReplayData>();
    data->finalScore = 1234;
    retained = data;
    loaded.set_value();
    return data;
  });
  replay::startBestReplayLoad(task, [&, resolver] {
    factoryThread = std::this_thread::get_id(); return resolver;
  }, "exact-attempt", "charts/song.bms", [&](const ReplayData &data) {
    assert(std::this_thread::get_id() == caller && data.finalScore == 1234);
    ++calls;
  });
  assert(loaded.get_future().wait_for(5s) == std::future_status::ready);
  assert(calls == 0 && !retained.expired());
  finish(task);
  assert(calls == 1 && factoryThread != caller && factoryThread == resolverThread);
  assert(retained.expired() && !task.takeCompletion());
}

// Without cancellation before replacement, the first consumer cannot exit.
// Accepting its late result would also deliver the wrong chart's ghost.
void testReplacementCancelsAndJoinsBlockedLoadBeforeStartingNext() {
  ReplayRecordTask task;
  std::promise<void> entered;
  bool firstExited = false;
  int chosen = 0;
  auto first = resolverWith([&](const auto &, const auto &, auto &cancelled) {
    entered.set_value();
    waitCancelled(cancelled);
    firstExited = true;
    return std::make_shared<ReplayData>();
  });
  replay::startBestReplayLoad(task, [first] { return first; }, "old", "old.bms",
                             [&](const auto &) { chosen = 1; });
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  auto second = resolverWith([&](const auto &record, const auto &path, auto &cancelled) {
    assert(firstExited && !cancelled.load());
    assert(record.result.attemptId == "new" && path == "new.bms");
    return std::make_shared<ReplayData>();
  });
  replay::startBestReplayLoad(task, [second] { return second; }, "new", "new.bms",
                             [&](const auto &) { chosen = 2; });
  finish(task);
  assert(firstExited && chosen == 2);
}

void testCancelDiscardsSuccessfulResultReturnedAfterCancellation() {
  ReplayRecordTask task;
  std::promise<void> entered;
  bool exited = false;
  int calls = 0;
  auto resolver = resolverWith([&](const auto &, const auto &, auto &cancelled) {
    entered.set_value();
    waitCancelled(cancelled);
    exited = true;
    return std::make_shared<ReplayData>();
  });
  replay::startBestReplayLoad(task, [resolver] { return resolver; }, "best", "song.bms",
                             [&](const auto &) { ++calls; });
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  task.cancelAndWait();
  assert(exited && !task.active() && !task.takeCompletion() && calls == 0);
}

void testMissingUnreadableAndMismatchedResultsLeaveFallbackUntouched() {
  for (int failure = 0; failure < 4; ++failure) {
    ReplayRecordTask task;
    int calls = 0;
    replay::BestReplayResolver resolver({
        .loadResult = [failure](std::string_view attemptId) {
          if (failure == 0) return ModernChartResultReadOutcome{
              .status = ModernChartResultReadStatus::NotFound};
          ModernChartResultRecord record;
          record.result.attemptId = failure == 1 ? "other" : std::string(attemptId);
          return ModernChartResultReadOutcome{
              .status = ModernChartResultReadStatus::Loaded, .record = std::move(record)};
        },
        .loadReplay = [failure](const auto &, const auto &, auto &) -> std::shared_ptr<ReplayData> {
          assert(failure >= 2);
          if (failure == 3) throw std::runtime_error("unreadable replay");
          return {};
        },
    });
    replay::startBestReplayLoad(task, [resolver] { return resolver; }, "best", "song.bms",
                               [&](const auto &) { ++calls; });
    finish(task);
    assert(calls == 0 && !task.takeCompletion());
  }
}

void testDestructionJoinsWorkAndReleasesCapturedSceneCompletion() {
  std::promise<void> entered;
  bool exited = false;
  int calls = 0;
  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> retained = lifetime;
  auto task = std::make_unique<ReplayRecordTask>();
  auto resolver = resolverWith([&](const auto &, const auto &, auto &cancelled) {
    entered.set_value();
    waitCancelled(cancelled);
    exited = true;
    return std::make_shared<ReplayData>();
  });
  replay::startBestReplayLoad(*task, [resolver] { return resolver; }, "best", "song.bms",
                             [&, lifetime](const auto &) { ++calls; });
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  lifetime.reset();
  task.reset();
  assert(exited && retained.expired() && calls == 0);
}
} // namespace

#include "best_replay_scene_fixture.h"

int main() {
  testSceneUpdatesPersonalBestGhostWithoutChangingSelectedTarget();
  testSceneIgnoresLoadedReplayWithoutChartOrBestSnapshot();
  testSceneStopPreventsLateReplayFromApplyingToAReplacementChart();
  testResolverRunsInWorkerAndLoadedReplayIsDeliveredOnceOnCaller();
  testReplacementCancelsAndJoinsBlockedLoadBeforeStartingNext();
  testCancelDiscardsSuccessfulResultReturnedAfterCancellation();
  testMissingUnreadableAndMismatchedResultsLeaveFallbackUntouched();
  testDestructionJoinsWorkAndReleasesCapturedSceneCompletion();
}
