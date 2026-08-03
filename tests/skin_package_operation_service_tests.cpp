#include "skin/package/SkinPackageOperationService.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

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
  const auto secondCompletion = waitFor(service, second.ticket);
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
    service.discardPrepared(std::move(*firstResult->prepared),
                            SkinDeferredCleanup([&] { ++discardRuns; }));
    expect(waitUntil([&] { return discardRuns == 1; }),
           "discarded staging cleanup runs once on the owned worker");
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
  const auto rejected = service.submitPrepareFolder(
      temporary.root() / "after-shutdown",
      {.directoryName = "Rejected", .collisionKey = "rejected"},
      SkinDeferredCleanup([&] { ++rejectedCleanupRuns; }));
  expect(rejected.ticket == 0 && !rejected.progress,
         "submission after shutdown is rejected without a reusable ticket");
  expect(rejectedCleanupRuns == 0,
         "rejected submission never runs caller cleanup synchronously");
}

} // namespace

int main() {
  testPrepareRequestsHaveFifoTicketsAndIndependentMailboxes();
  testDetachAndShutdownRunCleanupWithoutReenteringCaller();
  if (failures != 0) {
    std::cerr << failures
              << " skin package operation service assertion(s) failed\n";
    return 1;
  }
  return 0;
}
