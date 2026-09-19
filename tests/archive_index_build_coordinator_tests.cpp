#include "archive/IndexBuildCoordinator.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using Coordinator = archive_file::IndexBuildCoordinator;
using Outcome = Coordinator::WaitOutcome;

namespace {
struct CheckpointGate {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  bool checkpoint() {
    entered.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
    return true;
  }
  void wait() { assert(entered.get_future().wait_for(5s) == std::future_status::ready); }
};

void testOneBuilderPerKeyAndIndependentKeys() {
  Coordinator coordinator;
  auto builder = coordinator.acquire("a");
  auto first = coordinator.acquire("a");
  auto second = coordinator.acquire("a");
  auto other = coordinator.acquire("b");
  assert(builder.isBuilder() && !first.isBuilder() && !second.isBuilder());
  assert(other.isBuilder());
  other.complete(true);
  builder.complete(true);
  assert(first.wait({}) == Outcome::Succeeded);
  assert(second.wait({}) == Outcome::Succeeded);
  auto later = coordinator.acquire("a");
  assert(later.isBuilder());
  later.complete(true);
}

// A completed generation must not borrow a later build's result, even when
// its waiter is still inside an application pause callback during replacement.
void testWaitersRetainFailedGenerationWhileLaterRequestsRetry() {
  Coordinator coordinator;
  auto failedBuild = coordinator.acquire("archive");
  auto oldWaiter = coordinator.acquire("archive");
  CheckpointGate gate;
  auto waiting = std::async(std::launch::async, [&] {
    return oldWaiter.wait([&] { return gate.checkpoint(); });
  });
  gate.wait();
  failedBuild.complete(false);
  auto replacement = coordinator.acquire("archive");
  assert(replacement.isBuilder());
  auto newWaiter = coordinator.acquire("archive");
  replacement.complete(true);
  gate.release.set_value();
  assert(waiting.wait_for(5s) == std::future_status::ready);
  assert(waiting.get() == Outcome::Failed);
  assert(newWaiter.wait({}) == Outcome::Succeeded);
}

void testCancelledWaiterDoesNotCancelBuilderOrOtherWaiters() {
  Coordinator coordinator;
  auto builder = coordinator.acquire("archive");
  auto cancelled = coordinator.acquire("archive");
  auto healthy = coordinator.acquire("archive");
  assert(cancelled.wait([] { return false; }) == Outcome::Cancelled);
  auto laterWaiter = coordinator.acquire("archive");
  assert(!laterWaiter.isBuilder());
  builder.complete(true);
  assert(healthy.wait({}) == Outcome::Succeeded);
  assert(laterWaiter.wait({}) == Outcome::Succeeded);
}

void testCheckpointRunsWithoutCoordinatorLockAndCanCancelAfterCompletion() {
  Coordinator coordinator;
  auto builder = coordinator.acquire("archive");
  auto waiter = coordinator.acquire("archive");
  int checkpoints = 0;
  builder.complete(true);
  assert(waiter.wait([&] {
    ++checkpoints;
    auto independent = coordinator.acquire("other");
    assert(independent.isBuilder());
    independent.complete(true);
    return false;
  }) == Outcome::Cancelled);
  assert(checkpoints == 1);
}

void testBuilderExceptionWakesEveryCurrentWaiterWithFailure() {
  Coordinator coordinator;
  auto builder = coordinator.acquire("archive");
  auto first = coordinator.acquire("archive");
  auto second = coordinator.acquire("archive");
  auto a = std::async(std::launch::async, [&] { return first.wait({}); });
  auto b = std::async(std::launch::async, [&] { return second.wait({}); });
  try {
    auto owner = std::move(builder);
    throw std::runtime_error("backend failed");
  } catch (const std::runtime_error &) {}
  assert(a.wait_for(5s) == std::future_status::ready && a.get() == Outcome::Failed);
  assert(b.wait_for(5s) == std::future_status::ready && b.get() == Outcome::Failed);
  auto retry = coordinator.acquire("archive");
  assert(retry.isBuilder());
  retry.complete(true);
}

void testWaiterExceptionDoesNotPoisonCoordination() {
  Coordinator coordinator;
  auto builder = coordinator.acquire("archive");
  try {
    auto waiter = coordinator.acquire("archive");
    waiter.wait([]() -> bool { throw std::runtime_error("pause failed"); });
    assert(false);
  } catch (const std::runtime_error &) {}
  auto healthy = coordinator.acquire("archive");
  assert(!healthy.isBuilder());
  builder.complete(true);
  assert(healthy.wait({}) == Outcome::Succeeded);
  assert(coordinator.acquire("archive").isBuilder());
}

void testMismatchedResultRetryRechecksBuilderAdmission() {
  Coordinator coordinator;
  auto firstBuild = coordinator.acquire("archive");
  auto firstWaiter = coordinator.acquire("archive");
  auto secondWaiter = coordinator.acquire("archive");
  firstBuild.complete(true);
  assert(firstWaiter.wait({}) == Outcome::Succeeded);
  assert(secondWaiter.wait({}) == Outcome::Succeeded);
  // Both callers reject that cached identity outside the coordinator. Only
  // one can own the rebuild when they return to admission.
  auto rebuilding = coordinator.acquire("archive");
  auto following = coordinator.acquire("archive");
  assert(rebuilding.isBuilder() && !following.isBuilder());
  rebuilding.complete(true);
  assert(following.wait({}) == Outcome::Succeeded);
}

void testMovingAndCompletingLeasesCannotFinishAnotherBuild() {
  Coordinator coordinator;
  auto first = coordinator.acquire("first");
  auto firstWaiter = coordinator.acquire("first");
  auto second = coordinator.acquire("second");
  auto secondWaiter = coordinator.acquire("second");
  first = std::move(second);
  assert(!second.isBuilder() && first.isBuilder());
  assert(firstWaiter.wait({}) == Outcome::Failed);
  first.complete(true);
  first.complete(false);
  assert(secondWaiter.wait({}) == Outcome::Succeeded);
  auto retry = coordinator.acquire("second");
  first.complete(false);
  auto retryWaiter = coordinator.acquire("second");
  assert(retry.isBuilder() && !retryWaiter.isBuilder());
  retry.complete(true);
  assert(retryWaiter.wait({}) == Outcome::Succeeded);
}
} // namespace

int main() {
  testOneBuilderPerKeyAndIndependentKeys();
  testWaitersRetainFailedGenerationWhileLaterRequestsRetry();
  testCancelledWaiterDoesNotCancelBuilderOrOtherWaiters();
  testCheckpointRunsWithoutCoordinatorLockAndCanCancelAfterCompletion();
  testBuilderExceptionWakesEveryCurrentWaiterWithFailure();
  testWaiterExceptionDoesNotPoisonCoordination();
  testMismatchedResultRetryRechecksBuilderAdmission();
  testMovingAndCompletingLeasesCannotFinishAnotherBuild();
}
