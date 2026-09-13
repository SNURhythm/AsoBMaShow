#include "scene/ReplayRecordTask.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <semaphore>
#include <thread>

namespace replay_task_allocation_fault {
thread_local const ReplayRecordTask *armedTask = nullptr;
}

void *operator new(std::size_t size) {
  auto &armedTask = replay_task_allocation_fault::armedTask;
  if (armedTask != nullptr && armedTask->active()) {
    armedTask = nullptr;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testThreadStartupFailureReleasesAdmissionAndAllowsRetry() {
  ReplayRecordTask task;
  std::atomic_int calls = 0;
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> retained = payload;
  ReplayRecordTask::Work work = [&, payload](std::shared_ptr<std::atomic_bool>) {
    ++calls;
  };
  payload.reset();
  bool threw = false;
  // Allow cancellation-token allocation, then fail real thread startup after
  // the production task has claimed active ownership.
  replay_task_allocation_fault::armedTask = &task;
  try {
    task.start(std::move(work));
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  replay_task_allocation_fault::armedTask = nullptr;
  expect(threw, "thread startup allocation failure propagates to the caller");
  expect(!task.active(), "thread startup failure releases active ownership");
  expect(calls == 0 && retained.expired(),
         "failed startup releases worker captures without executing work");
  task.publish([&] { ++calls; });
  expect(!task.takeCompletion(), "failed startup rejects later completion publication");

  std::binary_semaphore published{0};
  std::atomic_bool freshToken = false;
  task.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
    freshToken = !cancelled->load();
    task.publish([&] { ++calls; });
    published.release();
  });
  published.acquire();
  expect(task.active() && freshToken,
         "retry owns fresh uncancelled work until completion is delivered");
  auto completion = task.takeCompletion();
  expect(static_cast<bool>(completion) && !task.active(),
         "retry delivers completion and releases active ownership");
  if (completion) completion();
  expect(calls == 1 && !task.takeCompletion(), "retry delivers its completion exactly once");
}

void testCompletionIsDeliveredAfterWorkerExitOnTheCaller() {
  ReplayRecordTask task;
  std::binary_semaphore published{0};
  std::binary_semaphore releaseWorker{0};
  std::binary_semaphore takingCompletion{0};
  std::atomic_bool workerExited = false;
  std::atomic_int calls = 0;
  std::thread::id workerThread;
  std::thread::id completionThread;
  const auto caller = std::this_thread::get_id();
  task.start([&](std::shared_ptr<std::atomic_bool>) {
    workerThread = std::this_thread::get_id();
    task.publish([&] {
      completionThread = std::this_thread::get_id();
      ++calls;
    });
    published.release();
    releaseWorker.acquire();
    workerExited = true;
  });
  published.acquire();
  expect(task.active(), "a published result retains active ownership");
  expect(calls == 0, "publishing must not execute the UI completion");
  auto pending = std::async(std::launch::async, [&] {
    takingCompletion.release();
    return task.takeCompletion();
  });
  takingCompletion.acquire();
  expect(pending.wait_for(std::chrono::milliseconds(20)) ==
             std::future_status::timeout,
         "completion cannot be delivered while its worker still owns resources");
  releaseWorker.release();
  auto completion = pending.get();
  expect(static_cast<bool>(completion), "the caller receives the completion");
  expect(workerExited, "taking a completion joins its worker first");
  expect(!task.active(), "delivering a completion releases active ownership");
  if (completion) completion();
  expect(calls == 1 && completionThread == caller && workerThread != caller,
         "work runs in the background and completion runs on its caller");
  expect(!task.takeCompletion(), "a completion is delivered only once");
}

void testPollingWithoutCompletionKeepsTheWorkerActive() {
  ReplayRecordTask task;
  std::binary_semaphore entered{0};
  std::binary_semaphore release{0};
  task.start([&](std::shared_ptr<std::atomic_bool>) {
    entered.release();
    release.acquire();
  });
  entered.acquire();
  expect(!task.takeCompletion(), "polling an empty mailbox returns no work");
  expect(task.active(), "empty polling must not release worker ownership");
  release.release();
  task.cancelAndWait();
  expect(!task.active(), "cancelling work without a result releases ownership");
}

void testPreparationCanCancelWithoutPublishingAResult() {
  ReplayRecordTask task;
  task.start([](std::shared_ptr<std::atomic_bool> cancelled) { cancelled->store(true); });
  ReplayRecordTask::Completion completion;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!completion && std::chrono::steady_clock::now() < deadline) {
    completion = task.takeCompletion();
    std::this_thread::yield();
  }
  expect(static_cast<bool>(completion) && !task.active(),
         "self-cancelled preparation delivers a terminal notification and releases ownership");
  if (completion) completion();
  expect(!task.takeCompletion(), "a terminal notification is delivered only once");
}

