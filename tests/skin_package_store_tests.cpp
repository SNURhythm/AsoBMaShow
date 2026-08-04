#include "skin/SkinCommitCoordinator.h"
#include "skin/package/SkinPackageOperationService.h"
#include "skin/package/SkinPackageStore.h"
#include "skin/package/SkinPathPolicy.h"
#include "FileChecksum.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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
    static std::atomic_uint64_t serial{0};
    root_ = fs::canonical(fs::temp_directory_path()) /
            ("asobmashow-skin-store-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
    fs::create_directories(root_);
  }

  ~TempDirectory() {
    std::error_code ignored;
    for (fs::recursive_directory_iterator iterator(root_, ignored), end;
         !ignored && iterator != end; ++iterator) {
      if (iterator->is_directory(ignored)) {
        fs::permissions(iterator->path(), fs::perms::owner_all,
                        fs::perm_options::add, ignored);
      }
    }
    ignored.clear();
    fs::permissions(root_, fs::perms::owner_all, fs::perm_options::add,
                    ignored);
    ignored.clear();
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

SkinStorageRoots rootsBelow(const fs::path &root) {
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

void writeText(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
}

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class BlockingAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    return SkinRejectedLinkKind::None;
  }

  bool waitUntilEntered(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() const {
    std::scoped_lock lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable bool entered_ = false;
  mutable bool released_ = false;
};

class FakeProfileSnapshots final : public ISkinProfileSnapshotProvider {
public:
  std::optional<ProfileInventoryCommitFence> issueFence() {
    return makeInventoryCommitFence([this] { ++releaseCount; });
  }

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
    if (beforeFence) {
      beforeFence();
    }
    if (rejectFence) {
      return std::nullopt;
    }
    return issueFence();
  }

  ProfileInventoryMutationBarrier beginInventoryMutation() override {
    throw std::logic_error("not used by catalog RED tests");
  }

  void finishInventoryMutation(
      ProfileInventoryMutationBarrier &&) noexcept override {}

  int releaseCount = 0;
  bool rejectFence = false;
  std::function<void()> beforeFence;
};

class SelectableValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *desired,
                                std::stop_token) override {
    ++calls;
    const bool cancelNow =
        cancelled || (cancelConfigured && desired != nullptr);
    if (cancelNow || (rejectConfigured && desired != nullptr)) {
      return {.disposition = rejectConfigured
                                 ? SkinValidationDisposition::Invalid
                                 : SkinValidationDisposition::Selectable7Key,
              .cancelled = cancelNow};
    }
    if (disposition != SkinValidationDisposition::Selectable7Key) {
      return {.disposition = disposition};
    }
    SkinEntryMetadataSnapshot metadata;
    metadata.displayName = "Synthetic Play";
    metadata.author = "AsoBMaShow tests";
    metadata.skinType = 0;
    metadata.authoredWidth = 1280;
    metadata.authoredHeight = 720;
    for (std::size_t index = 0; index < metadataDeclarations; ++index) {
      metadata.categories.push_back(SkinCatalogCategoryDeclaration{
          .name = "category-" + std::to_string(index)});
    }
    return {.disposition = SkinValidationDisposition::Selectable7Key,
            .reconciledSettings = EntryProfileSettings{},
            .metadata = std::move(metadata),
            .configurationDigest = configurationDigest};
  }

  SkinValidationDisposition disposition =
      SkinValidationDisposition::Selectable7Key;
  std::string configurationDigest =
      "1111111111111111111111111111111111111111111111111111111111111111";
  bool rejectConfigured = false;
  bool cancelled = false;
  bool cancelConfigured = false;
  std::size_t metadataDeclarations = 0;
  int calls = 0;
};

class OneShotThrowingObserver final : public SkinImportIoObserver {
public:
  explicit OneShotThrowingObserver(std::function<void()> beforeThrow = {})
      : beforeThrow_(std::move(beforeThrow)) {}

  void reached(SkinImportIoOperation operation,
               const fs::path &) const override {
    if (operation == SkinImportIoOperation::BeforeVisiblePublication &&
        throwsRemaining.fetch_sub(1) > 0) {
      if (beforeThrow_) {
        beforeThrow_();
      }
      throw std::runtime_error("injected visible publication failure");
    }
  }

private:
  std::function<void()> beforeThrow_;
  mutable std::atomic_int throwsRemaining{1};
};

class RemovalMutationObserver final : public SkinPackageStoreIoObserver {
public:
  explicit RemovalMutationObserver(std::function<void()> mutate)
      : mutate_(std::move(mutate)) {}

  void reached(SkinPackageStoreIoOperation operation,
               const fs::path &) const noexcept override {
    if (operation != SkinPackageStoreIoOperation::RemovalVisibleRetained) {
      return;
    }
    try {
      mutate_();
    } catch (...) {
    }
  }

private:
  std::function<void()> mutate_;
};

class FakeProfileOwner final : public ISkinProfileSettingsOwner {
public:
  explicit FakeProfileOwner(VersionedSkinProfileSettings initial)
      : current(std::move(initial)) {}

  VersionedSkinProfileSettings snapshot(const SkinProfileId &) const override {
    return current;
  }

  SkinProfileCommitResult beginCommit(const SkinProfileId &,
                                      std::uint64_t expectedGeneration,
                                      SkinProfileSettings candidate) override {
    if (throwBegin) {
      throw std::runtime_error("injected owner begin failure");
    }
    if (expectedGeneration != current.generation) {
      return {.status = SkinProfileCommitResult::Status::GenerationChanged,
              .generationChanged = true,
              .snapshot = current};
    }
    pending = std::move(candidate);
    return {.status = SkinProfileCommitResult::Status::Pending, .ticket = 77};
  }

  SkinProfileCommitResult pollCommit(std::uint64_t ticket) override {
    expect(ticket == 77, "store polls the exact retained owner ticket");
    if (throwPollOnce) {
      throwPollOnce = false;
      throw std::runtime_error("injected owner poll failure");
    }
    if (terminalStatus == SkinProfileCommitResult::Status::GenerationChanged) {
      return {.status = SkinProfileCommitResult::Status::GenerationChanged,
              .ticket = ticket,
              .generationChanged = true,
              .snapshot = current};
    }
    if (terminalStatus == SkinProfileCommitResult::Status::RetryableFailure) {
      return {.status = SkinProfileCommitResult::Status::RetryableFailure,
              .ticket = ticket,
              .failure = SkinDiagnostic{.code = "injected_retry",
                                        .message = "retry profile save",
                                        .severity = DiagnosticSeverity::Error}};
    }
    if (!persisted) {
      return {.status = SkinProfileCommitResult::Status::Pending,
              .ticket = ticket};
    }
    if (pending) {
      current.settings = std::move(*pending);
      pending.reset();
      ++current.generation;
    }
    return {.status = SkinProfileCommitResult::Status::Persisted,
            .ticket = ticket,
            .snapshot = current};
  }

  void acknowledgeCommit(std::uint64_t ticket) noexcept override {
    if (ticket == 77) {
      ++acknowledgements;
    }
  }

  VersionedSkinProfileSettings current;
  std::optional<SkinProfileSettings> pending;
  bool persisted = false;
  bool throwBegin = false;
  bool throwPollOnce = false;
  SkinProfileCommitResult::Status terminalStatus =
      SkinProfileCommitResult::Status::Pending;
  int acknowledgements = 0;
};

constexpr std::string_view kOldTreeDigest =
    "e2a1d82523f74a9ac8c8a500848fb173055c60d2430b3a8d0f67d6a6e0052ee1";
constexpr std::string_view kNewTreeDigest =
    "9ec86e2e753614c8b0667f1aa0a67784c1a97337eb348c3984f5fd492abd9fd5";
constexpr std::string_view kUnreferencedTreeDigest =
    "d07465eeb4d8f2ef47c8c17cfaa7cbae798ac9a246dd0230e5b5503dfbc65647";
constexpr std::string_view kOldCatalogDigest =
    "49deb5306b817e688d1085a9f56ac2461664b7650e5673294619882ecd700510";
constexpr std::string_view kNewCatalogDigest =
    "db1cdde7ff9115aa0c8aa1b8d7d539375cc88aa29556c43bc9683f7517af7e7b";

std::string catalogDocument(std::uint64_t generation,
                            std::string_view revisionDigest) {
  return "{\"schemaVersion\":1,\"catalogGeneration\":" +
         std::to_string(generation) +
         ",\"sourceGeneration\":" + std::to_string(generation) +
         ",\"packages\":[{\"directoryName\":\"FixtureSkin\","
         "\"collisionKey\":\"fixtureskin\"}],\"entries\":[{\"entry\":{"
         "\"package\":{\"directoryName\":\"FixtureSkin\","
         "\"collisionKey\":\"fixtureskin\"},\"packageRelativePath\":"
         "\"play/play7.luaskin\",\"collisionKey\":"
         "\"fixtureskin/play/play7.luaskin\"},\"revisionDigest\":\"" +
         std::string(revisionDigest) +
         "\",\"validation\":\"selectable7Key\","
         "\"validatedConfigurationDigests\":[]}]}\n";
}

