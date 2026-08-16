#include "skin/package/SkinPackageOperationService.h"

#include <archive.h>
#include <archive_entry.h>

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
#include <stdexcept>
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
static_assert(std::is_nothrow_move_constructible_v<SkinPackageId>);
static_assert(std::is_nothrow_move_assignable_v<SkinPackageId>);
static_assert(std::is_nothrow_move_constructible_v<PublishPackageResult>);
static_assert(std::is_nothrow_move_constructible_v<RemovePackageResult>);

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

class AcceptingProfiles final : public ISkinProfileSnapshotProvider {
public:
  std::uint64_t beginSnapshotAllProfiles() override { return 1; }
  std::optional<AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t) override {
    return AllSkinProfileSnapshotsResult{
        .complete = true,
        .inventory = ProfileInventorySnapshot{.inventoryGeneration = 1}};
  }
  void cancelSnapshotAllProfiles(std::uint64_t) noexcept override {}
  std::optional<ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) override {
    return makeInventoryCommitFence([] {});
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

class SelectableValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *,
                                std::stop_token) override {
    EntryProfileSettings reconciled;
    SkinEntryMetadataSnapshot metadata;
    metadata.skinType = 0;
    const std::string digest = skinConfigurationDigest(reconciled);
    return {.disposition = SkinValidationDisposition::Selectable7Key,
            .reconciledSettings = std::move(reconciled),
            .metadata = std::move(metadata),
            .configurationDigest = digest};
  }
};

class ThrowingValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *,
                                std::stop_token) override {
    throw std::bad_alloc();
  }
};