void testCancellationDiscardsQueuedAndLateCompletions() {
  ReplayRecordTask task;
  std::binary_semaphore entered{0};
  std::atomic_bool workerExited = false;
  int calls = 0;
  task.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
    task.publish([&] { ++calls; });
    entered.release();
    while (!cancelled->load()) std::this_thread::yield();
    task.publish([&] { ++calls; });
    workerExited = true;
  });
  entered.acquire();
  task.cancelAndWait();
  expect(workerExited, "cancellation waits for the worker to stop");
  expect(!task.active(), "cancellation releases active ownership");
  expect(!task.takeCompletion(), "queued and late results are discarded");
  task.publish([&] { ++calls; });
  expect(!task.takeCompletion(), "a cancelled task rejects later publication");
  expect(calls == 0, "cancellation never executes discarded UI callbacks");
}

void testRestartReplacesOldCompletionAndCancellationAuthority() {
  ReplayRecordTask task;
  std::binary_semaphore firstPublished{0};
  std::binary_semaphore secondPublished{0};
  std::shared_ptr<std::atomic_bool> firstToken;
  std::atomic_bool firstExited = false;
  std::atomic_bool replacementSawFirstExit = false;
  std::atomic_bool replacementHasFreshToken = false;
  int chosen = 0;
  task.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
    firstToken = cancelled;
    task.publish([&] { chosen = 1; });
    firstPublished.release();
    firstExited = true;
  });
  firstPublished.acquire();
  task.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
    replacementSawFirstExit = firstExited.load();
    replacementHasFreshToken = cancelled != firstToken && !cancelled->load();
    task.publish([&] { chosen = 2; });
    task.publish([&] { chosen = 3; });
    secondPublished.release();
  });
  secondPublished.acquire();
  auto completion = task.takeCompletion();
  if (completion) completion();
  expect(replacementSawFirstExit, "replacement starts after the old worker exits");
  expect(!firstToken->load(), "replacement joins without cancelling old work");
  expect(replacementHasFreshToken, "each start has its own cancellation token");
  expect(chosen == 3, "replacement discards the old result and keeps the latest");

  task.cancelAndWait();
  std::binary_semaphore restarted{0};
  task.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
    expect(!cancelled->load(), "a cancelled task can start fresh work");
    task.publish([&] { chosen = 4; });
    restarted.release();
  });
  restarted.acquire();
  completion = task.takeCompletion();
  if (completion) completion();
  expect(chosen == 4, "a new start delivers results after cancellation");
}

