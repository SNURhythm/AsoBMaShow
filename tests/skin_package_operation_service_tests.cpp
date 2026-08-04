#include "skin/package/SkinPackageOperationService.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

static_assert(std::is_move_constructible_v<SkinPackageOperationHandle>);
static_assert(!std::is_copy_constructible_v<SkinPackageOperationHandle>);
static_assert(std::is_move_constructible_v<RejectedPreparedDisposal>);
static_assert(!std::is_copy_constructible_v<RejectedPreparedDisposal>);

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial = 0;
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-skin-operation-service-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class NoProfiles final : public ISkinProfileSnapshotProvider {
public:
  std::uint64_t beginSnapshotAllProfiles() override { return 1; }
  std::optional<AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t) override {
    return std::nullopt;
  }
  void cancelSnapshotAllProfiles(std::uint64_t) noexcept override {}
  std::optional<ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) override {
    return std::nullopt;
  }
  ProfileInventoryMutationBarrier beginInventoryMutation() override {
    std::terminate();
  }
  void finishInventoryMutation(
      ProfileInventoryMutationBarrier &&) noexcept override {}
};

class NoValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *,
                                std::stop_token) override {
    return {};
  }
};

void writeText(const fs::path &path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

bool waitUntil(const std::function<bool()> &condition) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

std::optional<SkinPackageOperationCompletion>
waitFor(SkinPackageOperationService &service, std::uint64_t ticket) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto completion = service.poll(ticket)) {
      return completion;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::nullopt;
}

SkinStorageRoots rootsBelow(const fs::path &root) {
  fs::create_directories(root / "Documents");
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

void testPrepareRequestsHaveFifoTicketsAndIndependentMailboxes() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto sourceOne = temporary.root() / "source-one";
  const auto sourceTwo = temporary.root() / "source-two";
  writeText(sourceOne / "play/play7.luaskin", "return { type = 0 }");
  writeText(sourceTwo / "play/play7.luaskin", "return { type = 0 }");

  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  SkinPackageOperationService service(store, validator);

  const auto first = service.submitPrepareFolder(
      sourceOne, {.directoryName = "One", .collisionKey = "one"}, {});
  const auto second = service.submitPrepareFolder(
      sourceTwo, {.directoryName = "Two", .collisionKey = "two"}, {});

  expect(first.ticket != 0, "first operation has a nonzero ticket");
  expect(second.ticket > first.ticket,
         "operation tickets are strictly monotonic");
  expect(first.progress != second.progress,
         "each request owns a distinct immutable progress mailbox");
  auto firstCompletion = waitFor(service, first.ticket);
  auto secondCompletion = waitFor(service, second.ticket);
  expect(firstCompletion.has_value() && secondCompletion.has_value(),
         "both FIFO preparation requests reach terminal completion");
  expect(firstCompletion && std::holds_alternative<PreparePackageResult>(
                                firstCompletion->payload),
         "first completion retains a typed prepare result");
  expect(secondCompletion && std::holds_alternative<PreparePackageResult>(
                                 secondCompletion->payload),
         "second completion retains a typed prepare result");
  expect(!service.poll(first.ticket).has_value(),
         "poll consumes one completion and cannot affect later ticket state");
  std::atomic_int discardRuns = 0;
  auto *firstResult =
      firstCompletion
          ? std::get_if<PreparePackageResult>(&firstCompletion->payload)
          : nullptr;
  if (firstResult && !firstResult->prepared) {
    for (const auto &diagnostic : firstResult->diagnostics) {
      std::cerr << "prepare diagnostic: " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    }
  }
  expect(firstResult && firstResult->prepared.has_value(),
         "completed preparation retains independently owned staging");
  if (firstResult && firstResult->prepared) {
    const auto rejected =
        service.discardPrepared(std::move(*firstResult->prepared),
                                SkinDeferredCleanup([&] { ++discardRuns; }));
    expect(!rejected, "available discard lane consumes both capabilities");
    expect(waitUntil([&] { return discardRuns == 1; }),
           "discarded staging cleanup runs once on the owned worker");
  }

  auto *secondResult =
      secondCompletion
          ? std::get_if<PreparePackageResult>(&secondCompletion->payload)
          : nullptr;
  expect(secondResult && secondResult->prepared.has_value(),
         "second preparation owns staging for rejection transfer test");
  service.shutdown();
  if (secondResult && secondResult->prepared) {
    const auto expectedRoot = secondResult->prepared->visibleStagingRoot();
    std::atomic_int rejectedDiscardRuns = 0;
    auto rejected = service.discardPrepared(
        std::move(*secondResult->prepared),
        SkinDeferredCleanup([&] { ++rejectedDiscardRuns; }));
    expect(rejected.has_value(),
           "closed discard lane returns both move-only capabilities");
    expect(rejected && rejected->prepared.visibleStagingRoot() == expectedRoot,
           "rejected discard returns the exact prepared staging ownership");
    expect(rejectedDiscardRuns == 0,
           "rejected discard does not invoke cleanup synchronously");
    if (rejected) {
      rejected->cleanup.run();
    }
    expect(rejectedDiscardRuns == 1,
           "caller can explicitly dispose returned cleanup ownership");
  }
}