void writeText(const fs::path &path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

fs::path makeZip(const fs::path &path, std::string_view member,
                 std::string_view contents) {
  archive *writer = archive_write_new();
  expect(writer != nullptr, "archive coverage allocates a ZIP writer");
  if (!writer) {
    return path;
  }
  expect(archive_write_set_format_zip(writer) == ARCHIVE_OK,
         "archive coverage selects ZIP format");
  expect(archive_write_zip_set_compression_store(writer) == ARCHIVE_OK,
         "archive coverage selects stored compression");
  expect(archive_write_open_filename(writer, path.string().c_str()) ==
             ARCHIVE_OK,
         "archive coverage opens its ZIP output");
  archive_entry *entry = archive_entry_new();
  archive_entry_set_pathname(entry, std::string(member).c_str());
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  expect(archive_write_header(writer, entry) == ARCHIVE_OK,
         "archive coverage writes its member header");
  expect(archive_write_data(writer, contents.data(), contents.size()) ==
             static_cast<la_ssize_t>(contents.size()),
         "archive coverage writes its member data");
  archive_entry_free(entry);
  expect(archive_write_close(writer) == ARCHIVE_OK,
         "archive coverage closes its ZIP output");
  archive_write_free(writer);
  return path;
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

std::optional<PreparedPackage>
prepareFolderForDisposal(SkinPackageOperationService &service,
                         const fs::path &source, SkinPackageId package) {
  const auto handle =
      service.submitPrepareFolder(source, std::move(package), {});
  auto completion = waitFor(service, handle.ticket);
  auto *result = completion
                     ? std::get_if<PreparePackageResult>(&completion->payload)
                     : nullptr;
  if (!result || !result->prepared) {
    return std::nullopt;
  }
  return std::move(result->prepared);
}

void disposeRejectedOffCaller(
    std::optional<RejectedPreparedDisposal> &rejected) {
  if (!rejected) {
    return;
  }
  std::jthread disposalWorker(
      [owned = std::move(*rejected)]() mutable {
        owned.cleanup.run();
        PreparedPackage workerOwned = std::move(owned.prepared);
        (void)workerOwned;
      });
  rejected.reset();
}

SkinStorageRoots rootsBelow(const fs::path &root) {
  fs::create_directories(root / "Documents");
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

void bootstrapStore(SkinPackageStore &store) {
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "operation-service fixture completes recovery before construction");
}

void testConstructionRequiresSuccessfulRecovery() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);

  bool rejected = false;
  try {
    SkinPackageOperationService service(store, validator);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  expect(rejected,
         "operation service refuses construction before recovery succeeds");

  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);
  service.shutdown();
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
  bootstrapStore(store);
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
    auto rejectedPublish = service.submitPublish(
        std::move(*secondResult->prepared), PackageCollisionPolicy::Reject, {},
        SkinDeferredCleanup([&] { ++rejectedDiscardRuns; }));
    expect(rejectedPublish.ticket == 0 &&
               rejectedPublish.rejectedPrepared.has_value() &&
               !rejectedPublish.rejectedCleanup,
           "closed publish returns paired staging and cleanup ownership");
    expect(
        rejectedPublish.rejectedPrepared &&
            rejectedPublish.rejectedPrepared->prepared.visibleStagingRoot() ==
                expectedRoot,
        "closed publish rejection returns the exact prepared staging");
    auto rejected =
        rejectedPublish.rejectedPrepared
            ? service.discardPrepared(
                  std::move(rejectedPublish.rejectedPrepared->prepared),
                  std::move(rejectedPublish.rejectedPrepared->cleanup))
            : std::optional<RejectedPreparedDisposal>{};
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

void testPrepareArchiveReturnsPreparedStaging() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto archive = makeZip(temporary.root() / "service-archive.zip",
                               "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  std::atomic_int cleanupRuns = 0;
  const auto handle = service.submitPrepareArchive(
      archive, {.directoryName = "Archive", .collisionKey = "archive"},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  auto completion = waitFor(service, handle.ticket);
  auto *result = completion
                     ? std::get_if<PreparePackageResult>(&completion->payload)
                     : nullptr;
  expect(result && result->prepared,
         "archive submission returns independently owned prepared staging");
  expect(cleanupRuns == 1,
         "archive submission transfers cleanup to the serialized worker");
  if (result && result->prepared) {
    expect(!service.discardPrepared(std::move(*result->prepared)),
           "archive-prepared staging transfers to worker disposal");
  }
  service.shutdown();
}

void testDetachAndShutdownRunCleanupWithoutReenteringCaller() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
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
  bool failAdmissionAllocation() const noexcept override { return false; }
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
  bool failAdmissionAllocation() const noexcept override { return false; }

  bool cancelBeforeExecution(std::uint64_t ticket) const noexcept override {
    return cancelBeforeExecutionTicket_.load(std::memory_order_acquire) ==
           ticket;
  }

  void cancelBeforeExecution(std::uint64_t ticket) noexcept {
    cancelBeforeExecutionTicket_.store(ticket, std::memory_order_release);
  }

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
  std::atomic_uint64_t cancelBeforeExecutionTicket_ = 0;
};

class FailingAdmissionObserver final : public SkinPackageOperationTestObserver {
public:
  void failNextAdmission() noexcept {
    failNext_.store(true, std::memory_order_release);
  }

  bool failAdmissionAllocation() const noexcept override {
    if (!failNext_.exchange(false, std::memory_order_acq_rel)) {
      return false;
    }
    ++failuresInjected_;
    return true;
  }

  void beforeCompletion(std::uint64_t) const noexcept override {}
  void completed(std::uint64_t) const noexcept override {}
  void disposing(std::uint64_t) const noexcept override {}

  int failuresInjected() const noexcept {
    return failuresInjected_.load(std::memory_order_acquire);
  }

private:
  mutable std::atomic_bool failNext_ = false;
  mutable std::atomic_int failuresInjected_ = 0;
};

class FailingPublishPackageCopyObserver final
    : public SkinPackageOperationTestObserver {
public:
  void failNextCopy() noexcept {
    failNext_.store(true, std::memory_order_release);
  }

  bool failAdmissionAllocation() const noexcept override { return false; }

  bool failPublishPackageCopy() const noexcept override {
    if (!failNext_.exchange(false, std::memory_order_acq_rel)) {
      return false;
    }
    ++failuresInjected_;
    return true;
  }

  void beforeCompletion(std::uint64_t) const noexcept override {}
  void completed(std::uint64_t) const noexcept override {}
  void disposing(std::uint64_t) const noexcept override {}

  int failuresInjected() const noexcept {
    return failuresInjected_.load(std::memory_order_acquire);
  }

private:
  mutable std::atomic_bool failNext_ = false;
  mutable std::atomic_int failuresInjected_ = 0;
};

class FailingPublishTerminalObserver final
    : public SkinPackageOperationTestObserver {
public:
  bool failAdmissionAllocation() const noexcept override { return false; }

  bool failPublishTerminalAllocation() const noexcept override {
    if (injected_.exchange(true, std::memory_order_acq_rel)) {
      return false;
    }
    return true;
  }

  void beforeCompletion(std::uint64_t) const noexcept override {}
  void completed(std::uint64_t) const noexcept override {}
  void disposing(std::uint64_t) const noexcept override {}

  bool injected() const noexcept {
    return injected_.load(std::memory_order_acquire);
  }

private:
  mutable std::atomic_bool injected_ = false;
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
  bootstrapStore(store);
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
  bootstrapStore(store);
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
  bootstrapStore(store);
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

void testAllocationFailureReturnsExactPublishCapabilities() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "allocation-rejection-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  auto observer = std::make_shared<FailingAdmissionObserver>();
  SkinPackageOperationService service(store, validator, observer);

  const auto prepare = service.submitPrepareFolder(
      source, {.directoryName = "Allocation", .collisionKey = "allocation"},
      {});
  auto completion = waitFor(service, prepare.ticket);
  auto *preparedResult =
      completion ? std::get_if<PreparePackageResult>(&completion->payload)
                 : nullptr;
  expect(preparedResult && preparedResult->prepared.has_value(),
         "allocation rejection test first obtains owned prepared staging");
  if (!preparedResult || !preparedResult->prepared) {
    return;
  }

  const auto expectedRoot = preparedResult->prepared->visibleStagingRoot();
  std::atomic_int cleanupRuns = 0;
  observer->failNextAdmission();
  auto rejected = service.submitPublish(
      std::move(*preparedResult->prepared), PackageCollisionPolicy::Reject, {},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  expect(observer->failuresInjected() == 1,
         "enqueue deterministically takes its allocation-failure branch");
  expect(rejected.ticket == 0 && !rejected.progress,
         "allocation failure rejects publish before admission");
  expect(!rejected.rejectedCleanup && rejected.rejectedPrepared.has_value(),
         "publish rejection uses the paired prepared-capability result");
  expect(rejected.rejectedPrepared &&
             rejected.rejectedPrepared->prepared.visibleStagingRoot() ==
                 expectedRoot,
         "allocation rejection returns the exact prepared staging owner");
  expect(cleanupRuns == 0,
         "allocation rejection does not run returned cleanup synchronously");
  if (rejected.rejectedPrepared) {
    auto disposal =
        service.discardPrepared(std::move(rejected.rejectedPrepared->prepared),
                                std::move(rejected.rejectedPrepared->cleanup));
    expect(!disposal,
           "caller can explicitly transfer allocation-rejected capabilities");
  }
  expect(waitUntil([&] { return cleanupRuns == 1; }),
         "transferred allocation-rejected cleanup runs exactly once");
  service.shutdown();
}

void testPublishPackageCopyFailureReturnsExactCapabilities() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "copy-failure-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  auto observer = std::make_shared<FailingPublishPackageCopyObserver>();
  SkinPackageOperationService service(store, validator, observer);

  const auto prepare = service.submitPrepareFolder(
      source, {.directoryName = "CopyFailure", .collisionKey = "copyfailure"},
      {});
  auto completion = waitFor(service, prepare.ticket);
  auto *prepared = completion
                       ? std::get_if<PreparePackageResult>(&completion->payload)
                       : nullptr;
  expect(prepared && prepared->prepared,
         "copy-failure fixture first owns prepared staging");
  if (!prepared || !prepared->prepared) {
    service.shutdown();
    return;
  }

  const auto expectedRoot = prepared->prepared->visibleStagingRoot();
  std::atomic_int cleanupRuns = 0;
  observer->failNextCopy();
  auto rejected = service.submitPublish(
      std::move(*prepared->prepared), PackageCollisionPolicy::Reject, {},
      SkinDeferredCleanup([&] { ++cleanupRuns; }));
  expect(observer->failuresInjected() == 1,
         "publish deterministically reaches its pre-enqueue ID-copy fault");
  expect(rejected.ticket == 0 && rejected.rejectedPrepared &&
             !rejected.rejectedCleanup,
         "ID-copy failure rejects publish with paired capabilities");
  expect(rejected.rejectedPrepared &&
             rejected.rejectedPrepared->prepared.visibleStagingRoot() ==
                 expectedRoot,
         "ID-copy failure returns the exact prepared staging capability");
  expect(cleanupRuns == 0,
         "ID-copy failure leaves cleanup ownership with the caller");
  if (rejected.rejectedPrepared) {
    expect(
        !service.discardPrepared(std::move(rejected.rejectedPrepared->prepared),
                                 std::move(rejected.rejectedPrepared->cleanup)),
        "caller can transfer ID-copy-rejected capabilities for disposal");
  }
  service.shutdown();
  expect(cleanupRuns == 1,
         "transferred ID-copy-rejected cleanup runs exactly once");
}

void testThrowingPublishValidatorReturnsTypedTerminalFailure() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "throwing-publish-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  AcceptingProfiles profiles;
  ThrowingValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  auto observer = std::make_shared<FailingPublishTerminalObserver>();
  SkinPackageOperationService service(store, validator, observer);
  const SkinPackageId package{.directoryName = "ThrowingPublish",
                              .collisionKey = "throwingpublish"};

  const auto prepare = service.submitPrepareFolder(source, package, {});
  auto prepareCompletion = waitFor(service, prepare.ticket);
  auto *prepared =
      prepareCompletion
          ? std::get_if<PreparePackageResult>(&prepareCompletion->payload)
          : nullptr;
  expect(prepared && prepared->prepared,
         "throwing-validator fixture first owns prepared staging");
  if (!prepared || !prepared->prepared) {
    service.shutdown();
    return;
  }

  const auto publish = service.submitPublish(
      std::move(*prepared->prepared), PackageCollisionPolicy::Reject, {});
  auto publishCompletion = waitFor(service, publish.ticket);
  auto *failure =
      publishCompletion
          ? std::get_if<PublishPackageResult>(&publishCompletion->payload)
          : nullptr;
  expect(failure && !failure->published && failure->package == package,
         "throwing Store validation returns the typed publish failure");
  expect(observer->injected(),
         "terminal failure deterministically reaches its allocation fault");
  service.shutdown();
}