void testDiscardedCapturesAreDestroyedOutsideTheMailboxLock() {
  enum class Discard { Cancel, Restart, Replace };
  for (const auto action : {Discard::Cancel, Discard::Restart, Discard::Replace}) {
    ReplayRecordTask task;
    std::binary_semaphore queued{0}, replace{0}, replacementQueued{0};
    std::binary_semaphore destroying{0}, publicationReturned{0};
    std::atomic_bool mailboxUnlocked = false, captureDestroyed = false;
    std::atomic_bool replacementSawCleanup = false;
    int discardedCalls = 0, chosen = 0;
    struct Capture {
      std::binary_semaphore &destroying, &publicationReturned;
      std::atomic_bool &mailboxUnlocked, &captureDestroyed;
      ~Capture() {
        // An external worker publishes during capture cleanup. A bounded wait
        // detects a held mailbox lock without hanging the regression runner.
        destroying.release();
        mailboxUnlocked = publicationReturned.try_acquire_for(std::chrono::seconds(2));
        captureDestroyed = true;
      }
    };
    std::jthread observer([&] {
      destroying.acquire();
      task.publish([&] { chosen = 3; });
      publicationReturned.release();
    });
    auto capture = std::make_shared<Capture>(destroying, publicationReturned,
                                             mailboxUnlocked, captureDestroyed);
    task.start([&, capture](std::shared_ptr<std::atomic_bool>) mutable {
      task.publish([capture = std::move(capture), &discardedCalls] { ++discardedCalls; });
      queued.release();
      if (action == Discard::Replace) {
        replace.acquire();
        task.publish([&] { chosen = 2; });
        replacementQueued.release();
      }
    });
    queued.acquire();
    capture.reset();
    if (action == Discard::Cancel) {
      task.cancelAndWait();
    } else if (action == Discard::Restart) {
      task.start([&](std::shared_ptr<std::atomic_bool>) {
        replacementSawCleanup = captureDestroyed.load();
        replace.acquire();
        task.publish([&] { chosen = 2; });
        replacementQueued.release();
      });
    } else {
      replace.release();
      replacementQueued.acquire();
    }
    observer.join();
    expect(mailboxUnlocked,
           "discarded completion capture cleanup must run outside the mailbox lock");
    if (action == Discard::Restart) {
      replace.release();
      replacementQueued.acquire();
      expect(replacementSawCleanup,
             "restart releases old completion captures before new work begins");
    }
    auto completion = task.takeCompletion();
    if (completion) completion();
    expect(discardedCalls == 0, "discarding completion ownership never invokes the old action");
    if (action == Discard::Cancel) {
      expect(!completion && chosen == 0 && !task.active(),
             "cancellation still rejects publication during capture cleanup");
    } else {
      expect(completion && chosen == (action == Discard::Restart ? 2 : 3),
             "the latest accepted publication survives capture cleanup");
    }
    task.cancelAndWait();
  }
}

void testDestructionCancelsBeforeReleasingWorkerAndMailboxStorage() {
  std::binary_semaphore entered{0};
  std::atomic_bool workerExited = false;
  std::atomic_int calls = 0;
  auto payload = std::make_shared<int>(7);
  std::weak_ptr<int> retained = payload;
  auto task = std::make_unique<ReplayRecordTask>();
  auto *owner = task.get();
  task->start([&, owner, payload](std::shared_ptr<std::atomic_bool> cancelled) {
    owner->publish([&, payload] { ++calls; });
    entered.release();
    while (!cancelled->load()) std::this_thread::yield();
    owner->publish([&, payload] { ++calls; });
    workerExited = true;
  });
  entered.acquire();
  payload.reset();
  task.reset();
  expect(workerExited, "destruction joins work before destroying task storage");
  expect(retained.expired(), "destruction releases worker and callback captures");
  expect(calls == 0, "destruction never executes queued or late UI work");
}
} // namespace

int main() {
  testThreadStartupFailureReleasesAdmissionAndAllowsRetry();
  testCompletionIsDeliveredAfterWorkerExitOnTheCaller();
  testPollingWithoutCompletionKeepsTheWorkerActive();
  testPreparationCanCancelWithoutPublishingAResult();
  testCancellationDiscardsQueuedAndLateCompletions();
  testRestartReplacesOldCompletionAndCancellationAuthority();
  testDiscardedCapturesAreDestroyedOutsideTheMailboxLock();
  testDestructionCancelsBeforeReleasingWorkerAndMailboxStorage();
  return failures == 0 ? 0 : 1;
}
