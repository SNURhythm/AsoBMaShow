#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/SkinAcceptanceRecorder.h"
#include "skin/beatoraja/SkinOverlayDigestProvider.h"
#include "skin/package/SkinPathPolicy.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace skin;

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic_uint64_t serial{0};
    std::error_code error;
    const auto trustedTemporaryRoot =
        fs::canonical(fs::temp_directory_path(), error);
    require(!error && trustedTemporaryRoot.is_absolute(),
            "fixture resolves the platform temporary root once");
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = trustedTemporaryRoot /
            ("asobmashow-overlay-digest-test-" + std::to_string(timestamp) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  [[nodiscard]] const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "visible",
          .privateRevisions = root / "revisions",
          .privateCatalog = root / "catalog",
          .profileOverlays = root / "overlays"};
}

SkinAcceptanceActivationKey activation(std::string_view package = "Fixture",
                                       std::string_view entry = "main.lua") {
  const auto normalizedPackage = normalizePackageId(package);
  require(normalizedPackage.package.has_value(), "fixture package is valid");
  const auto normalizedEntry =
      normalizeEntryPath(*normalizedPackage.package, entry);
  require(normalizedEntry.entry.has_value(), "fixture entry is valid");
  return {
      .profileId = *makeSkinProfileId("11111111-1111-4111-8111-111111111111"),
      .entry = *normalizedEntry.entry,
      .revisionDigest = std::string(64, 'a'),
      .configurationDigest = std::string(64, 'b'),
  };
}

fs::path overlayRoot(const SkinStorageRoots &roots,
                     const SkinAcceptanceActivationKey &key) {
  const auto derived =
      deriveSkinPrivateOverlayRoot(roots, key.profileId, key.entry);
  require(derived.root.has_value(), "fixture overlay identity derives a root");
  return *derived.root;
}

void writeFile(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  require(static_cast<bool>(output), "fixture file write succeeds");
}