void testBoundedBackpressureReturnsCleanupOwnership() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "full-publish-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  const auto publishPrepare = service.submitPrepareFolder(
      source, {.directoryName = "FullPublish", .collisionKey = "fullpublish"},
      {});
  auto publishCompletion = waitFor(service, publishPrepare.ticket);
  auto *publishPrepared =
      publishCompletion
          ? std::get_if<PreparePackageResult>(&publishCompletion->payload)
          : nullptr;
  expect(publishPrepared && publishPrepared->prepared.has_value(),
         "bounded rejection test first obtains publish staging ownership");

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

  std::atomic_int rejectedPublishCleanupRuns = 0;
  std::optional<RejectedPreparedDisposal> pendingPublishDisposal;
  if (publishPrepared && publishPrepared->prepared) {
    const auto expectedRoot = publishPrepared->prepared->visibleStagingRoot();
    auto rejectedPublish = service.submitPublish(
        std::move(*publishPrepared->prepared), PackageCollisionPolicy::Reject,
        {}, SkinDeferredCleanup([&] { ++rejectedPublishCleanupRuns; }));
    expect(rejectedPublish.ticket == 0 &&
               rejectedPublish.rejectedPrepared.has_value() &&
               !rejectedPublish.rejectedCleanup,
           "bounded-full publish returns both capabilities together");
    expect(
        rejectedPublish.rejectedPrepared &&
            rejectedPublish.rejectedPrepared->prepared.visibleStagingRoot() ==
                expectedRoot,
        "bounded-full rejection returns the exact publish staging owner");
    if (rejectedPublish.rejectedPrepared) {
      pendingPublishDisposal.emplace(
          std::move(*rejectedPublish.rejectedPrepared));
    }
  }
  expect(rejectedPublishCleanupRuns == 0,
         "bounded-full publish rejection does not run cleanup on caller");

  service.cancelAndDetach(accepted.front().ticket);
  const auto retryDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (pendingPublishDisposal &&
         std::chrono::steady_clock::now() < retryDeadline) {
    auto rejectedDisposal =
        service.discardPrepared(std::move(pendingPublishDisposal->prepared),
                                std::move(pendingPublishDisposal->cleanup));
    pendingPublishDisposal = std::move(rejectedDisposal);
    if (pendingPublishDisposal) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  expect(!pendingPublishDisposal,
         "bounded rejection capabilities transfer after capacity is released");
  expect(waitUntil([&] { return rejectedPublishCleanupRuns == 1; }),
         "bounded-full publish cleanup runs exactly once after transfer");
  service.shutdown();
  expect(acceptedCleanupRuns == 128,
         "shutdown drains cleanup for every accepted bounded request");
  expect(rejectedCleanupRuns == 1,
         "rejected cleanup remains exactly-once caller ownership");
}