std::string journalDocument(std::string_view phase) {
  return "{\"schemaVersion\":1,\"operation\":\"replace-package\","
         "\"operationId\":\"op-17\",\"phase\":\"" +
         std::string(phase) +
         "\",\"package\":{\"directoryName\":\"FixtureSkin\","
         "\"collisionKey\":\"fixtureskin\"},\"visible\":{"
         "\"destinationDirectory\":\"FixtureSkin\","
         "\"stagingToken\":\"import-op-17\","
         "\"backupToken\":\"op-17\",\"oldPresent\":true,"
         "\"oldTreeDigest\":\"" +
         std::string(kOldTreeDigest) + "\",\"newTreeDigest\":\"" +
         std::string(kNewTreeDigest) + "\"},\"revision\":{\"oldDigest\":\"" +
         std::string(kOldTreeDigest) + "\",\"newDigest\":\"" +
         std::string(kNewTreeDigest) +
         "\",\"stagingToken\":\"revision-op-17\"},\"catalog\":{"
         "\"fileName\":\"catalog.json\","
         "\"stagingToken\":\"op-17-new.json\","
         "\"backupToken\":\"op-17-old.json\",\"oldGeneration\":7,"
         "\"newGeneration\":8,\"oldSourceGeneration\":7,"
         "\"newSourceGeneration\":8,\"oldSnapshotDigest\":\"" +
         std::string(kOldCatalogDigest) + "\",\"newSnapshotDigest\":\"" +
         std::string(kNewCatalogDigest) + "\"}}\n";
}

std::string emptyCatalogDocument(std::uint64_t generation = 7) {
  return "{\"schemaVersion\":1,\"catalogGeneration\":" +
         std::to_string(generation) +
         ",\"sourceGeneration\":" + std::to_string(generation) +
         ",\"packages\":[],\"entries\":[]}\n";
}

std::string removalJournalDocument(std::string_view oldCatalogDigest,
                                   std::string_view newCatalogDigest) {
  return "{\"schemaVersion\":1,\"operation\":\"remove-package\","
         "\"operationId\":\"remove-17\",\"package\":{"
         "\"directoryName\":\"FixtureSkin\",\"collisionKey\":"
         "\"fixtureskin\"},\"retainedToken\":\"remove-17\","
         "\"oldTreeDigest\":\"" +
         std::string(kOldTreeDigest) +
         "\",\"catalog\":{\"stagingToken\":\"remove-17-new.json\","
         "\"backupToken\":\"remove-17-old.json\",\"oldGeneration\":7,"
         "\"newGeneration\":8,\"oldSourceGeneration\":7,"
         "\"newSourceGeneration\":8,\"oldSnapshotDigest\":\"" +
         std::string(oldCatalogDigest) + "\",\"newSnapshotDigest\":\"" +
         std::string(newCatalogDigest) + "\"}}\n";
}

std::string createJournalDocument(std::string_view phase,
                                  std::string_view oldCatalogDigest) {
  return "{\"schemaVersion\":1,\"operation\":\"replace-package\","
         "\"operationId\":\"create-17\",\"phase\":\"" +
         std::string(phase) +
         "\",\"package\":{\"directoryName\":\"FixtureSkin\","
         "\"collisionKey\":\"fixtureskin\"},\"visible\":{"
         "\"destinationDirectory\":\"FixtureSkin\","
         "\"stagingToken\":\"import-create-17\","
         "\"backupToken\":\"create-17\",\"oldPresent\":false,"
         "\"newTreeDigest\":\"" +
         std::string(kNewTreeDigest) + "\"},\"revision\":{\"newDigest\":\"" +
         std::string(kNewTreeDigest) +
         "\",\"stagingToken\":\"revision-create-17\"},\"catalog\":{"
         "\"fileName\":\"catalog.json\","
         "\"stagingToken\":\"create-17-new.json\","
         "\"backupToken\":\"create-17-old.json\",\"oldGeneration\":7,"
         "\"newGeneration\":8,\"oldSourceGeneration\":7,"
         "\"newSourceGeneration\":8,\"oldSnapshotDigest\":\"" +
         std::string(oldCatalogDigest) + "\",\"newSnapshotDigest\":\"" +
         std::string(kNewCatalogDigest) + "\"}}\n";
}

void writeOldTree(const fs::path &root) {
  writeText(root / "old-only.txt", "old");
  writeText(root / "play/play7.luaskin",
            "return { type = 0, generation = 'old' }");
}

void writeNewTree(const fs::path &root) {
  writeText(root / "new-only.txt", "new");
  writeText(root / "play/play7.luaskin",
            "return { type = 0, generation = 'new' }");
}

void writeUnreferencedTree(const fs::path &root) {
  writeText(root / "unreferenced.txt", "keep");
}

bool treeIsOld(const fs::path &root) {
  return readText(root / "old-only.txt") == "old" &&
         readText(root / "play/play7.luaskin") ==
             "return { type = 0, generation = 'old' }" &&
         !fs::exists(root / "new-only.txt");
}

bool treeIsNew(const fs::path &root) {
  return readText(root / "new-only.txt") == "new" &&
         readText(root / "play/play7.luaskin") ==
             "return { type = 0, generation = 'new' }" &&
         !fs::exists(root / "old-only.txt");
}

void freezeRevisionTree(const fs::path &root) {
  constexpr fs::perms writes =
      fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
  std::error_code error;
  for (fs::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; ++iterator) {
    fs::permissions(iterator->path(), writes, fs::perm_options::remove, error);
  }
  expect(!error, "revision fixture descendants become immutable");
  error.clear();
  fs::permissions(root, writes, fs::perm_options::remove, error);
  expect(!error, "revision fixture root becomes immutable");
}

bool snapshotIsExactly(const SkinPackageCatalogSnapshot &snapshot,
                       std::uint64_t generation,
                       std::string_view revisionDigest) {
  return snapshot.catalogGeneration == generation &&
         snapshot.sourceGeneration == generation &&
         snapshot.packages.size() == 1 &&
         snapshot.packages.front().directoryName == "FixtureSkin" &&
         snapshot.packages.front().collisionKey == "fixtureskin" &&
         snapshot.entries.size() == 1 &&
         snapshot.entries.front().entry.packageRelativePath ==
             "play/play7.luaskin" &&
         snapshot.entries.front().revisionDigest == revisionDigest;
}

void testRecoveryFixtureDigestsBindPhysicalBytes() {
  expect(file_checksum::sha256(catalogDocument(7, kOldTreeDigest)) ==
             kOldCatalogDigest,
         "old catalog fixture digest binds its exact bytes");
  expect(file_checksum::sha256(catalogDocument(8, kNewTreeDigest)) ==
             kNewCatalogDigest,
         "new catalog fixture digest binds its exact bytes");

  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  expect(package.package.has_value(), "recovery fixture package ID is valid");
  if (!package.package) {
    return;
  }
  const fs::path oldSource = temp.root() / "digest-old";
  const fs::path newSource = temp.root() / "digest-new";
  const fs::path unreferencedSource = temp.root() / "digest-unreferenced";
  writeOldTree(oldSource);
  writeNewTree(newSource);
  writeUnreferencedTree(unreferencedSource);
  NoAliases aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto oldSnapshot = snapshotter.snapshot(oldSource, *package.package, {}, {});
  auto newSnapshot = snapshotter.snapshot(newSource, *package.package, {}, {});
  auto unreferencedSnapshot =
      snapshotter.snapshot(unreferencedSource, *package.package, {}, {});
  expect(oldSnapshot.prepared.has_value() &&
             oldSnapshot.prepared->revision().lowercaseSha256 == kOldTreeDigest,
         "old tree fixture digest binds its exact files");
  expect(newSnapshot.prepared.has_value() &&
             newSnapshot.prepared->revision().lowercaseSha256 == kNewTreeDigest,
         "new tree fixture digest binds its exact files");
  expect(unreferencedSnapshot.prepared.has_value() &&
             unreferencedSnapshot.prepared->revision().lowercaseSha256 ==
                 kUnreferencedTreeDigest,
         "unreferenced tree fixture digest binds its exact files");
}

void testFakeProviderFenceReleasesExactlyOnce() {
  FakeProfileSnapshots provider;
  {
    auto fence = provider.issueFence();
    expect(fence.has_value(), "fake provider can issue a real commit fence");
    auto moved = std::move(*fence);
    (void)moved;
  }
  expect(provider.releaseCount == 1,
         "a moved fake-provider commit fence releases exactly once");
}