SkinOverlayDigestPollResult waitReady(SkinOverlayDigestProvider &provider,
                                      SkinOverlayDigestTicket ticket) {
  for (int attempt = 0; attempt < 5'000; ++attempt) {
    auto result = provider.pollDigest(ticket);
    if (result.state == SkinOverlayDigestPollState::Ready) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {};
}

SkinOverlayDigestPollResult digest(SkinOverlayDigestProvider &provider,
                                   const SkinAcceptanceActivationKey &key) {
  const auto ticket = provider.beginDigest(key);
  require(static_cast<bool>(ticket), "provider accepts a bounded digest job");
  const auto first = provider.pollDigest(ticket);
  if (first.state == SkinOverlayDigestPollState::Ready) {
    return first;
  }
  require(first.state == SkinOverlayDigestPollState::Pending,
          "accepted work is pending or already ready");
  return waitReady(provider, ticket);
}

bool isLowerSha256(std::string_view value) {
  return value.size() == 64 &&
         value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

class StabilityBarrier {
public:
  static void pause(void *context) {
    auto &barrier = *static_cast<StabilityBarrier *>(context);
    std::unique_lock lock(barrier.mutex_);
    barrier.entered_ = true;
    barrier.condition_.notify_all();
    barrier.condition_.wait(lock, [&barrier] { return barrier.released_; });
  }

  void waitUntilEntered() {
    std::unique_lock lock(mutex_);
    require(condition_.wait_for(lock, std::chrono::seconds(5),
                                [this] { return entered_; }),
            "worker reaches the deterministic stability barrier");
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class ThreadGate {
public:
  explicit ThreadGate(std::size_t participants) : participants_(participants) {}

  void arriveAndWait() {
    std::unique_lock lock(mutex_);
    ++arrived_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
  }

  void waitUntilAllArrived() {
    std::unique_lock lock(mutex_);
    require(condition_.wait_for(lock, std::chrono::seconds(5), [this] {
              return arrived_ == participants_;
            }),
            "all shutdown callers are synchronized before release");
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t participants_ = 0;
  std::size_t arrived_ = 0;
  bool released_ = false;
};

void requireSanitizedFailure(const SkinOverlayDigestPollResult &result,
                             const fs::path &hostRoot,
                             std::string_view expectedCode) {
  require(result.state == SkinOverlayDigestPollState::Ready && result.failure,
          "hostile overlay publishes a terminal failure");
  require(result.failure->code == expectedCode,
          "hostile overlay reports the expected stable diagnostic code");
  require(result.failure->message.find(hostRoot.string()) ==
                  std::string::npos &&
              result.failure->virtualPath.empty() && !result.failure->source,
          "overlay failures contain no host path or source location");
}

void testMissingOverlayMatchesAnEmptyTree() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();

  SkinOverlayDigestProvider missing(roots);
  const auto missingResult = digest(missing, key);
  require(!missingResult.failure && missingResult.lowercaseSha256 ==
                                        "d6d34a9e817fa135a1f84824d2e188ac664721"
                                        "07ee7fb7391a3280c70f28b262",
          "a missing overlay produces a safe empty-tree digest");

  fs::create_directories(overlayRoot(roots, key));
  SkinOverlayDigestProvider empty(roots);
  const auto emptyResult = digest(empty, key);
  require(!emptyResult.failure &&
              emptyResult.lowercaseSha256 == missingResult.lowercaseSha256,
          "missing and explicitly empty overlays have one digest");
}

void testTypedRootIsValueOwnedAndTreeDigestIsDeterministic() {
  TemporaryDirectory temporary;
  auto roots = rootsBelow(temporary.root() / "owned");
  const auto ownedRoots = roots;
  const auto key = activation();
  const auto root = overlayRoot(ownedRoots, key);
  writeFile(root / "z.txt", "last");
  writeFile(root / "nested" / "a.txt", "first");
  fs::create_directories(root / "structural" / "empty");

  SkinOverlayDigestProvider first(roots);
  roots.profileOverlays = temporary.root() / "decoy-overlays";
  const auto firstResult = digest(first, key);
  require(!firstResult.failure && firstResult.lowercaseSha256 ==
                                      "7f6ab19d5e43c35672d1402fe3711868a"
                                      "b5e95c8cb3327d5d505063ad37a31e0",
          "provider derives from its value-owned roots and typed activation");

  std::error_code ignored;
  fs::remove_all(root, ignored);
  writeFile(root / "nested" / "a.txt", "first");
  writeFile(root / "z.txt", "last");
  fs::create_directories(root / "different-empty-directory");
  SkinOverlayDigestProvider recreated(ownedRoots);
  const auto recreatedResult = digest(recreated, key);
  require(recreatedResult.lowercaseSha256 == firstResult.lowercaseSha256,
          "creation and directory enumeration order do not affect the digest");

  writeFile(root / "z.txt", "changed");
  SkinOverlayDigestProvider changed(ownedRoots);
  const auto changedResult = digest(changed, key);
  require(changedResult.lowercaseSha256 != firstResult.lowercaseSha256,
          "file content participates in the framed tree digest");
}

void testPollingIsMemoryOnlyAndReadyIsIdempotent() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  const auto root = overlayRoot(roots, key);
  writeFile(root / "value.txt", "before");

  SkinOverlayDigestProvider provider(roots);
  const auto ticket = provider.beginDigest(key);
  require(static_cast<bool>(ticket), "provider accepts the polling fixture");
  auto ready = provider.pollDigest(ticket);
  if (ready.state == SkinOverlayDigestPollState::Pending) {
    ready = waitReady(provider, ticket);
  }
  require(!ready.failure && isLowerSha256(ready.lowercaseSha256),
          "worker publishes a successful digest");

  writeFile(root / "value.txt", "after");
  const auto repeated = provider.pollDigest(ticket);
  require(repeated.state == SkinOverlayDigestPollState::Ready &&
              repeated.lowercaseSha256 == ready.lowercaseSha256,
          "polling a ready ticket returns only retained memory state");
  require(provider.pollDigest({}).state == SkinOverlayDigestPollState::Unknown,
          "an unissued ticket is unknown");
}

void testTraversalAndLinkNodesFailClosedWithoutHostPaths() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  const auto root = overlayRoot(roots, key);
  const auto outside = temporary.root() / "outside.txt";
  writeFile(outside, "private");
  fs::create_directories(root);

  std::error_code linkError;
  fs::create_symlink(outside, root / "escape", linkError);
#if !defined(_WIN32)
  require(!linkError, "POSIX symlink fixture is available");
#endif
  if (!linkError) {
    SkinOverlayDigestProvider linked(roots);
    requireSanitizedFailure(digest(linked, key), temporary.root(),
                            "skin_overlay_digest_unsafe_node");
    fs::remove(root / "escape");
  }

  fs::create_hard_link(outside, root / "hard-link", linkError);
  require(!linkError, "hard-link fixture is available");
  SkinOverlayDigestProvider hardLinked(roots);
  requireSanitizedFailure(digest(hardLinked, key), temporary.root(),
                          "skin_overlay_digest_unsafe_node");
  fs::remove(root / "hard-link");

#if !defined(_WIN32)
  require(::mkfifo((root / "pipe").c_str(), 0600) == 0,
          "FIFO fixture is available");
  SkinOverlayDigestProvider nonregular(roots);
  requireSanitizedFailure(digest(nonregular, key), temporary.root(),
                          "skin_overlay_digest_unsafe_node");
  fs::remove(root / "pipe");
#endif

  auto invalidKey = key;
  invalidKey.entry.packageRelativePath = "../escape.lua";
  SkinOverlayDigestProvider traversal(roots);
  requireSanitizedFailure(digest(traversal, invalidKey), temporary.root(),
                          "skin_overlay_identity_invalid");
}

void testSymlinkedStorageAncestorCannotEscapeTheConfiguredRoot() {
  TemporaryDirectory temporary;
  const auto outside = temporary.root() / "outside";
  fs::create_directories(outside);
  const auto alias = temporary.root() / "alias";
  std::error_code error;
  fs::create_directory_symlink(outside, alias, error);
#if !defined(_WIN32)
  require(!error, "POSIX directory-symlink fixture is available");
#endif
  if (error) {
    return;
  }

  auto roots = rootsBelow(temporary.root());
  roots.profileOverlays = alias / "overlays";
  const auto key = activation();
  writeFile(overlayRoot(roots, key) / "escaped.txt", "must not be read");

  SkinOverlayDigestProvider provider(roots);
  requireSanitizedFailure(digest(provider, key), temporary.root(),
                          "skin_overlay_digest_unsafe_root");
}

void testFileCountAndByteCeilingsFailClosed() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  const auto root = overlayRoot(roots, key);
  writeFile(root / "one", "a");
  writeFile(root / "two", "b");

  SkinOverlayDigestLimits oneFile;
  oneFile.maximumFiles = 1;
  SkinOverlayDigestProvider fileLimited(roots, oneFile);
  requireSanitizedFailure(digest(fileLimited, key), temporary.root(),
                          "skin_overlay_digest_limit_exceeded");

  std::error_code ignored;
  fs::remove_all(root, ignored);
  writeFile(root / "large", "12345");
  SkinOverlayDigestLimits oneFileBytes;
  oneFileBytes.maximumFileBytes = 4;
  SkinOverlayDigestProvider fileByteLimited(roots, oneFileBytes);
  requireSanitizedFailure(digest(fileByteLimited, key), temporary.root(),
                          "skin_overlay_digest_limit_exceeded");

  SkinOverlayDigestLimits totalBytes;
  totalBytes.maximumFileBytes = 8;
  totalBytes.maximumTotalBytes = 4;
  SkinOverlayDigestProvider totalByteLimited(roots, totalBytes);
  requireSanitizedFailure(digest(totalByteLimited, key), temporary.root(),
                          "skin_overlay_digest_limit_exceeded");

  fs::remove_all(root, ignored);
  writeFile(root / "exact", "1234");
  SkinOverlayDigestLimits exactLimits;
  exactLimits.maximumFiles = 1;
  exactLimits.maximumFileBytes = 4;
  exactLimits.maximumTotalBytes = 4;
  SkinOverlayDigestProvider exact(roots, exactLimits);
  const auto exactResult = digest(exact, key);
  require(!exactResult.failure && isLowerSha256(exactResult.lowercaseSha256),
          "file-count and byte ceilings are inclusive at the exact limit");
}

void testPathDepthAndUnicodeCollisionPolicyFailClosed() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  auto root = overlayRoot(roots, key);

  for (std::uint32_t depth = 1;
       depth < SkinPackagePolicy::maxPathComponents; ++depth) {
    root /= "d";
  }
  writeFile(root / "at-limit", "value");
  SkinOverlayDigestProvider atLimit(roots);
  const auto atLimitResult = digest(atLimit, key);
  require(!atLimitResult.failure && isLowerSha256(atLimitResult.lowercaseSha256),
          "exactly 64 virtual path components are accepted");

  std::error_code ignored;
  fs::remove_all(overlayRoot(roots, key), ignored);
  root = overlayRoot(roots, key);

  for (std::uint32_t depth = 0; depth < SkinPackagePolicy::maxPathComponents;
       ++depth) {
    root /= "d";
  }
  writeFile(root / "too-deep", "value");
  SkinOverlayDigestProvider tooDeep(roots);
  requireSanitizedFailure(digest(tooDeep, key), temporary.root(),
                          "skin_overlay_digest_limit_exceeded");

  const auto sharpS = skinPathCollisionKey("Stra\xC3\x9F"
                                           "e");
  const auto folded = skinPathCollisionKey("STRASSE");
  require(sharpS && folded && *sharpS == *folded,
          "full Unicode case folding detects expansion collisions");

  using Node = SkinOverlayDigestProvider::InventoryNodeForTesting;
  require(SkinOverlayDigestProvider::inventoryRejectsNodesForTesting(
              std::array<Node, 2>{
                  Node{.virtualPath = "Stra\xC3\x9F" "e", .directory = false},
                  Node{.virtualPath = "STRASSE", .directory = false}}),
          "inventory rejects a deterministic Unicode file collision");
  require(SkinOverlayDigestProvider::inventoryRejectsNodesForTesting(
              std::array<Node, 2>{
                  Node{.virtualPath = "Assets", .directory = false},
                  Node{.virtualPath = "assets", .directory = true}}),
          "inventory rejects a file-directory collision in one namespace");
}