void testReservedDisposalBypassesFullNormalQueueWithoutCallerCleanup() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "reserved-disposal-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  auto reservation = service.reservePreparedDisposal();
  expect(reservation.has_value(),
         "a live service reserves disposal capacity before staging exists");
  auto prepared = prepareFolderForDisposal(
      service, source,
      {.directoryName = "ReservedDisposal", .collisionKey = "reserveddisposal"});
  expect(prepared.has_value(),
         "reserved-disposal fixture obtains prepared staging");
  if (!reservation || !prepared) {
    reservation.reset();
    service.shutdown();
    return;
  }
  const auto stagingRoot = prepared->visibleStagingRoot();

  std::vector<SkinPackageOperationHandle> retained;
  retained.reserve(128);
  for (int index = 0; index < 128; ++index) {
    auto handle = service.submitPrepareFolder(
        temporary.root() / "missing-reserved-capacity-source",
        {.directoryName = "ReservedCapacity" + std::to_string(index),
         .collisionKey = "reservedcapacity" + std::to_string(index)},
        {});
    expect(handle.ticket != 0,
           "normal operation capacity remains independently bounded");
    retained.push_back(std::move(handle));
  }

  std::atomic_int cleanupRuns = 0;
  std::mutex cleanupMutex;
  std::condition_variable cleanupStarted;
  std::condition_variable cleanupRelease;
  bool cleanupIsRunning = false;
  bool releaseCleanup = false;
  std::thread::id cleanupThread;
  auto rejected = service.submitPublish(
      std::move(*prepared), PackageCollisionPolicy::Reject, {},
      SkinDeferredCleanup([&] {
        std::unique_lock lock(cleanupMutex);
        cleanupThread = std::this_thread::get_id();
        ++cleanupRuns;
        cleanupIsRunning = true;
        cleanupStarted.notify_all();
        cleanupRelease.wait(lock, [&] { return releaseCleanup; });
      }));
  expect(rejected.ticket == 0 && rejected.rejectedPrepared.has_value(),
         "a full normal queue returns paired publish capabilities");
  if (!rejected.rejectedPrepared) {
    reservation.reset();
    service.shutdown();
    return;
  }

  const std::thread::id callerThread = std::this_thread::get_id();
  const auto transferStarted = std::chrono::steady_clock::now();
  auto transferRejected = std::move(*reservation).transfer(
      std::move(*rejected.rejectedPrepared));
  const auto transferElapsed =
      std::chrono::steady_clock::now() - transferStarted;
  expect(!transferRejected,
         "reserved disposal accepts capabilities while normal slots are full");
  expect(transferElapsed < std::chrono::milliseconds(100),
         "reserved capability transfer does not wait for cleanup or a slot");

  {
    std::unique_lock lock(cleanupMutex);
    expect(cleanupStarted.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return cleanupIsRunning; }),
           "reserved cleanup starts on the service worker");
  }
  expect(cleanupRuns == 1,
         "reserved transfer invokes cleanup exactly once");
  expect(cleanupThread != callerThread,
         "reserved cleanup never runs on the transferring thread");
  expect(fs::exists(stagingRoot),
         "prepared staging remains owned while worker cleanup is blocked");
  {
    std::scoped_lock lock(cleanupMutex);
    releaseCleanup = true;
  }
  cleanupRelease.notify_all();
  expect(waitUntil([&] { return !fs::exists(stagingRoot); }),
         "prepared staging destruction follows cleanup on the worker");
  disposeRejectedOffCaller(transferRejected);
  service.shutdown();
}