void testCatalogJournalReplayAtEveryDurabilityBoundary() {
  enum class RequiredGeneration { Old, New };
  struct Boundary {
    std::string_view phase;
    bool oldVisible;
    bool oldBackup;
    bool newVisible;
    bool newVisibleStaging;
    bool newRevision;
    bool newRevisionStaging;
    bool newCatalog;
    bool corruptNewRevision;
    RequiredGeneration required;
  };
  constexpr Boundary boundaries[] = {
      // A rename without its parent fsync is modeled as lost after restart.
      {"intent-written", true, false, false, true, false, true, false, false,
       RequiredGeneration::Old},
      {"intent-parent-synced", true, false, false, true, false, true, false,
       false, RequiredGeneration::Old},
      {"visible-backup-renamed", true, false, false, true, false, true, false,
       false, RequiredGeneration::Old},
      {"visible-backup-parent-synced", false, true, false, true, false, true,
       false, false, RequiredGeneration::Old},
      {"visible-published", false, true, false, true, false, true, false, false,
       RequiredGeneration::Old},
      {"visible-parent-synced", false, true, true, false, false, true, false,
       false, RequiredGeneration::Old},
      {"revision-published", false, true, true, false, false, true, false,
       false, RequiredGeneration::Old},
      {"revision-parent-synced", false, true, true, false, true, false, false,
       false, RequiredGeneration::New},
      {"catalog-published", false, true, true, false, true, false, false, false,
       RequiredGeneration::New},
      {"catalog-parent-synced", false, true, true, false, true, false, true,
       false, RequiredGeneration::New},
      // Identical advisory phases with different physical revision states
      // prove replay is driven by verified trees rather than phase alone.
      {"revision-published", false, true, true, false, true, false, false,
       false, RequiredGeneration::New},
      {"revision-parent-synced", false, true, true, false, true, false, false,
       true, RequiredGeneration::Old},
  };

  for (const Boundary &boundary : boundaries) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const fs::path visible = roots.visiblePackages / "FixtureSkin";
    const fs::path visibleStaging = roots.visiblePackages.parent_path() /
                                    ".skin-import-staging/import-op-17";
    const fs::path visibleBackup =
        roots.visiblePackages.parent_path() /
        ".skin-publication-backups/op-17/FixtureSkin";
    const fs::path oldRevision =
        roots.privateRevisions / std::string(kOldTreeDigest);
    const fs::path newRevision =
        roots.privateRevisions / std::string(kNewTreeDigest);
    const fs::path unreferencedRevision =
        roots.privateRevisions / std::string(kUnreferencedTreeDigest);
    const fs::path newRevisionStaging =
        roots.privateRevisions / ".staging/revision-op-17";
    const fs::path catalogFile = roots.privateCatalog / "catalog.json";
    const fs::path catalogStaging =
        roots.privateCatalog / ".publication-staging/op-17-new.json";
    const fs::path catalogBackup =
        roots.privateCatalog / ".publication-backups/op-17-old.json";
    const fs::path journalFile =
        roots.privateCatalog / "publication-journal.json";

    if (boundary.oldVisible) {
      writeOldTree(visible);
    }
    if (boundary.oldBackup) {
      writeOldTree(visibleBackup);
    }
    if (boundary.newVisible) {
      writeNewTree(visible);
    }
    if (boundary.newVisibleStaging) {
      writeNewTree(visibleStaging);
    }
    writeOldTree(oldRevision);
    freezeRevisionTree(oldRevision);
    writeUnreferencedTree(unreferencedRevision);
    freezeRevisionTree(unreferencedRevision);
    if (boundary.newRevision) {
      writeNewTree(newRevision);
      if (boundary.corruptNewRevision) {
        writeText(newRevision / "play/play7.luaskin", "corrupt");
      }
      freezeRevisionTree(newRevision);
    }
    if (boundary.newRevisionStaging) {
      writeNewTree(newRevisionStaging);
      freezeRevisionTree(newRevisionStaging);
    }
    writeText(catalogFile, boundary.newCatalog
                               ? catalogDocument(8, kNewTreeDigest)
                               : catalogDocument(7, kOldTreeDigest));
    if (!boundary.newCatalog) {
      writeText(catalogStaging, catalogDocument(8, kNewTreeDigest));
    } else {
      writeText(catalogBackup, catalogDocument(7, kOldTreeDigest));
    }
    const std::string journal = journalDocument(boundary.phase);
    expect(journal.find(temp.root().string()) == std::string::npos,
           "publication journal owns typed identities, never host paths");
    writeText(journalFile, journal);

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    const auto recovery = store.recoverBeforeServiceStart();
    if (recovery.disposition != SkinRecoveryDisposition::Recovered) {
      for (const auto &diagnostic : recovery.diagnostics) {
        std::cerr << "  recovery diagnostic " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
      }
    }
    expect(recovery.disposition == SkinRecoveryDisposition::Recovered,
           "the exclusive pre-service recovery call completes");
    const auto recovered = catalog.snapshot();
    const bool expectNew = boundary.required == RequiredGeneration::New;
    expect(expectNew ? snapshotIsExactly(*recovered, 8, kNewTreeDigest)
                     : snapshotIsExactly(*recovered, 7, kOldTreeDigest),
           std::string("journal recovery selects the physically complete ") +
               (expectNew ? "new" : "old") + " generation after " +
               std::string(boundary.phase));
    expect(expectNew ? treeIsNew(visible) : treeIsOld(visible),
           "recovery repairs the visible tree to the selected generation");
    expect(expectNew ? treeIsNew(newRevision) : treeIsOld(oldRevision),
           "recovery retains a digest-matched revision for the selection");
    if (boundary.corruptNewRevision) {
      expect(!fs::exists(newRevision),
             "recovery removes or quarantines the journal-owned corrupt final "
             "revision");
    }
    expect(fs::exists(unreferencedRevision) &&
               readText(unreferencedRevision / "unreferenced.txt") == "keep",
           "recovery preserves a valid revision not owned by the journal");
    expect(readText(catalogFile) == (expectNew
                                         ? catalogDocument(8, kNewTreeDigest)
                                         : catalogDocument(7, kOldTreeDigest)),
           "recovery repairs catalog metadata to the selected generation");
    expect(!fs::exists(visibleStaging) && !fs::exists(visibleBackup) &&
               !fs::exists(newRevisionStaging) && !fs::exists(catalogStaging) &&
               !fs::exists(catalogBackup) && !fs::exists(journalFile),
           "recovery cleans journal-owned staging and backup capabilities");
  }
}

void testCreateJournalRestoresAbsenceOrCompleteNewPackage() {
  struct Boundary {
    bool visibleNew;
    bool stagedNew;
    bool revisionNew;
    bool catalogNew;
    bool expectNew;
  };
  constexpr Boundary boundaries[] = {
      {false, true, false, false, false},
      {true, false, false, false, false},
      {true, false, true, false, true},
      {true, false, true, true, true},
  };
  const std::string oldCatalog = emptyCatalogDocument();
  const std::string oldCatalogDigest = file_checksum::sha256(oldCatalog);
  for (const Boundary &boundary : boundaries) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const fs::path visible = roots.visiblePackages / "FixtureSkin";
    const fs::path visibleStaging = roots.visiblePackages.parent_path() /
                                    ".skin-import-staging/import-create-17";
    const fs::path newRevision =
        roots.privateRevisions / std::string(kNewTreeDigest);
    const fs::path catalogFile = roots.privateCatalog / "catalog.json";
    const fs::path catalogStaging =
        roots.privateCatalog / ".publication-staging/create-17-new.json";
    const fs::path catalogBackup =
        roots.privateCatalog / ".publication-backups/create-17-old.json";
    if (boundary.visibleNew) {
      writeNewTree(visible);
    }
    if (boundary.stagedNew) {
      writeNewTree(visibleStaging);
    }
    if (boundary.revisionNew) {
      writeNewTree(newRevision);
      freezeRevisionTree(newRevision);
    }
    writeText(catalogFile, boundary.catalogNew
                               ? catalogDocument(8, kNewTreeDigest)
                               : oldCatalog);
    if (boundary.catalogNew) {
      writeText(catalogBackup, oldCatalog);
    } else {
      writeText(catalogStaging, catalogDocument(8, kNewTreeDigest));
    }
    writeText(roots.privateCatalog / "publication-journal.json",
              createJournalDocument("fixture", oldCatalogDigest));

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    const auto recovered = store.recoverBeforeServiceStart();
    expect(recovered.disposition == SkinRecoveryDisposition::Recovered,
           "create recovery completes from a physical generation");
    expect(boundary.expectNew ? treeIsNew(visible) : !fs::exists(visible),
           "create recovery selects complete new or restores old absence");
    expect(boundary.expectNew
               ? snapshotIsExactly(*catalog.snapshot(), 8, kNewTreeDigest)
               : catalog.snapshot()->packages.empty(),
           "create recovery restores the matching catalog generation");
  }
}

void testCorruptNewCatalogFallsBackToCompleteOldGeneration() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const fs::path visible = roots.visiblePackages / "FixtureSkin";
  const fs::path backup = roots.visiblePackages.parent_path() /
                          ".skin-publication-backups/op-17/FixtureSkin";
  const fs::path oldRevision =
      roots.privateRevisions / std::string(kOldTreeDigest);
  const fs::path newRevision =
      roots.privateRevisions / std::string(kNewTreeDigest);
  writeNewTree(visible);
  writeOldTree(backup);
  writeOldTree(oldRevision);
  writeNewTree(newRevision);
  freezeRevisionTree(oldRevision);
  freezeRevisionTree(newRevision);
  writeText(roots.privateCatalog / "catalog.json",
            catalogDocument(7, kOldTreeDigest));
  writeText(roots.privateCatalog / ".publication-staging/op-17-new.json",
            "corrupt");
  writeText(roots.privateCatalog / "publication-journal.json",
            journalDocument("revision-parent-synced"));

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  const auto recovered = store.recoverBeforeServiceStart();
  expect(recovered.disposition == SkinRecoveryDisposition::Recovered,
         "corrupt new catalog falls back to the complete old generation");
  expect(treeIsOld(visible),
         "corrupt new catalog rollback restores the old visible tree");
  expect(snapshotIsExactly(*catalog.snapshot(), 7, kOldTreeDigest),
         "corrupt new catalog rollback retains old catalog identity");
}

void testRecoveryRejectsRepeatedBootstrapOwnership() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);

  const auto first = store.recoverBeforeServiceStart();
  const fs::path lateJournal =
      roots.privateCatalog / "publication-journal.json";
  writeText(lateJournal, "must-not-be-replayed");
  const auto repeated = store.recoverBeforeServiceStart();
  expect(first.disposition == SkinRecoveryDisposition::Recovered,
         "the first bootstrap caller exclusively owns recovery");
  expect(repeated.disposition == SkinRecoveryDisposition::AlreadyRecovered,
         "a repeated bootstrap call is reported without replaying recovery");
  expect(readText(lateJournal) == "must-not-be-replayed",
         "a repeated bootstrap call performs no filesystem replay");
}