void testMutationCancellationAndShutdownAreDeterministic() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  const auto root = overlayRoot(roots, key);
  writeFile(root / "value", "before");

  StabilityBarrier mutationBarrier;
  SkinOverlayDigestProvider mutated(roots);
  mutated.setStabilityHookForTesting(&StabilityBarrier::pause,
                                     &mutationBarrier);
  const auto mutationTicket = mutated.beginDigest(key);
  require(static_cast<bool>(mutationTicket), "mutation fixture is accepted");
  mutationBarrier.waitUntilEntered();
  writeFile(root / "value", "after-and-longer");
  mutationBarrier.release();
  requireSanitizedFailure(waitReady(mutated, mutationTicket), temporary.root(),
                          "skin_overlay_digest_source_changed");

  std::error_code ignored;
  fs::remove_all(root, ignored);
  StabilityBarrier appearanceBarrier;
  SkinOverlayDigestProvider appeared(roots);
  appeared.setStabilityHookForTesting(&StabilityBarrier::pause,
                                      &appearanceBarrier);
  const auto appearanceTicket = appeared.beginDigest(key);
  require(static_cast<bool>(appearanceTicket),
          "missing-root mutation fixture is accepted");
  appearanceBarrier.waitUntilEntered();
  writeFile(root / "appeared", "value");
  appearanceBarrier.release();
  requireSanitizedFailure(waitReady(appeared, appearanceTicket),
                          temporary.root(),
                          "skin_overlay_digest_source_changed");

  StabilityBarrier cancellationBarrier;
  SkinOverlayDigestProvider cancelled(roots);
  cancelled.setStabilityHookForTesting(&StabilityBarrier::pause,
                                       &cancellationBarrier);
  const auto cancelledTicket = cancelled.beginDigest(key);
  require(static_cast<bool>(cancelledTicket),
          "active-cancellation fixture is accepted");
  cancellationBarrier.waitUntilEntered();
  cancelled.cancelDigest(cancelledTicket);
  require(cancelled.pollDigest(cancelledTicket).state ==
              SkinOverlayDigestPollState::Unknown,
          "active cancellation immediately suppresses publication");
  cancellationBarrier.release();
  cancelled.shutdown();

  StabilityBarrier shutdownWorkerBarrier;
  StabilityBarrier shutdownJoinBarrier;
  SkinOverlayDigestProvider concurrentShutdown(roots);
  concurrentShutdown.setStabilityHookForTesting(&StabilityBarrier::pause,
                                                &shutdownWorkerBarrier);
  concurrentShutdown.setShutdownHookForTesting(&StabilityBarrier::pause,
                                                &shutdownJoinBarrier);
  const auto shutdownTicket = concurrentShutdown.beginDigest(key);
  require(static_cast<bool>(shutdownTicket),
          "concurrent-shutdown worker fixture is accepted");
  shutdownWorkerBarrier.waitUntilEntered();

  std::array<std::thread, 8> shutdownThreads;
  ThreadGate shutdownGate(shutdownThreads.size());
  for (auto &thread : shutdownThreads) {
    thread = std::thread([&concurrentShutdown, &shutdownGate] {
      shutdownGate.arriveAndWait();
      concurrentShutdown.shutdown();
    });
  }
  shutdownGate.waitUntilAllArrived();
  shutdownGate.release();
  shutdownJoinBarrier.waitUntilEntered();
  shutdownWorkerBarrier.release();
  shutdownJoinBarrier.release();
  for (auto &thread : shutdownThreads) {
    thread.join();
  }
  require(!concurrentShutdown.beginDigest(key),
          "concurrent shutdown closes admission exactly once");
}