void testReservedDisposalTransferWinsConcurrentShutdown() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "shutdown-reserved-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  auto reservation = service.reservePreparedDisposal();
  auto prepared = prepareFolderForDisposal(
      service, source,
      {.directoryName = "ShutdownReserved", .collisionKey = "shutdownreserved"});
  expect(reservation.has_value() && prepared.has_value(),
         "shutdown-race fixture owns reservation and staging");
  if (!reservation || !prepared) {
    reservation.reset();
    service.shutdown();
    return;
  }
  const auto stagingRoot = prepared->visibleStagingRoot();
  std::atomic_int cleanupRuns = 0;
  RejectedPreparedDisposal disposal{
      .prepared = std::move(*prepared),
      .cleanup = SkinDeferredCleanup([&] { ++cleanupRuns; })};
  std::optional<RejectedPreparedDisposal> transferRejected;
  std::barrier start(3);
  std::jthread transfer([&] {
    start.arrive_and_wait();
    transferRejected =
        std::move(*reservation).transfer(std::move(disposal));
  });
  std::jthread shutdown([&] {
    start.arrive_and_wait();
    service.shutdown();
  });
  start.arrive_and_wait();
  transfer.join();
  shutdown.join();

  expect(!transferRejected,
         "an existing reservation transfers even when shutdown wins the race");
  expect(cleanupRuns == 1,
         "shutdown drains the raced reserved cleanup exactly once");
  expect(!fs::exists(stagingRoot),
         "shutdown drains raced prepared staging before returning");
  disposeRejectedOffCaller(transferRejected);
  service.shutdown();
}