void testDetachAndShutdownRunCleanupWithoutReenteringCaller() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  SkinPackageOperationService service(store, validator);

  std::atomic_int cleanupRuns = 0;
  const auto handle = service.submitPrepareFolder(
      temporary.root() / "missing-picker-folder",
      {.directoryName = "Missing", .collisionKey = "missing"},
      SkinDeferredCleanup([&] {
        ++cleanupRuns;
        throw 7;
      }));

  const auto start = std::chrono::steady_clock::now();
  service.cancelAndDetach(handle.ticket);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  expect(elapsed < std::chrono::milliseconds(100),
         "cancel and detach returns immediately without callback dispatch");
  service.shutdown();
  service.shutdown();
  expect(cleanupRuns == 1,
         "detached request cleanup is owned by worker and runs exactly once");
  expect(!service.poll(handle.ticket).has_value(),
         "a detached old ticket cannot deliver into any later controller");

  std::atomic_int rejectedCleanupRuns = 0;
  auto rejected = service.submitPrepareFolder(
      temporary.root() / "after-shutdown",
      {.directoryName = "Rejected", .collisionKey = "rejected"},
      SkinDeferredCleanup([&] { ++rejectedCleanupRuns; }));
  expect(rejected.ticket == 0 && !rejected.progress,
         "submission after shutdown is rejected without a reusable ticket");
  expect(rejected.rejectedCleanup.has_value(),
         "zero-ticket rejection returns the exact cleanup ownership");
  expect(rejectedCleanupRuns == 0,
         "rejected submission never runs returned cleanup synchronously");
  if (rejected.rejectedCleanup) {
    rejected.rejectedCleanup->run();
  }
  expect(rejectedCleanupRuns == 1,
         "caller can explicitly dispose rejected cleanup ownership");
}

class BlockingDisposalObserver final : public SkinPackageOperationTestObserver {
public:
  void beforeCompletion(std::uint64_t) const noexcept override {}

  void completed(std::uint64_t ticket) const noexcept override {
    try {
      std::scoped_lock lock(mutex_);
      completedTicket_ = ticket;
      completedCondition_.notify_all();
    } catch (...) {
    }
  }

  void disposing(std::uint64_t ticket) const noexcept override {
    try {
      std::unique_lock lock(mutex_);
      disposalTicket_ = ticket;
      disposalThread_ = std::this_thread::get_id();
      ++disposalCount_;
      disposalCondition_.notify_all();
      releaseCondition_.wait(lock, [this] { return released_; });
    } catch (...) {
    }
  }

  bool waitCompleted(std::uint64_t ticket) const {
    std::unique_lock lock(mutex_);
    return completedCondition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return completedTicket_ == ticket;
    });
  }

  bool waitDisposing(std::uint64_t ticket) const {
    std::unique_lock lock(mutex_);
    return disposalCondition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return disposalTicket_ == ticket;
    });
  }

  void release() const {
    std::scoped_lock lock(mutex_);
    released_ = true;
    releaseCondition_.notify_all();
  }

  std::thread::id disposalThread() const {
    std::scoped_lock lock(mutex_);
    return disposalThread_;
  }

  int disposalCount() const {
    std::scoped_lock lock(mutex_);
    return disposalCount_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable completedCondition_;
  mutable std::condition_variable disposalCondition_;
  mutable std::condition_variable releaseCondition_;
  mutable std::uint64_t completedTicket_ = 0;
  mutable std::uint64_t disposalTicket_ = 0;
  mutable std::thread::id disposalThread_;
  mutable int disposalCount_ = 0;
  mutable bool released_ = false;
};