void testRecoveryRejectsOverlappingBootstrapOwnership() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const fs::path visible = roots.visiblePackages / "FixtureSkin";
  const fs::path visibleStaging =
      roots.visiblePackages.parent_path() / ".skin-import-staging/import-op-17";
  const fs::path oldRevision =
      roots.privateRevisions / std::string(kOldTreeDigest);
  const fs::path newRevisionStaging =
      roots.privateRevisions / ".staging/revision-op-17";
  const fs::path catalogFile = roots.privateCatalog / "catalog.json";
  const fs::path catalogStaging =
      roots.privateCatalog / ".publication-staging/op-17-new.json";
  const fs::path journalFile =
      roots.privateCatalog / "publication-journal.json";
  writeOldTree(visible);
  writeNewTree(visibleStaging);
  writeOldTree(oldRevision);
  freezeRevisionTree(oldRevision);
  writeNewTree(newRevisionStaging);
  writeText(catalogFile, catalogDocument(7, kOldTreeDigest));
  writeText(catalogStaging, catalogDocument(8, kNewTreeDigest));
  const std::string journal = journalDocument("intent-written");
  writeText(journalFile, journal);

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  BlockingAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  auto owner = std::async(std::launch::async, [&store] {
    return store.recoverBeforeServiceStart();
  });
  const bool ownerReachedPhysicalInspection =
      aliases.waitUntilEntered(std::chrono::seconds(1));
  auto overlapping = std::async(std::launch::async, [&store] {
    return store.recoverBeforeServiceStart();
  });
  const bool overlapReturnedWhileOwnerBlocked =
      overlapping.wait_for(std::chrono::milliseconds(250)) ==
      std::future_status::ready;
  const bool journalUnchangedWhileOwnerBlocked =
      readText(journalFile) == journal;
  aliases.release();
  const auto ownerResult = owner.get();
  const auto overlappingResult = overlapping.get();

  expect(ownerReachedPhysicalInspection,
         "the first recovery call remains in physical verification");
  expect(overlapReturnedWhileOwnerBlocked,
         "an overlapping recovery call is rejected without waiting");
  expect(ownerResult.disposition == SkinRecoveryDisposition::Recovered,
         "the exclusive recovery owner completes after inspection resumes");
  expect(overlappingResult.disposition ==
             SkinRecoveryDisposition::ConcurrentCallRejected,
         "an overlapping caller receives the typed rejection");
  expect(journalUnchangedWhileOwnerBlocked,
         "an overlapping caller performs no filesystem replay");
}

void testMalformedAndOversizedCatalogFailBootstrap() {
  for (const bool oversized : {false, true}) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    writeText(roots.privateCatalog / "catalog.json",
              oversized ? std::string(33 * 1024 * 1024, 'x') : "{");
    writeText(roots.privateCatalog / "catalog.json.bak",
              emptyCatalogDocument());
    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    const auto recovered = store.recoverBeforeServiceStart();
    expect(recovered.disposition == SkinRecoveryDisposition::Failed,
           oversized ? "oversized catalog fails closed during bootstrap"
                     : "malformed catalog fails closed during bootstrap");
    expect(catalog.snapshot()->catalogGeneration == 0 &&
               catalog.snapshot()->packages.empty(),
           "failed catalog bootstrap never exposes backup/default as durable "
           "metadata");
  }
}

void testRemovalJournalRecoversOldAndNewGenerations() {
  const std::string oldCatalog = catalogDocument(7, kOldTreeDigest);
  const std::string newCatalog = emptyCatalogDocument(8);
  const std::string oldDigest = file_checksum::sha256(oldCatalog);
  const std::string newDigest = file_checksum::sha256(newCatalog);
  for (const bool committedNew : {false, true}) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const fs::path retained = roots.visiblePackages.parent_path() /
                              ".skin-removal-staging/remove-17/FixtureSkin";
    writeOldTree(retained);
    const fs::path oldRevision =
        roots.privateRevisions / std::string(kOldTreeDigest);
    writeOldTree(oldRevision);
    freezeRevisionTree(oldRevision);
    writeText(roots.privateCatalog / "catalog.json",
              committedNew ? newCatalog : oldCatalog);
    writeText(roots.privateCatalog / ".removal-staging/remove-17-new.json",
              newCatalog);
    writeText(roots.privateCatalog / ".removal-backups/remove-17-old.json",
              oldCatalog);
    writeText(roots.privateCatalog / "removal-journal.json",
              removalJournalDocument(oldDigest, newDigest));

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    const auto recovered = store.recoverBeforeServiceStart();
    expect(recovered.disposition == SkinRecoveryDisposition::Recovered,
           "removal journal selects a complete physical generation");
    expect(committedNew ? !fs::exists(roots.visiblePackages / "FixtureSkin")
                        : treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "removal recovery restores old visible or committed absence");
    expect(committedNew
               ? catalog.snapshot()->packages.empty()
               : snapshotIsExactly(*catalog.snapshot(), 7, kOldTreeDigest),
           "removal recovery installs the matching exact catalog bytes");
    expect(!fs::exists(roots.privateCatalog / "removal-journal.json"),
           "successful removal recovery consumes its journal exactly once");
  }
}

void testRemovalRecoveryRetainsIntentWhenOwnedCleanupCannotFinish() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const std::string oldCatalog = catalogDocument(7, kOldTreeDigest);
  const std::string newCatalog = emptyCatalogDocument(8);
  const std::string oldDigest = file_checksum::sha256(oldCatalog);
  const std::string newDigest = file_checksum::sha256(newCatalog);
  writeOldTree(roots.visiblePackages / "FixtureSkin");
  const fs::path oldRevision =
      roots.privateRevisions / std::string(kOldTreeDigest);
  writeOldTree(oldRevision);
  freezeRevisionTree(oldRevision);
  writeText(roots.privateCatalog / "catalog.json", oldCatalog);
  writeText(roots.privateCatalog / ".removal-staging/remove-17-new.json",
            newCatalog);
  const fs::path cleanupTarget = temp.root() / "outside-removal-cleanup";
  fs::create_directories(cleanupTarget);
  writeText(cleanupTarget / "sentinel.txt",
            "must remain outside transaction ownership");
  std::error_code linkError;
  fs::create_directory_symlink(
      cleanupTarget, roots.privateCatalog / ".recovery-quarantine", linkError);
  if (linkError) {
    return;
  }
  writeText(roots.privateCatalog / "removal-journal.json",
            removalJournalDocument(oldDigest, newDigest));

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  const auto recovered = store.recoverBeforeServiceStart();
  expect(recovered.disposition == SkinRecoveryDisposition::Failed,
         "removal recovery fails closed when an owned cleanup path is a link");
  expect(fs::exists(roots.privateCatalog / "removal-journal.json"),
         "failed removal cleanup retains the durable intent for restart");
  expect(readText(cleanupTarget / "sentinel.txt") ==
             "must remain outside transaction ownership",
         "removal cleanup never follows an unowned link target");
}

void testStableRevisionRestoresCorruptedTransactionCopies() {
  const std::string oldCatalog = catalogDocument(7, kOldTreeDigest);
  const std::string newCatalog = emptyCatalogDocument(8);
  {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const fs::path stableOld =
        roots.privateRevisions / std::string(kOldTreeDigest);
    writeOldTree(stableOld);
    freezeRevisionTree(stableOld);
    writeNewTree(roots.visiblePackages.parent_path() /
                 ".skin-publication-backups/op-17/FixtureSkin");
    writeText(roots.privateCatalog / "catalog.json", oldCatalog);
    writeText(roots.privateCatalog / ".publication-staging/op-17-new.json",
              newCatalog);
    writeText(roots.privateCatalog / ".publication-backups/op-17-old.json",
              oldCatalog);
    writeText(roots.privateCatalog / "publication-journal.json",
              journalDocument("visible-backup-parent-synced"));

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    expect(store.recoverBeforeServiceStart().disposition ==
                   SkinRecoveryDisposition::Recovered &&
               treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "publication recovery rematerializes immutable old bytes when the "
           "renamed backup was corrupted through an open descendant");
  }
  {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const fs::path stableOld =
        roots.privateRevisions / std::string(kOldTreeDigest);
    writeOldTree(stableOld);
    freezeRevisionTree(stableOld);
    writeNewTree(roots.visiblePackages.parent_path() /
                 ".skin-removal-staging/remove-17/FixtureSkin");
    writeText(roots.privateCatalog / "catalog.json", oldCatalog);
    writeText(roots.privateCatalog / ".removal-staging/remove-17-new.json",
              newCatalog);
    writeText(roots.privateCatalog / ".removal-backups/remove-17-old.json",
              oldCatalog);
    writeText(roots.privateCatalog / "removal-journal.json",
              removalJournalDocument(file_checksum::sha256(oldCatalog),
                                     file_checksum::sha256(newCatalog)));

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    expect(store.recoverBeforeServiceStart().disposition ==
                   SkinRecoveryDisposition::Recovered &&
               treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "removal recovery rematerializes immutable old bytes when retained "
           "staging was corrupted through an open descendant");
  }
}

void testIntentOnlyRecoveryNeedsNoUnjournaledCatalogArtifacts() {
  const std::string oldCatalog = catalogDocument(7, kOldTreeDigest);
  for (const bool removal : {false, true}) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    writeOldTree(roots.visiblePackages / "FixtureSkin");
    const fs::path stableOld =
        roots.privateRevisions / std::string(kOldTreeDigest);
    writeOldTree(stableOld);
    freezeRevisionTree(stableOld);
    writeText(roots.privateCatalog / "catalog.json", oldCatalog);
    if (removal) {
      const std::string newCatalog = emptyCatalogDocument(8);
      writeText(roots.privateCatalog / "removal-journal.json",
                removalJournalDocument(file_checksum::sha256(oldCatalog),
                                       file_checksum::sha256(newCatalog)));
    } else {
      writeText(roots.privateCatalog / "publication-journal.json",
                journalDocument("intent-written"));
    }

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    expect(store.recoverBeforeServiceStart().disposition ==
                   SkinRecoveryDisposition::Recovered &&
               treeIsOld(roots.visiblePackages / "FixtureSkin") &&
               snapshotIsExactly(*catalog.snapshot(), 7, kOldTreeDigest),
           removal
               ? "removal intent recovers before any catalog artifact exists"
               : "publication intent recovers before any catalog artifact "
                 "exists");
  }
}