void testReservedCleanupOnlyTransferIsNonblockingAndShutdownDrains() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  auto reservation = service.reservePreparedDisposal();
  expect(reservation.has_value(),
         "cleanup-only disposal reserves the close-safe worker lane");
  if (!reservation) {
    service.shutdown();
    return;
  }

  std::mutex cleanupMutex;
  std::condition_variable cleanupStarted;
  std::condition_variable cleanupRelease;
  bool cleanupIsRunning = false;
  bool releaseCleanup = false;
  std::atomic_int cleanupRuns = 0;
  std::thread::id cleanupThread;
  const std::thread::id callerThread = std::this_thread::get_id();
  const auto transferStarted = std::chrono::steady_clock::now();
  auto rejected = std::move(*reservation).transfer(SkinDeferredCleanup([&] {
    std::unique_lock lock(cleanupMutex);
    cleanupThread = std::this_thread::get_id();
    ++cleanupRuns;
    cleanupIsRunning = true;
    cleanupStarted.notify_all();
    cleanupRelease.wait(lock, [&] { return releaseCleanup; });
  }));
  const auto transferElapsed =
      std::chrono::steady_clock::now() - transferStarted;
  expect(!rejected,
         "cleanup-only capability transfers through its reservation");
  expect(transferElapsed < std::chrono::milliseconds(100),
         "cleanup-only transfer never waits for worker cleanup");
  {
    std::unique_lock lock(cleanupMutex);
    expect(cleanupStarted.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return cleanupIsRunning; }),
           "cleanup-only transfer reaches the service worker");
  }
  expect(cleanupRuns == 1 && cleanupThread != callerThread,
         "cleanup-only transfer runs exactly once off the caller thread");

  std::atomic_bool shutdownReturned = false;
  std::jthread shutdown([&] {
    service.shutdown();
    shutdownReturned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  expect(!shutdownReturned,
         "shutdown waits while reserved cleanup is still running");
  {
    std::scoped_lock lock(cleanupMutex);
    releaseCleanup = true;
  }
  cleanupRelease.notify_all();
  shutdown.join();
  expect(shutdownReturned && cleanupRuns == 1,
         "shutdown drains cleanup-only reserved work exactly once");
}

void testMovedReservationReturnsCleanupOnlyCapabilityIntact() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  auto reservation = service.reservePreparedDisposal();
  expect(reservation.has_value(),
         "moved-reservation fixture acquires the cleanup lane");
  if (!reservation) {
    service.shutdown();
    return;
  }
  auto validOwner =
      std::optional<SkinPreparedDisposalReservation>(std::move(*reservation));
  std::atomic_int returnedCleanupRuns = 0;
  auto returned = std::move(*reservation).transfer(SkinDeferredCleanup([&] {
    ++returnedCleanupRuns;
  }));
  expect(returned.has_value() && returnedCleanupRuns == 0,
         "moved-from reservation returns cleanup without running it");
  if (returned) {
    returned->run();
  }
  expect(returnedCleanupRuns == 1,
         "the caller retains explicit ownership of rejected cleanup");
  validOwner.reset();
  service.shutdown();
}

