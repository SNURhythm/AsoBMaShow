// Uses the controller fixture's real candidates and submission values. No
// network or durable repository is touched by the controlled enqueue callback.
#include <cassert>
#include <chrono>
#include <future>

namespace {
using namespace std::chrono_literals;
using PreparationTask = ir_uploads::PreparationTask;

struct PreparationGate {
  std::promise<void> entered, release;
  std::shared_future<void> released = release.get_future().share();
  void block() {
    entered.set_value();
    assert(released.wait_for(5s) == std::future_status::ready);
  }
  void wait() { assert(entered.get_future().wait_for(5s) == std::future_status::ready); }
};

PreparationTask::Updates takePreparationResult(PreparationTask &task) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto updates = task.takeUpdates();
    if (updates.completion) { return updates; }
    std::this_thread::yield();
  }
  assert(false && "IR preparation did not deliver a result");
  return {};
}

ir::IrSavedResultBatchUploadResult acceptPreparedBatch(
    std::span<const ir::IrSubmission> submissions) {
  ir::IrSavedResultBatchUploadResult result;
  for (const auto &submission : submissions) {
    result.items.push_back({.attemptId = submission.attemptId,
                           .status = ir::IrManualBatchItemStatus::Inserted});
  }
  return result;
}

void testPreparationTaskProgressPartialFailureAndCompletionJoining() {
  PreparationTask task;
  PreparationGate gate;
  const auto applicationThread = std::this_thread::get_id();
  int enqueues = 0;
  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const auto &value, const auto &) {
    assert(std::this_thread::get_id() != applicationThread);
    if (value.modernChartResultId == 1) {
      gate.block();
      return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
    }
    return ir_uploads::VerificationOutcome{.diagnostic = "Unreadable replay"};
  };
  dependencies.enqueueBatch = [&](auto submissions) {
    ++enqueues;
    assert(submissions.size() == 1);
    return acceptPreparedBatch(submissions);
  };
  assert(task.start({candidate(1), candidate(2)}, std::move(dependencies)));
  gate.wait();
  assert(task.hasWorker() && !task.start({candidate(3)}, {}));
  auto progress = task.takeUpdates();
  assert(progress.progress && progress.progress->first == 0 && progress.progress->second == 2);
  assert(!progress.completion && !task.takeUpdates().progress);
  gate.release.set_value();
  const auto updates = takePreparationResult(task);
  assert(!task.hasWorker() && enqueues == 1);
  assert(updates.completion->queuedAttemptIds == attemptIds({1}));
  assert(updates.completion->failedAttemptIds == attemptIds({2}));
  assert(failureReason(*updates.completion, 2) == "Unreadable replay");
  assert(!task.takeUpdates().completion);
}

void testPreparationTaskStopBeforeEnqueueRetainsCancellationUntilConsumed() {
  PreparationTask task;
  std::promise<void> entered, stopped;
  int enqueues = 0;
  assert(task.start({candidate(4)}, {
      .verify = [&](const auto &value, const auto &token) {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        assert(token.stop_requested());
        stopped.set_value();
        return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
      },
      .enqueueBatch = [&](auto submissions) { ++enqueues; return acceptPreparedBatch(submissions); }}));
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  task.stopAndWait();
  assert(stopped.get_future().wait_for(5s) == std::future_status::ready);
  assert(!task.hasWorker() && enqueues == 0);
  assert(!task.start({candidate(5)}, {}));
  task.stopAndWait();
  const auto updates = task.takeUpdates();
  assert(updates.completion && updates.completion->cancelled);
  assert(updates.completion->failedAttemptIds == attemptIds({4}));
  assert(task.start({candidate(5)}, {}));
  const auto next = takePreparationResult(task);
  assert(!next.completion->cancelled && next.completion->failedAttemptIds == attemptIds({5}));
}

void testPreparationTaskStopAfterEnqueueBeginsPreservesCommittedOutcome() {
  PreparationTask task;
  PreparationGate enqueue;
  std::stop_token workerToken;
  assert(task.start({candidate(6)}, {
      .verify = [&](const auto &value, const auto &token) {
        workerToken = token;
        return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
      },
      .enqueueBatch = [&](auto submissions) {
        enqueue.block();
        return acceptPreparedBatch(submissions);
      }}));
  enqueue.wait();
  std::promise<void> stopping;
  auto stop = std::async(std::launch::async, [&] {
    stopping.set_value(); task.stopAndWait();
  });
  assert(stopping.get_future().wait_for(5s) == std::future_status::ready);
  assert(stop.wait_for(50ms) == std::future_status::timeout);
  // The gate decides cancellation before the stop token is signaled.
  assert(!workerToken.stop_requested());
  enqueue.release.set_value();
  assert(stop.wait_for(5s) == std::future_status::ready);
  stop.get();
  const auto updates = task.takeUpdates();
  assert(updates.completion && !updates.completion->cancelled);
  assert(updates.completion->queuedAttemptIds == attemptIds({6}));
  assert(updates.completion->failedAttemptIds.empty());
}

void testPreparationTaskResetAndDestructorOwnCaptureLifetime() {
  PreparationTask task;
  assert(task.start({candidate(7)}, {}));
  task.reset();
  const auto empty = task.takeUpdates();
  assert(!task.hasWorker() && !empty.progress && !empty.completion);
  assert(task.start({candidate(8)}, {}));
  assert(takePreparationResult(task).completion->failedAttemptIds == attemptIds({8}));

  auto owner = std::make_unique<PreparationTask>();
  auto resource = std::make_shared<int>(42);
  std::weak_ptr<int> observed = resource;
  std::promise<void> entered;
  assert(owner->start({candidate(9)}, {
      .verify = [resource, &entered](const auto &, const auto &token) {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        assert(token.stop_requested() && *resource == 42);
        return ir_uploads::VerificationOutcome{};
      }}));
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  resource.reset();
  owner.reset();
  assert(observed.expired());
}
} // namespace