void testOperationNamesDoNotReusePreseededPriorProcessArtifacts() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  if (!package.package) {
    expect(false, "operation-identity fixture package ID is valid");
    return;
  }
  const fs::path source = temp.root() / "operation-identity-source";
  writeNewTree(source);
  const fs::path priorBackup = roots.visiblePackages.parent_path() /
                               ".skin-publication-backups/publish-1";
  writeText(priorBackup / "sentinel.txt", "prior-process-backup");
  const fs::path priorCatalogStaging =
      roots.privateCatalog / ".publication-staging/publish-1-new.json";
  const fs::path priorCatalogBackup =
      roots.privateCatalog / ".publication-backups/publish-1-old.json";
  writeText(priorCatalogStaging, "prior-process-staging");
  writeText(priorCatalogBackup, "prior-process-catalog-backup");

  NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases);
  auto prepared = importer.prepareFolder(source, *package.package, {}, {});
  expect(prepared.prepared.has_value(),
         "operation-identity candidate prepares");
  if (!prepared.prepared) {
    return;
  }
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  SelectableValidator validator;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "operation-identity fixture bootstraps storage");
  const auto published = store.publish(
      std::move(*prepared.prepared), PackageCollisionPolicy::Reject,
      ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
  expect(published.published,
         "a new lifetime publishes without colliding with prior artifacts");
  expect(readText(priorBackup / "sentinel.txt") == "prior-process-backup" &&
             readText(priorCatalogStaging) == "prior-process-staging" &&
             readText(priorCatalogBackup) == "prior-process-catalog-backup",
         "new operations preserve every preseeded prior-process artifact");
  std::error_code reservationError;
  std::size_t reservations = 0;
  for (fs::directory_iterator entry(
           roots.privateCatalog / ".operation-reservations", reservationError),
       end;
       !reservationError && entry != end; ++entry) {
    ++reservations;
  }
  expect(!reservationError && reservations == 0,
         "successful publication releases its exclusive operation reservation");
}

void testBootstrapCleansOnlyBoundedRecognizableOwnedArtifacts() {
#if !defined(_WIN32)
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const std::string ownedOperation = "op-" + std::string(32, 'a');
  const std::string ownedQuarantine = "q-" + std::string(32, 'b');
  const fs::path reservation =
      roots.privateCatalog / ".operation-reservations" / ownedOperation;
  const fs::path quarantine =
      roots.privateCatalog / ".recovery-quarantine" / ownedQuarantine;
  const fs::path catalogStaging = roots.privateCatalog /
                                  ".publication-staging" /
                                  (ownedOperation + "-new.json");
  writeText(reservation / "owned.txt", "owned");
  writeText(quarantine / "owned.txt", "owned");
  writeText(catalogStaging, "owned regular staging");

  const fs::path unknown =
      roots.privateCatalog / ".operation-reservations/user-preserved";
  writeText(unknown / "keep.txt", "unknown");
  const fs::path outside = temp.root() / "outside-owned-cleanup";
  writeText(outside / "keep.txt", "outside");
  const fs::path linkedOwned = roots.privateCatalog /
                               ".operation-reservations" /
                               ("op-" + std::string(32, 'c'));
  std::error_code linkError;
  fs::create_directory_symlink(outside, linkedOwned, linkError);

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "owned-artifact cleanup bootstraps an empty catalog");
  expect(!fs::exists(reservation) && !fs::exists(quarantine) &&
             !fs::exists(catalogStaging),
         "bootstrap retries recognizable owner-created file and directory "
         "artifacts");
  expect(
      fs::exists(unknown / "keep.txt") && fs::is_symlink(linkedOwned) &&
          readText(outside / "keep.txt") == "outside",
      "bootstrap preserves unknown entries and recognizable reparse entries");
#endif
}

void testInventoryFenceAndDescendantEditReturnPreparedWithoutMutation() {
  for (const bool editDescendant : {false, true}) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const auto package = normalizePackageId("FixtureSkin");
    if (!package.package) {
      expect(false, "inventory-race fixture package ID is valid");
      return;
    }
    writeOldTree(roots.visiblePackages / "FixtureSkin");
    const fs::path source = temp.root() / "inventory-candidate";
    writeNewTree(source);
    NoAliases aliases;
    SkinArchiveImporter importer(roots, aliases);
    auto prepared = importer.prepareFolder(source, *package.package, {}, {});
    expect(prepared.prepared.has_value(), "inventory-race candidate prepares");
    if (!prepared.prepared) {
      continue;
    }
    const std::string revision =
        prepared.prepared->candidateRevision().lowercaseSha256;
    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    if (editDescendant) {
      profiles.beforeFence = [&] {
        writeText(roots.visiblePackages / "FixtureSkin/old-only.txt",
                  "edited immediately before commit");
      };
    } else {
      profiles.rejectFence = true;
    }
    SelectableValidator validator;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    auto published = store.publish(
        std::move(*prepared.prepared), PackageCollisionPolicy::Replace,
        ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
    expect(published.retryableInventoryRace &&
               published.retryPrepared.has_value() && !published.published,
           editDescendant
               ? "descendant edit at the fence returns the exact prepared "
                 "candidate"
               : "stale inventory fence returns the exact prepared candidate");
    expect(catalog.snapshot()->catalogGeneration == 0 &&
               !fs::exists(roots.privateCatalog / "publication-journal.json") &&
               !fs::exists(roots.privateRevisions / revision),
           "inventory race performs zero journal/catalog/revision mutation");
  }
}

void testNormalizedPhysicalCollisionsRejectOrReplaceAsOnePackage() {
  struct AliasPair {
    std::string existing;
    std::string candidate;
  };
  const std::vector<AliasPair> pairs = {
      {"FixtureSkin", "fixtureskin"},
      {"Straße", "STRASSE"},
      {"Caf\xC3\xA9", "Cafe\xCC\x81"},
  };
  for (const AliasPair &pair : pairs) {
    const auto existingId = normalizePackageId(pair.existing);
    const auto candidateId = normalizePackageId(pair.candidate);
    expect(existingId.package && candidateId.package &&
               existingId.package->collisionKey ==
                   candidateId.package->collisionKey,
           "collision fixture aliases normalize to one package identity");
    if (!existingId.package || !candidateId.package ||
        existingId.package->collisionKey != candidateId.package->collisionKey) {
      continue;
    }
    for (const PackageCollisionPolicy policy :
         {PackageCollisionPolicy::Reject, PackageCollisionPolicy::Replace}) {
      TempDirectory temp;
      const SkinStorageRoots roots = rootsBelow(temp.root());
      writeOldTree(roots.visiblePackages / pair.existing);
      const fs::path source = temp.root() / "collision-candidate";
      writeNewTree(source);
      NoAliases aliases;
      SkinArchiveImporter importer(roots, aliases);
      auto prepared =
          importer.prepareFolder(source, *candidateId.package, {}, {});
      SkinPackageCatalog catalog(roots.privateCatalog);
      FakeProfileSnapshots profiles;
      SelectableValidator validator;
      SkinPackageStore store(roots, catalog, aliases, profiles);
      expect(store.recoverBeforeServiceStart().disposition ==
                 SkinRecoveryDisposition::Recovered,
             "collision fixture bootstraps store");
      auto result =
          store.publish(std::move(*prepared.prepared), policy,
                        ProfileInventorySnapshot{.inventoryGeneration = 1},
                        validator, {}, {});
      std::size_t matchingChildren = 0;
      std::error_code error;
      for (fs::directory_iterator child(roots.visiblePackages, error), end;
           !error && child != end; ++child) {
        const auto name = child->path().filename().u8string();
        const std::string utf8(reinterpret_cast<const char *>(name.data()),
                               name.size());
        const auto normalized = normalizePackageId(utf8);
        if (normalized.package && normalized.package->collisionKey ==
                                      candidateId.package->collisionKey) {
          ++matchingChildren;
        }
      }
      if (policy == PackageCollisionPolicy::Reject) {
        expect(!result.published && matchingChildren == 1 &&
                   treeIsOld(roots.visiblePackages / pair.existing),
               "Reject uses normalized collision identity without adding an "
               "alias root");
      } else {
        expect(result.published && matchingChildren == 1,
               "Replace atomically leaves one normalized physical package");
      }
    }
  }
}

void testAmbiguousPhysicalCollisionCannotPersistDuplicateCatalogIdentity() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const fs::path first = roots.visiblePackages / "FixtureSkin";
  const fs::path second = roots.visiblePackages / "fixtureskin";
  writeOldTree(first);
  writeNewTree(second);
  std::error_code equivalentError;
  if (fs::equivalent(first, second, equivalentError) && !equivalentError) {
    return;
  }
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SelectableValidator validator;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "ambiguous collision fixture bootstraps store");
  const auto scan = store.rescanVisibleSources(
      {}, {}, ProfileInventorySnapshot{.inventoryGeneration = 1}, validator);
  expect(!scan.diagnostics.empty() && catalog.snapshot()->packages.empty() &&
             catalog.snapshot()->entries.empty(),
         "ambiguous normalized direct children fail closed without duplicate "
         "catalog identities");
  catalog.flush();
  SkinPackageCatalog restartedCatalog(roots.privateCatalog);
  FakeProfileSnapshots restartedProfiles;
  SkinPackageStore restarted(roots, restartedCatalog, aliases,
                             restartedProfiles);
  expect(restarted.recoverBeforeServiceStart().disposition ==
                 SkinRecoveryDisposition::Recovered &&
             restartedCatalog.snapshot()->packages.empty(),
         "restart never reloads a duplicate normalized catalog identity");
}