void testUnusedReservationReleasesShutdownAndPostShutdownRejectsAdmission() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);

  auto reservation = service.reservePreparedDisposal();
  expect(reservation.has_value(),
         "live service admits a prepared-disposal reservation");
  expect(!service.reservePreparedDisposal(),
         "the single close-safe disposal lane rejects excess reservation "
         "before another staging capability exists");
  reservation.reset();
  service.shutdown();
  service.shutdown();
  expect(!service.reservePreparedDisposal(),
         "post-shutdown reservation admission fails explicitly");
}

void testDetachedQueuedRescanSuppressesCompletion() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "block-rescan";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  auto observer = std::make_shared<BlockingCompletionObserver>();
  SkinPackageOperationService service(store, validator, observer);

  const auto blocker = service.submitPrepareFolder(
      source, {.directoryName = "Block", .collisionKey = "block"}, {});
  expect(observer->waitBeforeCompletion(blocker.ticket),
         "the first request holds the serialized worker before rescan starts");
  const auto rescan = service.submitRescan({});
  service.cancelAndDetach(rescan.ticket);
  observer->release();
  service.shutdown();
  expect(!service.poll(rescan.ticket),
         "drained detached rescan suppresses completion delivery");
}

void testQueuedRemoveDeliversTypedCancellationWithoutDetach() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "block-cancelled-remove";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  NoProfiles profiles;
  NoValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  auto observer = std::make_shared<BlockingCompletionObserver>();
  SkinPackageOperationService service(store, validator, observer);

  const auto blocker = service.submitPrepareFolder(
      source, {.directoryName = "CancelBlock", .collisionKey = "cancelblock"},
      {});
  expect(observer->waitBeforeCompletion(blocker.ticket),
         "typed cancellation fixture holds the worker before removal starts");
  const SkinPackageId package{.directoryName = "CancelledRemove",
                              .collisionKey = "cancelledremove"};
  const auto remove = service.submitRemove(package);
  observer->cancelBeforeExecution(remove.ticket);
  observer->release();

  auto completion = waitFor(service, remove.ticket);
  auto *cancelled = completion
                        ? std::get_if<RemovePackageResult>(&completion->payload)
                        : nullptr;
  expect(cancelled && cancelled->cancelled && cancelled->package == package,
         "non-detached queued cancellation returns its typed payload");
  service.shutdown();
}