void testCancellationChurnAndAllocationFailuresRetainBounds() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto key = activation();
  writeFile(overlayRoot(roots, key) / "value", "data");

  StabilityBarrier activeBarrier;
  SkinOverlayDigestProvider provider(roots);
  provider.setStabilityHookForTesting(&StabilityBarrier::pause, &activeBarrier);
  const auto active = provider.beginDigest(key);
  require(static_cast<bool>(active), "bounded-churn active job is accepted");
  activeBarrier.waitUntilEntered();
  for (std::uint32_t attempt = 0; attempt < 2'000; ++attempt) {
    const auto ticket = provider.beginDigest(
        activation("Churn" + std::to_string(attempt), "main.lua"));
    require(static_cast<bool>(ticket),
            "cancellation churn remains within admission capacity");
    provider.cancelDigest(ticket);
    require(provider.queuedJobCountForTesting() == 0U,
            "cancel removes its not-yet-active queued job");
  }
  provider.cancelDigest(active);
  activeBarrier.release();
  provider.shutdown();

  SkinOverlayDigestProvider allocation(roots);
  allocation.failNextQueueCommitForTesting();
  bool queueFailureObserved = false;
  try {
    (void)allocation.beginDigest(key);
  } catch (const std::bad_alloc &) {
    queueFailureObserved = true;
  }
  require(queueFailureObserved,
          "queue-allocation fixture observes the injected failure");
  std::array<SkinOverlayDigestTicket, 4> afterFailure{};
  for (std::size_t index = 0; index < afterFailure.size(); ++index) {
    afterFailure[index] = allocation.beginDigest(
        activation("AfterFailure" + std::to_string(index), "main.lua"));
    require(static_cast<bool>(afterFailure[index]),
            "failed queue commit leaves no unreachable admission entry");
  }
  require(!allocation.beginDigest(activation("Bounded", "main.lua")),
          "transactional admission still enforces the exact bound");
  for (const auto ticket : afterFailure) {
    allocation.cancelDigest(ticket);
  }

  SkinOverlayDigestProvider resultFailure(roots);
  resultFailure.failNextComputationAndFailureResultForTesting();
  const auto resultFailureTicket = resultFailure.beginDigest(key);
  require(static_cast<bool>(resultFailureTicket),
          "terminal-allocation failure fixture is accepted");
  for (std::uint32_t attempt = 0; attempt < 5'000; ++attempt) {
    if (resultFailure.pollDigest(resultFailureTicket).state ==
        SkinOverlayDigestPollState::Unknown) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(false, "terminal allocation failure cannot strand Pending state");
}