void testEncoderInvalidSnapshotPerformsZeroPublicationMutation() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  if (!package.package) {
    expect(false, "encoder validation fixture package ID is valid");
    return;
  }
  const fs::path source = temp.root() / "encoder-invalid";
  writeNewTree(source);
  NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases);
  auto prepared = importer.prepareFolder(source, *package.package, {}, {});
  const std::string revision =
      prepared.prepared->candidateRevision().lowercaseSha256;
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  SelectableValidator validator;
  validator.metadataDeclarations = 257;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "encoder validation fixture bootstraps store");
  const auto result = store.publish(
      std::move(*prepared.prepared), PackageCollisionPolicy::Reject,
      ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
  expect(!result.published && catalog.snapshot()->catalogGeneration == 0 &&
             !fs::exists(roots.visiblePackages / "FixtureSkin") &&
             !fs::exists(roots.privateRevisions / revision) &&
             !fs::exists(roots.privateCatalog / "publication-journal.json"),
         "decoder-invalid metadata is rejected before any publication "
         "mutation");
}

void testConfiguredValidationFailureAndCancellationPreserveOldPackage() {
  for (const bool cancelConfigured : {false, true}) {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const auto package = normalizePackageId("FixtureSkin");
    if (!package.package) {
      expect(false, "configured-validation fixture package ID is valid");
      return;
    }
    const fs::path oldSource = temp.root() / "configured-old";
    writeOldTree(oldSource);
    NoAliases aliases;
    SkinArchiveImporter importer(roots, aliases);
    auto oldPrepared =
        importer.prepareFolder(oldSource, *package.package, {}, {});
    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    SelectableValidator validator;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    expect(store.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "configured-validation fixture bootstraps store roots");
    auto initial = store.publish(
        std::move(*oldPrepared.prepared), PackageCollisionPolicy::Reject,
        ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
    expect(initial.published && initial.entries.size() == 1,
           "configured-validation fixture publishes old package");
    if (!initial.published || initial.entries.empty()) {
      continue;
    }
    const auto before = store.catalogSnapshot();
    ProfileInventorySnapshot inventory{.inventoryGeneration = 2};
    VersionedSkinProfileSettings selected{
        .profileId =
            SkinProfileId{.opaque = "12345678-1234-1234-1234-123456789abc"},
        .generation = 9};
    selected.settings.selected7KeyEntry = initial.entries.front();
    selected.settings.entries.emplace(initial.entries.front(),
                                      EntryProfileSettings{});
    inventory.profiles.push_back(std::move(selected));

    const fs::path replacementSource = temp.root() / "configured-new";
    writeNewTree(replacementSource);
    auto replacement =
        importer.prepareFolder(replacementSource, *package.package, {}, {});
    validator.rejectConfigured = !cancelConfigured;
    validator.cancelConfigured = cancelConfigured;
    auto rejected = store.publish(std::move(*replacement.prepared),
                                  PackageCollisionPolicy::Replace,
                                  std::move(inventory), validator, {}, {});
    expect(!rejected.published,
           cancelConfigured
               ? "selected-profile cancellation rejects replacement"
               : "selected-profile validation failure rejects replacement");
    expect(treeIsOld(roots.visiblePackages / "FixtureSkin") &&
               store.catalogSnapshot()->catalogGeneration ==
                   before->catalogGeneration &&
               store.catalogSnapshot()->entries.front().revisionDigest ==
                   before->entries.front().revisionDigest,
           "selected-profile rejection preserves the whole old tree/catalog");
  }
}

void testTransactionFailureRetainsJournalForRestartRecovery() {
  {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const auto package = normalizePackageId("FixtureSkin");
    if (!package.package) {
      expect(false, "publication rollback fixture package ID is valid");
      return;
    }
    const fs::path oldSource = temp.root() / "rollback-old";
    writeOldTree(oldSource);
    NoAliases aliases;
    SkinArchiveImporter importer(roots, aliases);
    auto oldPrepared =
        importer.prepareFolder(oldSource, *package.package, {}, {});
    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    SelectableValidator validator;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    expect(store.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "publication rollback fixture bootstraps store roots");
    auto initial = store.publish(
        std::move(*oldPrepared.prepared), PackageCollisionPolicy::Reject,
        ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
    expect(initial.published,
           "publication rollback fixture publishes old tree");

    const fs::path newSource = temp.root() / "rollback-new";
    writeNewTree(newSource);
    auto openOldWriter = std::make_shared<std::fstream>(
        roots.visiblePackages / "FixtureSkin/old-only.txt",
        std::ios::in | std::ios::out);
    expect(openOldWriter->is_open(),
           "publication rollback keeps an old descendant writer open");
    std::atomic_bool oldBackupMutated{false};
    auto observer = std::make_shared<OneShotThrowingObserver>([&] {
      openOldWriter->seekp(0);
      *openOldWriter << "corrupted-through-open-handle";
      openOldWriter->flush();
      oldBackupMutated.store(openOldWriter->good(), std::memory_order_release);
    });
    SkinArchiveImporter throwingImporter(roots, aliases, observer);
    auto replacement =
        throwingImporter.prepareFolder(newSource, *package.package, {}, {});
    catalog.shutdown();
    bool threw = false;
    try {
      (void)store.publish(std::move(*replacement.prepared),
                          PackageCollisionPolicy::Replace,
                          ProfileInventorySnapshot{.inventoryGeneration = 2},
                          validator, {}, {});
    } catch (const std::runtime_error &) {
      threw = true;
    }
    openOldWriter->close();
    expect(oldBackupMutated.load(std::memory_order_acquire),
           "open descendant writer corrupts the renamed publication backup");
    expect(
        threw && fs::exists(roots.privateCatalog / "publication-journal.json"),
        "throw after old visible move retains publication recovery evidence");
    expect(treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "best-effort publication rollback restores old visible identity");
    SkinPackageCatalog restartedCatalog(roots.privateCatalog);
    FakeProfileSnapshots restartedProfiles;
    SkinPackageStore restarted(roots, restartedCatalog, aliases,
                               restartedProfiles);
    expect(restarted.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "restart consumes retained publication evidence");
    expect(treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "publication restart recovery selects the complete old generation");
  }

  {
    TempDirectory temp;
    const SkinStorageRoots roots = rootsBelow(temp.root());
    const auto package = normalizePackageId("FixtureSkin");
    if (!package.package) {
      expect(false, "removal rollback fixture package ID is valid");
      return;
    }
    const fs::path oldSource = temp.root() / "remove-rollback-old";
    writeOldTree(oldSource);
    NoAliases aliases;
    SkinArchiveImporter importer(roots, aliases);
    auto oldPrepared =
        importer.prepareFolder(oldSource, *package.package, {}, {});
    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    SelectableValidator validator;
    std::shared_ptr<std::fstream> openOldWriter;
    std::atomic_bool retainedTreeMutated{false};
    auto removalObserver = std::make_shared<RemovalMutationObserver>([&] {
      if (!openOldWriter) {
        return;
      }
      openOldWriter->seekp(0);
      *openOldWriter << "corrupted-through-open-handle";
      openOldWriter->flush();
      retainedTreeMutated.store(openOldWriter->good(),
                                std::memory_order_release);
    });
    SkinPackageStore store(roots, catalog, aliases, profiles, removalObserver);
    expect(store.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "removal rollback fixture bootstraps store roots");
    expect(store
               .publish(std::move(*oldPrepared.prepared),
                        PackageCollisionPolicy::Reject,
                        ProfileInventorySnapshot{.inventoryGeneration = 1},
                        validator, {}, {})
               .published,
           "removal rollback fixture publishes old tree");
    openOldWriter = std::make_shared<std::fstream>(
        roots.visiblePackages / "FixtureSkin/old-only.txt",
        std::ios::in | std::ios::out);
    expect(openOldWriter->is_open(),
           "removal rollback keeps an old descendant writer open");
    catalog.shutdown();
    const auto removed = store.removePackage(*package.package, {});
    openOldWriter->close();
    expect(retainedTreeMutated.load(std::memory_order_acquire),
           "open descendant writer corrupts retained removal staging");
    expect(!removed.removed &&
               fs::exists(roots.privateCatalog / "removal-journal.json"),
           "failed removal rollback retains its journal and catalog copies");
    expect(treeIsOld(roots.visiblePackages / "FixtureSkin"),
           "best-effort removal rollback restores old visible identity");
    SkinPackageCatalog restartedCatalog(roots.privateCatalog);
    FakeProfileSnapshots restartedProfiles;
    SkinPackageStore restarted(roots, restartedCatalog, aliases,
                               restartedProfiles);
    expect(restarted.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "restart consumes retained removal rollback evidence");
  }
}

void testGarbageCollectionRejectsLinksAndRetriesQuarantine() {
#if !defined(_WIN32)
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  fs::create_directories(roots.privateRevisions);
  const fs::path outside = temp.root() / "outside";
  writeText(outside / "keep.txt", "outside");
  const fs::path linkedRoot = roots.privateRevisions / std::string(64, 'a');
  fs::create_directory_symlink(outside, linkedRoot);
  const fs::path nestedLink = roots.privateRevisions / std::string(64, 'b');
  writeText(nestedLink / "file.txt", "candidate");
  fs::create_symlink(outside / "keep.txt", nestedLink / "linked.txt");
  const fs::path stale =
      roots.privateRevisions / (".gc-quarantine/op-" + std::string(32, 'd'));
  writeText(stale / "old.txt", "retry");

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  const auto collected = store.collectGarbage();
  expect(collected.revisionsRemoved == 0 && fs::exists(linkedRoot) &&
             fs::exists(nestedLink) && fs::exists(outside / "keep.txt"),
         "GC rejects root and nested links without following or deleting them");
  expect(!fs::exists(stale),
         "GC retries and removes a prior safe quarantine directory");
#endif
}

void testGarbageCollectionNeverUnlinksAnExchangedQuarantinePath() {
#if !defined(_WIN32)
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const std::string digest(64, 'c');
  const fs::path candidate = roots.privateRevisions / digest;
  for (int index = 0; index < 2048; ++index) {
    writeText(candidate / ("entry-" + std::to_string(index) + ".txt"),
              "collectible");
  }

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  NoAliases aliases;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  std::atomic_bool collectionFinished{false};
  std::atomic_bool exchanged{false};
  fs::path replacement;
  std::thread attacker([&] {
    const fs::path gcRoot = roots.privateRevisions / ".gc-quarantine";
    while (!collectionFinished.load(std::memory_order_acquire)) {
      std::error_code error;
      for (fs::directory_iterator operation(gcRoot, error), end;
           !error && operation != end; ++operation) {
        const fs::path target = operation->path() / digest;
        const fs::path held = operation->path() / "attacker-held-original";
        if (!fs::exists(target, error) || error) {
          error.clear();
          continue;
        }
        fs::rename(target, held, error);
        if (error) {
          error.clear();
          continue;
        }
        fs::create_directory(target, error);
        if (!error) {
          replacement = target;
          exchanged.store(true, std::memory_order_release);
          return;
        }
      }
      std::this_thread::yield();
    }
  });
  const auto collected = store.collectGarbage();
  collectionFinished.store(true, std::memory_order_release);
  attacker.join();

  expect(exchanged.load(std::memory_order_acquire),
         "GC exchange fixture replaces the quarantined pathname");
  if (exchanged.load(std::memory_order_acquire)) {
    expect(fs::exists(replacement),
           "GC keeps an attacker replacement at the retained pathname");
    expect(collected.revisionsRemoved == 0,
           "GC does not report an exchanged quarantine as deleted");
  }
#endif
}

void testManualInvalidEditRetainsActivationButDeleteHidesIt() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  if (!package.package) {
    expect(false, "manual-edit fixture package ID is valid");
    return;
  }
  const fs::path source = temp.root() / "manual-edit-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases);
  auto prepared = importer.prepareFolder(source, *package.package, {}, {});
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  SelectableValidator validator;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "manual-edit fixture bootstraps store roots");
  auto published = store.publish(
      std::move(*prepared.prepared), PackageCollisionPolicy::Reject,
      ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
  if (!published.published || published.entries.empty()) {
    expect(false, "manual-edit fixture publishes an entry");
    return;
  }
  const SkinEntryId entry = published.entries.front();
  const SkinProfileId profile{.opaque = "12345678-1234-1234-1234-123456789abc"};
  VersionedSkinProfileSettings base{.profileId = profile, .generation = 3};
  base.settings.selected7KeyEntry = entry;
  base.settings.entries.emplace(entry, EntryProfileSettings{});
  validator.configurationDigest = std::string(64, '2');
  auto activation =
      store.prepareActivation(base, entry, base.settings, validator, {});
  FakeProfileOwner owner(base);
  owner.persisted = true;
  const auto begun = store.beginPreparedActivationCommit(
      std::move(*activation.prepared), owner);
  const auto committed =
      store.pollPreparedActivationCommit(begun.ticket, owner);
  expect(committed.disposition ==
             ActivationCommitDisposition::ActivatedRequested,
         "manual-edit fixture activates a validated revision");

  writeText(roots.visiblePackages / "FixtureSkin/play/play7.luaskin",
            "return invalid manual edit");
  validator.disposition = SkinValidationDisposition::Invalid;
  const auto invalidScan = store.rescanVisibleSources(
      {}, {}, ProfileInventorySnapshot{.inventoryGeneration = 2}, validator);
  expect(
      !invalidScan.cancelled &&
          store.acquireValidatedActivation(profile, entry, std::string(64, '2'))
              .activation.has_value(),
      "invalid manual edit retains the exact last-known-good activation");
  expect(readText(roots.visiblePackages / "FixtureSkin/play/play7.luaskin") ==
             "return invalid manual edit",
         "invalid manual edit remains user-visible and diagnosed");

  writeText(roots.visiblePackages / "FixtureSkin/play/play7.luaskin",
            "return { type = 0, generation = 'valid-new' }");
  validator.disposition = SkinValidationDisposition::Selectable7Key;
  validator.configurationDigest = std::string(64, '3');
  const auto validScan = store.rescanVisibleSources(
      {}, {}, ProfileInventorySnapshot{.inventoryGeneration = 3}, validator);
  expect(!validScan.cancelled && !store
                                      .acquireValidatedActivation(
                                          profile, entry, std::string(64, '2'))
                                      .activation,
         "valid manual revision advance invalidates the stale activation");

  fs::remove_all(roots.visiblePackages / "FixtureSkin");
  (void)store.rescanVisibleSources(
      {}, {}, ProfileInventorySnapshot{.inventoryGeneration = 4}, validator);
  expect(!store.acquireValidatedActivation(profile, entry, std::string(64, '2'))
              .activation,
         "manual package deletion immediately hides activation from new "
         "sessions");
}

void testReplacementPublishesExactlyTheNewWholePackage() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  expect(package.package.has_value(), "synthetic package ID is valid");
  if (!package.package) {
    return;
  }

  writeText(roots.visiblePackages / "FixtureSkin/old-only.txt", "old");
  const fs::path source = temp.root() / "picked-folder";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  writeText(source / "new-only.txt", "new");

  NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases);
  auto prepared = importer.prepareFolder(source, *package.package, {}, {});
  expect(prepared.prepared.has_value(),
         "Task 6 prepares the replacement candidate");
  if (!prepared.prepared) {
    for (const auto &diagnostic : prepared.diagnostics) {
      std::cerr << "  prepare diagnostic " << diagnostic.code << ": "
                << diagnostic.message << '\n';
    }
    return;
  }

  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  SelectableValidator validator;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  auto result = store.publish(
      std::move(*prepared.prepared), PackageCollisionPolicy::Replace,
      ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});

  expect(result.published, "whole-package replacement publishes");
  expect(!fs::exists(roots.visiblePackages / "FixtureSkin/old-only.txt"),
         "replacement never merges an old-only file");
  expect(readText(roots.visiblePackages / "FixtureSkin/new-only.txt") == "new",
         "replacement exposes the complete new package");
  for (const auto &diagnostic : result.diagnostics) {
    expect(diagnostic.message.find(temp.root().string()) == std::string::npos,
           "publish diagnostics do not expose a raw host path");
  }
}