void testRecoveredServiceForwardsSerializedStoreOperations() {
  TempDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto source = temporary.root() / "service-integration";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  AcceptingProfiles profiles;
  SelectableValidator validator;
  SkinPackageCatalog catalog(roots.privateCatalog);
  SkinPackageStore store(roots, catalog, aliases, profiles);
  bootstrapStore(store);
  SkinPackageOperationService service(store, validator);
  const SkinPackageId package{.directoryName = "ServiceSkin",
                              .collisionKey = "serviceskin"};

  const auto prepared = service.submitPrepareFolder(source, package, {});
  auto preparedCompletion = waitFor(service, prepared.ticket);
  auto *preparedResult =
      preparedCompletion
          ? std::get_if<PreparePackageResult>(&preparedCompletion->payload)
          : nullptr;
  expect(preparedResult && preparedResult->prepared,
         "service integration returns the prepare payload variant");
  if (!preparedResult || !preparedResult->prepared) {
    service.shutdown();
    return;
  }

  const auto published = service.submitPublish(
      std::move(*preparedResult->prepared), PackageCollisionPolicy::Reject, {});
  auto publishedCompletion = waitFor(service, published.ticket);
  auto *publishedResult =
      publishedCompletion
          ? std::get_if<PublishPackageResult>(&publishedCompletion->payload)
          : nullptr;
  expect(publishedResult && publishedResult->published &&
             publishedResult->entries.size() == 1,
         "service integration returns the publish payload variant");
  expect(
      fs::exists(roots.privateCatalog / "catalog.json"),
      "publication serializes its catalog snapshot before service continues");
  if (!publishedResult || publishedResult->entries.empty()) {
    service.shutdown();
    return;
  }

  const auto rescanned = service.submitRescan({});
  auto rescanCompletion = waitFor(service, rescanned.ticket);
  auto *rescanResult =
      rescanCompletion
          ? std::get_if<ScanPackagesResult>(&rescanCompletion->payload)
          : nullptr;
  expect(rescanResult && !rescanResult->cancelled,
         "service integration returns the rescan payload variant");

  const SkinEntryId entry = publishedResult->entries.front();
  VersionedSkinProfileSettings base{
      .profileId = SkinProfileId{.opaque = "service-profile"}, .generation = 7};
  base.settings.selected7KeyEntry = entry;
  base.settings.entries.emplace(entry, EntryProfileSettings{});
  const auto activation =
      service.submitPrepareActivation(base, entry, base.settings);
  auto activationCompletion = waitFor(service, activation.ticket);
  auto *activationResult =
      activationCompletion
          ? std::get_if<PrepareActivationResult>(&activationCompletion->payload)
          : nullptr;
  expect(activationResult && activationResult->prepared,
         "service integration returns the activation-prepare payload variant");
  expect(
      !service
           .acquireValidatedActivation(base.profileId, entry,
                                       std::string(64, 'a'))
           .activation,
      "direct acquisition forwards the activation miss without worker state");

  const auto reconciled =
      service.submitReconcileProfileActivations({base.profileId});
  auto reconcileCompletion = waitFor(service, reconciled.ticket);
  auto *reconcileResult = reconcileCompletion
                              ? std::get_if<ReconcileProfileActivationsResult>(
                                    &reconcileCompletion->payload)
                              : nullptr;
  expect(reconcileResult && reconcileResult->completed,
         "service integration returns the reconciliation payload variant");

  const auto removed = service.submitRemove(package);
  auto removeCompletion = waitFor(service, removed.ticket);
  auto *removeResult =
      removeCompletion
          ? std::get_if<RemovePackageResult>(&removeCompletion->payload)
          : nullptr;
  expect(removeResult && removeResult->removed,
         "service integration returns the remove payload variant");
  expect(service.catalogSnapshot()->packages.empty(),
         "catalog snapshot forwarding observes the serialized removal");

  const auto collected = service.submitGarbageCollection();
  auto collectionCompletion = waitFor(service, collected.ticket);
  expect(collectionCompletion &&
             std::holds_alternative<GarbageCollectionResult>(
                 collectionCompletion->payload),
         "service integration returns the garbage-collection payload variant");

  service.shutdown();
  const auto rejected = service.submitRescan({});
  expect(rejected.ticket == 0 && !rejected.progress &&
             !rejected.rejectedCleanup && !rejected.rejectedPrepared,
         "generic request rejection returns no capability ownership");
}

} // namespace

int main() {
  testConstructionRequiresSuccessfulRecovery();
  testPrepareRequestsHaveFifoTicketsAndIndependentMailboxes();
  testPrepareArchiveReturnsPreparedStaging();
  testDetachAndShutdownRunCleanupWithoutReenteringCaller();
  testCompletedResultDetachWaitsForWorkerDisposal();
  testCompletedShutdownDetachPollRaceHasExactlyOneOwner();
  testShutdownDetachPollRaceWhileWorkerCompletesDisposesExactlyOnce();
  testAllocationFailureReturnsExactPublishCapabilities();
  testPublishPackageCopyFailureReturnsExactCapabilities();
  testThrowingPublishValidatorReturnsTypedTerminalFailure();
  testBoundedBackpressureReturnsCleanupOwnership();
  testReservedDisposalBypassesFullNormalQueueWithoutCallerCleanup();
  testReservedDisposalTransferWinsConcurrentShutdown();
  testReservedCleanupOnlyTransferIsNonblockingAndShutdownDrains();
  testMovedReservationReturnsCleanupOnlyCapabilityIntact();
  testUnusedReservationReleasesShutdownAndPostShutdownRejectsAdmission();
  testDetachedQueuedRescanSuppressesCompletion();
  testQueuedRemoveDeliversTypedCancellationWithoutDetach();
  testRecoveredServiceForwardsSerializedStoreOperations();
  if (failures != 0) {
    std::cerr << failures
              << " skin package operation service assertion(s) failed\n";
    return 1;
  }
  return 0;
}