class BlockingCompletionObserver final
    : public SkinPackageOperationTestObserver {
public:
  void beforeCompletion(std::uint64_t ticket) const noexcept override {
    try {
      std::unique_lock lock(mutex_);
      beforeCompletionTicket_ = ticket;
      beforeCompletionCondition_.notify_all();
      releaseCondition_.wait(lock, [this] { return released_; });
    } catch (...) {
    }
  }

  void completed(std::uint64_t) const noexcept override {
    try {
      std::scoped_lock lock(mutex_);
      ++completedCount_;
    } catch (...) {
    }
  }

  void disposing(std::uint64_t ticket) const noexcept override {
    try {
      std::scoped_lock lock(mutex_);
      disposalTicket_ = ticket;
      ++disposalCount_;
    } catch (...) {
    }
  }

  bool waitBeforeCompletion(std::uint64_t ticket) const {
    std::unique_lock lock(mutex_);
    return beforeCompletionCondition_.wait_for(
        lock, std::chrono::seconds(5),
        [this, ticket] { return beforeCompletionTicket_ == ticket; });
  }

  void release() const {
    std::scoped_lock lock(mutex_);
    released_ = true;
    releaseCondition_.notify_all();
  }

  int completedCount() const {
    std::scoped_lock lock(mutex_);
    return completedCount_;
  }

  int disposalCount(std::uint64_t ticket) const {
    std::scoped_lock lock(mutex_);
    return disposalTicket_ == ticket ? disposalCount_ : 0;
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable beforeCompletionCondition_;
  mutable std::condition_variable releaseCondition_;
  mutable std::uint64_t beforeCompletionTicket_ = 0;
  mutable std::uint64_t disposalTicket_ = 0;
  mutable int completedCount_ = 0;
  mutable int disposalCount_ = 0;
  mutable bool released_ = false;
};

void testCompletedResultDetachWaitsForWorkerDisposal() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "completed-before-detach";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  auto observer = std::make_shared<BlockingDisposalObserver>();
  SkinPackageOperationService service(store, validator, observer);

  std::atomic_int cleanupRuns = 0;
  const auto handle = service.submitPrepareFolder(
      source, {.directoryName = "Complete", .collisionKey = "complete"},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  expect(observer->waitCompleted(handle.ticket),
         "test observer proves result reached Completed before detach");

  const auto start = std::chrono::steady_clock::now();
  service.cancelAndDetach(handle.ticket);
  expect(std::chrono::steady_clock::now() - start <
             std::chrono::milliseconds(100),
         "completed-result detach does not destroy staging on caller");
  expect(observer->waitDisposing(handle.ticket),
         "detached Completed result reaches worker disposal hook");
  expect(observer->disposalThread() != std::this_thread::get_id(),
         "completed PreparedPackage disposal remains on the worker thread");
  observer->release();
  service.shutdown();
  expect(observer->disposalCount() == 1,
         "completed result enters worker disposal exactly once");
  expect(cleanupRuns == 1, "request cleanup remains exactly once");
  expect(!service.poll(handle.ticket),
         "detached completed result is disposed without late delivery");
}

void testCompletedShutdownDetachPollRaceHasExactlyOneOwner() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "completed-race";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  auto observer = std::make_shared<BlockingDisposalObserver>();
  SkinPackageOperationService service(store, validator, observer);

  std::atomic_int cleanupRuns = 0;
  const auto handle = service.submitPrepareFolder(
      source, {.directoryName = "Race", .collisionKey = "race"},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  expect(observer->waitCompleted(handle.ticket),
         "race starts only after the accepted result becomes deliverable");
  observer->release();

  std::barrier start(5);
  std::optional<SkinPackageOperationCompletion> polledCompletion;
  std::jthread firstShutdown([&] {
    start.arrive_and_wait();
    service.shutdown();
  });
  std::jthread secondShutdown([&] {
    start.arrive_and_wait();
    service.shutdown();
  });
  std::jthread detach([&] {
    start.arrive_and_wait();
    service.cancelAndDetach(handle.ticket);
  });
  std::jthread poll([&] {
    start.arrive_and_wait();
    polledCompletion = service.poll(handle.ticket);
  });
  start.arrive_and_wait();
  firstShutdown.join();
  secondShutdown.join();
  detach.join();
  poll.join();

  const bool delivered = polledCompletion.has_value();
  const int disposalCount = observer->disposalCount();
  expect((delivered && disposalCount == 0) ||
             (!delivered && disposalCount == 1),
         "completed shutdown/detach/poll race has exactly one result owner");
  expect(cleanupRuns == 1,
         "concurrent shutdown callers retain exactly-once request cleanup");
  if (polledCompletion) {
    auto *result =
        std::get_if<PreparePackageResult>(&polledCompletion->payload);
    expect(result && result->prepared.has_value(),
           "a poll winner explicitly receives the accepted prepared result");
  } else {
    expect(disposalCount == 1,
           "detach or shutdown explicitly transfers the result to disposal");
  }
  expect(!service.poll(handle.ticket),
         "the completed race cannot deliver the accepted result twice");
}

void testShutdownDetachPollRaceWhileWorkerCompletesDisposesExactlyOnce() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "completion-race";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  auto observer = std::make_shared<BlockingCompletionObserver>();
  SkinPackageOperationService service(store, validator, observer);

  std::atomic_int cleanupRuns = 0;
  const auto handle = service.submitPrepareFolder(
      source,
      {.directoryName = "CompletionRace", .collisionKey = "completion-race"},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  expect(observer->waitBeforeCompletion(handle.ticket),
         "completion race pauses the worker before publishing its result");

  std::barrier start(4);
  std::optional<SkinPackageOperationCompletion> polledCompletion;
  std::jthread shutdown([&] {
    start.arrive_and_wait();
    service.shutdown();
  });
  std::jthread detach([&] {
    start.arrive_and_wait();
    service.cancelAndDetach(handle.ticket);
  });
  std::jthread poll([&] {
    start.arrive_and_wait();
    polledCompletion = service.poll(handle.ticket);
  });
  start.arrive_and_wait();
  detach.join();
  poll.join();
  expect(!polledCompletion,
         "poll cannot steal a result that the worker has not published");
  observer->release();
  shutdown.join();

  expect(cleanupRuns == 1,
         "shutdown racing completion runs accepted cleanup exactly once");
  expect(observer->completedCount() == 0,
         "explicit detach/shutdown prevents a late deliverable completion");
  expect(observer->disposalCount(handle.ticket) == 1,
         "the completion race transfers result disposal to the worker once");
  expect(!service.poll(handle.ticket),
         "the explicitly detached completion cannot be delivered later");
}

void testBoundedBackpressureReturnsCleanupOwnership() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  SkinPackageOperationService service(store, validator);

  std::atomic_int acceptedCleanupRuns = 0;
  std::vector<SkinPackageOperationHandle> accepted;
  accepted.reserve(128);
  for (int index = 0; index < 128; ++index) {
    auto handle = service.submitPrepareFolder(
        temporary.root() / "missing-capacity-source",
        {.directoryName = "Capacity" + std::to_string(index),
         .collisionKey = "capacity" + std::to_string(index)},
        SkinDeferredCleanup([&] { ++acceptedCleanupRuns; }));
    expect(handle.ticket != 0 && !handle.rejectedCleanup,
           "every available bounded slot accepts one owned request");
    accepted.push_back(std::move(handle));
  }

  std::atomic_int rejectedCleanupRuns = 0;
  auto rejected = service.submitPrepareFolder(
      temporary.root() / "missing-over-capacity-source",
      {.directoryName = "OverCapacity", .collisionKey = "overcapacity"},
      SkinDeferredCleanup([&] { ++rejectedCleanupRuns; }));
  expect(rejected.ticket == 0 && rejected.rejectedCleanup.has_value(),
         "bounded backpressure returns cleanup ownership losslessly");
  expect(rejectedCleanupRuns == 0,
         "backpressure does not run cleanup on the submitting thread");
  if (rejected.rejectedCleanup) {
    rejected.rejectedCleanup->run();
  }
  service.shutdown();
  expect(acceptedCleanupRuns == 128,
         "shutdown drains cleanup for every accepted bounded request");
  expect(rejectedCleanupRuns == 1,
         "rejected cleanup remains exactly-once caller ownership");
}

} // namespace

int main() {
  testPrepareRequestsHaveFifoTicketsAndIndependentMailboxes();
  testDetachAndShutdownRunCleanupWithoutReenteringCaller();
  testCompletedResultDetachWaitsForWorkerDisposal();
  testCompletedShutdownDetachPollRaceHasExactlyOneOwner();
  testShutdownDetachPollRaceWhileWorkerCompletesDisposesExactlyOnce();
  testBoundedBackpressureReturnsCleanupOwnership();
  if (failures != 0) {
    std::cerr << failures
              << " skin package operation service assertion(s) failed\n";
    return 1;
  }
  return 0;
}