void testActivationCommitRemovalAndLeaseAwareGarbageCollection() {
  TempDirectory temp;
  const SkinStorageRoots roots = rootsBelow(temp.root());
  const auto package = normalizePackageId("FixtureSkin");
  if (!package.package) {
    expect(false, "activation fixture package ID is valid");
    return;
  }
  const fs::path source = temp.root() / "activation-source";
  writeText(source / "play/play7.luaskin", "return { type = 0 }");
  NoAliases aliases;
  SkinArchiveImporter importer(roots, aliases);
  auto candidate = importer.prepareFolder(source, *package.package, {}, {});
  expect(candidate.prepared.has_value(),
         "activation fixture prepares a package");
  if (!candidate.prepared) {
    return;
  }
  SkinPackageCatalog catalog(roots.privateCatalog);
  FakeProfileSnapshots profiles;
  SelectableValidator validator;
  SkinPackageStore store(roots, catalog, aliases, profiles);
  expect(store.recoverBeforeServiceStart().disposition ==
             SkinRecoveryDisposition::Recovered,
         "activation fixture bootstraps the store");
  auto published = store.publish(
      std::move(*candidate.prepared), PackageCollisionPolicy::Reject,
      ProfileInventorySnapshot{.inventoryGeneration = 1}, validator, {}, {});
  expect(published.published && published.entries.size() == 1,
         "activation fixture publishes one validated entry");
  if (!published.published || published.entries.empty()) {
    return;
  }
  const SkinEntryId entry = published.entries.front();
  const std::string activationDigest(64, '2');
  validator.configurationDigest = activationDigest;
  VersionedSkinProfileSettings base{
      .profileId =
          SkinProfileId{.opaque = "12345678-1234-1234-1234-123456789abc"},
      .generation = 4};
  base.settings.gameplayCompatibilityEnabled = true;
  base.settings.selected7KeyEntry = entry;
  base.settings.entries.emplace(entry, EntryProfileSettings{});
  SkinProfileSettings candidateSettings = base.settings;
  auto activation =
      store.prepareActivation(base, entry, candidateSettings, validator, {});
  expect(activation.prepared.has_value(),
         "activation preparation validates without mutating store state");
  expect(
      !store.acquireValidatedActivation(base.profileId, entry, activationDigest)
           .activation,
      "prepared activation is unavailable before durable profile save");
  if (!activation.prepared) {
    return;
  }
  FakeProfileOwner owner(base);
  auto begun = store.beginPreparedActivationCommit(
      std::move(*activation.prepared), owner);
  expect(begun.disposition == ActivationCommitDisposition::PendingProfileSave &&
             begun.ticket != 0,
         "activation commit retains a distinct nonzero Store ticket");
  expect(store.pollPreparedActivationCommit(begun.ticket, owner).disposition ==
             ActivationCommitDisposition::PendingProfileSave,
         "pending owner save retains the old activation");
  owner.persisted = true;
  auto committed = store.pollPreparedActivationCommit(begun.ticket, owner);
  expect(committed.disposition ==
                 ActivationCommitDisposition::ActivatedRequested &&
             committed.activation.has_value(),
         "persisted owner save performs the activation CAS");
  expect(owner.acknowledgements == 1,
         "Store acknowledges the exact owner ticket once after CAS");
  auto acquired =
      store.acquireValidatedActivation(base.profileId, entry, activationDigest);
  expect(acquired.activation.has_value(),
         "exact profile-entry-configuration activation can be cloned");
  const auto &validatedDigests = store.catalogSnapshot()
                                     ->entries.front()
                                     .validatedConfigurationDigests;
  expect(std::ranges::find(validatedDigests, activationDigest) !=
             validatedDigests.end(),
         "activation CAS publishes a newly validated configuration digest");
  const auto repeatedTerminal =
      store.pollPreparedActivationCommit(begun.ticket, owner);
  expect(repeatedTerminal.disposition ==
             ActivationCommitDisposition::RetainedPrevious,
         "polling a terminal Store ticket again reports it as unknown");
  expect(owner.acknowledgements == 1,
         "polling a terminal Store ticket cannot acknowledge twice");

  const auto prepareDigest = [&](char digit) {
    validator.configurationDigest = std::string(64, digit);
    return store.prepareActivation(base, entry, candidateSettings, validator,
                                   {});
  };
  {
    auto generationChanged = prepareDigest('3');
    expect(generationChanged.prepared.has_value(),
           "generation-change activation fixture prepares");
    if (generationChanged.prepared) {
      FakeProfileOwner changedOwner(base);
      changedOwner.terminalStatus =
          SkinProfileCommitResult::Status::GenerationChanged;
      const auto started = store.beginPreparedActivationCommit(
          std::move(*generationChanged.prepared), changedOwner);
      const auto terminal =
          store.pollPreparedActivationCommit(started.ticket, changedOwner);
      expect(terminal.disposition ==
                 ActivationCommitDisposition::ProfileGenerationChanged,
             "owner generation change retains the previous activation");
      (void)store.pollPreparedActivationCommit(started.ticket, changedOwner);
      expect(changedOwner.acknowledgements == 1,
             "owner generation-change ticket is acknowledged exactly once");
    }
  }
  {
    auto retryable = prepareDigest('4');
    expect(retryable.prepared.has_value(),
           "retryable-failure activation fixture prepares");
    if (retryable.prepared) {
      FakeProfileOwner retryOwner(base);
      retryOwner.terminalStatus =
          SkinProfileCommitResult::Status::RetryableFailure;
      const auto started = store.beginPreparedActivationCommit(
          std::move(*retryable.prepared), retryOwner);
      const auto terminal =
          store.pollPreparedActivationCommit(started.ticket, retryOwner);
      expect(terminal.disposition ==
                     ActivationCommitDisposition::RetainedPrevious &&
                 !terminal.diagnostics.empty(),
             "retryable owner failure retains activation and its diagnostic");
      (void)store.pollPreparedActivationCommit(started.ticket, retryOwner);
      expect(retryOwner.acknowledgements == 1,
             "retryable owner ticket is acknowledged exactly once");
    }
  }
  {
    auto throwingPoll = prepareDigest('5');
    expect(throwingPoll.prepared.has_value(),
           "throwing-poll activation fixture prepares");
    if (throwingPoll.prepared) {
      FakeProfileOwner throwingOwner(base);
      throwingOwner.throwPollOnce = true;
      throwingOwner.persisted = true;
      const auto started = store.beginPreparedActivationCommit(
          std::move(*throwingPoll.prepared), throwingOwner);
      const auto retry =
          store.pollPreparedActivationCommit(started.ticket, throwingOwner);
      expect(retry.disposition ==
                     ActivationCommitDisposition::PendingProfileSave &&
                 !retry.diagnostics.empty(),
             "a throwing owner poll leaves the Store ticket retryable");
      const auto terminal =
          store.pollPreparedActivationCommit(started.ticket, throwingOwner);
      expect(terminal.disposition ==
                 ActivationCommitDisposition::ActivatedRequested,
             "a later owner poll completes after a transient exception");
      expect(throwingOwner.acknowledgements == 1,
             "throwing owner poll is acknowledged only after terminal CAS");
    }
  }
  {
    auto raced = prepareDigest('6');
    expect(raced.prepared.has_value(), "post-save race activation prepares");
    if (raced.prepared) {
      FakeProfileOwner racedOwner(base);
      racedOwner.persisted = true;
      const auto started = store.beginPreparedActivationCommit(
          std::move(*raced.prepared), racedOwner);
      const auto rescanned = store.rescanVisibleSources(
          {}, {}, ProfileInventorySnapshot{.inventoryGeneration = 1},
          validator);
      expect(!rescanned.retryableInventoryRace,
             "post-save race fixture advances the source generation");
      const auto terminal =
          store.pollPreparedActivationCommit(started.ticket, racedOwner);
      expect(terminal.disposition ==
                 ActivationCommitDisposition::ProfileCommittedNeedsRevalidation,
             "source race after profile save requests revalidation");
      (void)store.pollPreparedActivationCommit(started.ticket, racedOwner);
      expect(racedOwner.acknowledgements == 1,
             "post-save source race acknowledges exactly once");
    }
  }
  {
    auto throwingBegin = prepareDigest('7');
    expect(throwingBegin.prepared.has_value(),
           "throwing-begin activation fixture prepares");
    if (throwingBegin.prepared) {
      FakeProfileOwner throwingOwner(base);
      throwingOwner.throwBegin = true;
      const auto rejected = store.beginPreparedActivationCommit(
          std::move(*throwingBegin.prepared), throwingOwner);
      expect(rejected.ticket == 0 && !rejected.diagnostics.empty(),
             "throwing owner begin erases the preallocated Store ticket");
      expect(throwingOwner.acknowledgements == 0,
             "unadmitted throwing owner begin is never acknowledged");
    }
  }

  auto removed = store.removePackage(*package.package, {});
  expect(removed.removed && !fs::exists(roots.visiblePackages / "FixtureSkin"),
         "explicit removal hides discovery and removes activation keys");
  std::error_code reservationError;
  expect(fs::is_empty(roots.privateCatalog / ".operation-reservations",
                      reservationError) &&
             !reservationError,
         "successful removal releases its exclusive operation reservation");
  expect(
      !store.acquireValidatedActivation(base.profileId, entry, activationDigest)
           .activation,
      "removed package cannot start a new skin session");
  expect(store.collectGarbage().revisionsRemoved == 0,
         "a cloned active revision lease prevents garbage collection");
  acquired.activation.reset();
  committed.activation.reset();
  const auto collected = store.collectGarbage();
  expect(collected.revisionsRemoved == 1,
         "the final released revision lease makes the revision collectible");
}