void testOutstandingWorkIsBounded() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  SkinOverlayDigestProvider provider(roots);

  std::array<SkinOverlayDigestTicket, 4> accepted{};
  for (std::size_t index = 0; index < accepted.size(); ++index) {
    accepted[index] = provider.beginDigest(
        activation("Fixture" + std::to_string(index), "main.lua"));
    require(static_cast<bool>(accepted[index]),
            "provider accepts work within its retained-state bound");
  }
  require(!provider.beginDigest(activation("Overflow", "main.lua")),
          "provider rejects work beyond its retained-state bound");
  for (const auto ticket : accepted) {
    provider.cancelDigest(ticket);
  }
}

void testCancelSuppressesPublicationAndShutdownJoins() {
  TemporaryDirectory temporary;
  const auto roots = rootsBelow(temporary.root());
  const auto firstKey = activation();
  const auto secondKey = activation("OtherFixture", "play.lua");
  writeFile(overlayRoot(roots, firstKey) / "large.bin",
            std::string(2 * 1024 * 1024, 'x'));
  writeFile(overlayRoot(roots, secondKey) / "small.bin", "value");

  SkinOverlayDigestProvider provider(roots);
  const auto cancelled = provider.beginDigest(firstKey);
  require(static_cast<bool>(cancelled),
          "provider accepts the cancellation fixture");
  (void)provider.pollDigest(cancelled);
  provider.cancelDigest(cancelled);
  require(provider.pollDigest(cancelled).state ==
              SkinOverlayDigestPollState::Unknown,
          "cancellation immediately suppresses result publication");

  const auto queued = provider.beginDigest(secondKey);
  require(queued && queued.value > cancelled.value,
          "issued tickets remain process-monotonic");
  provider.shutdown();
  provider.shutdown();
  require(!provider.beginDigest(firstKey),
          "shutdown permanently stops accepting work");
  require(provider.pollDigest(queued).state ==
              SkinOverlayDigestPollState::Unknown,
          "shutdown cancels queued or active results before joining");
}

} // namespace

int main() {
  testMissingOverlayMatchesAnEmptyTree();
  testTypedRootIsValueOwnedAndTreeDigestIsDeterministic();
  testPollingIsMemoryOnlyAndReadyIsIdempotent();
  testTraversalAndLinkNodesFailClosedWithoutHostPaths();
  testSymlinkedStorageAncestorCannotEscapeTheConfiguredRoot();
  testFileCountAndByteCeilingsFailClosed();
  testPathDepthAndUnicodeCollisionPolicyFailClosed();
  testMutationCancellationAndShutdownAreDeterministic();
  testCancellationChurnAndAllocationFailuresRetainBounds();
  testOutstandingWorkIsBounded();
  testCancelSuppressesPublicationAndShutdownJoins();
  return 0;
}