static_assert(!std::is_copy_constructible_v<ValidatedSkinActivation>);
static_assert(!std::is_copy_constructible_v<SkinDeferredCleanup>);
static_assert(!std::is_copy_constructible_v<SkinProfileMutationBarrier>);
static_assert(std::is_move_constructible_v<SkinDeferredCleanup>);
static_assert(std::is_move_constructible_v<SkinProfileMutationBarrier>);
} // namespace

int main(int argc, char **argv) {
  testFakeProviderFenceReleasesExactlyOnce();
  if (argc == 2 && std::string_view(argv[1]) == "--fence-only") {
    return failures == 0 ? 0 : 1;
  }
  testRecoveryFixtureDigestsBindPhysicalBytes();
  testCatalogJournalReplayAtEveryDurabilityBoundary();
  testCreateJournalRestoresAbsenceOrCompleteNewPackage();
  testCorruptNewCatalogFallsBackToCompleteOldGeneration();
  testRecoveryRejectsRepeatedBootstrapOwnership();
  testRecoveryRejectsOverlappingBootstrapOwnership();
  testMalformedAndOversizedCatalogFailBootstrap();
  testRemovalJournalRecoversOldAndNewGenerations();
  testRemovalRecoveryRetainsIntentWhenOwnedCleanupCannotFinish();
  testStableRevisionRestoresCorruptedTransactionCopies();
  testIntentOnlyRecoveryNeedsNoUnjournaledCatalogArtifacts();
  testOperationNamesDoNotReusePreseededPriorProcessArtifacts();
  testBootstrapCleansOnlyBoundedRecognizableOwnedArtifacts();
  testInventoryFenceAndDescendantEditReturnPreparedWithoutMutation();
  testNormalizedPhysicalCollisionsRejectOrReplaceAsOnePackage();
  testAmbiguousPhysicalCollisionCannotPersistDuplicateCatalogIdentity();
  testEncoderInvalidSnapshotPerformsZeroPublicationMutation();
  testConfiguredValidationFailureAndCancellationPreserveOldPackage();
  testTransactionFailureRetainsJournalForRestartRecovery();
  testGarbageCollectionRejectsLinksAndRetriesQuarantine();
  testGarbageCollectionNeverUnlinksAnExchangedQuarantinePath();
  testManualInvalidEditRetainsActivationButDeleteHidesIt();
  testReplacementPublishesExactlyTheNewWholePackage();
  testActivationCommitRemovalAndLeaseAwareGarbageCollection();

  if (failures != 0) {
    std::cerr << failures << " skin package store assertion(s) failed\n";
  }
  return failures == 0 ? 0 : 1;
}
