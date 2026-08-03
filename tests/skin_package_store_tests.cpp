#include "skin/SkinCommitCoordinator.h"
#include "skin/package/SkinPackageOperationService.h"
#include "skin/package/SkinPackageStore.h"
#include "skin/package/SkinPathPolicy.h"
#include "FileChecksum.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
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
            ("asobmashow-skin-store-red-" +
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
    return issueFence();
  }

  ProfileInventoryMutationBarrier beginInventoryMutation() override {
    throw std::logic_error("not used by catalog RED tests");
  }

  void finishInventoryMutation(
      ProfileInventoryMutationBarrier &&) noexcept override {}

  int releaseCount = 0;
};

class SelectableValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *,
                                std::stop_token) override {
    SkinEntryMetadataSnapshot metadata;
    metadata.displayName = "Synthetic Play";
    metadata.author = "AsoBMaShow tests";
    metadata.skinType = 0;
    metadata.authoredWidth = 1280;
    metadata.authoredHeight = 720;
    return {
        .disposition = SkinValidationDisposition::Selectable7Key,
        .reconciledSettings = EntryProfileSettings{},
        .metadata = std::move(metadata),
        .configurationDigest =
            "1111111111111111111111111111111111111111111111111111111111111111"};
  }
};

constexpr std::string_view kOldTreeDigest =
    "e2a1d82523f74a9ac8c8a500848fb173055c60d2430b3a8d0f67d6a6e0052ee1";
constexpr std::string_view kNewTreeDigest =
    "9ec86e2e753614c8b0667f1aa0a67784c1a97337eb348c3984f5fd492abd9fd5";
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
         "\"backupToken\":\"op-17\",\"oldTreeDigest\":\"" +
         std::string(kOldTreeDigest) + "\",\"newTreeDigest\":\"" +
         std::string(kNewTreeDigest) + "\"},\"revision\":{\"oldDigest\":\"" +
         std::string(kOldTreeDigest) + "\",\"newDigest\":\"" +
         std::string(kNewTreeDigest) +
         "\",\"stagingToken\":\"revision-op-17\"},\"catalog\":{"
         "\"fileName\":\"catalog.json\",\"oldGeneration\":7,"
         "\"newGeneration\":8,\"oldSnapshotDigest\":\"" +
         std::string(kOldCatalogDigest) + "\",\"newSnapshotDigest\":\"" +
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
  writeOldTree(oldSource);
  writeNewTree(newSource);
  NoAliases aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto oldSnapshot = snapshotter.snapshot(oldSource, *package.package, {}, {});
  auto newSnapshot = snapshotter.snapshot(newSource, *package.package, {}, {});
  expect(oldSnapshot.prepared.has_value() &&
             oldSnapshot.prepared->revision().lowercaseSha256 == kOldTreeDigest,
         "old tree fixture digest binds its exact files");
  expect(newSnapshot.prepared.has_value() &&
             newSnapshot.prepared->revision().lowercaseSha256 == kNewTreeDigest,
         "new tree fixture digest binds its exact files");
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
    const fs::path newRevisionStaging =
        roots.privateRevisions / ".staging/revision-op-17";
    const fs::path catalogFile = roots.privateCatalog / "catalog.json";
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
    const std::string journal = journalDocument(boundary.phase);
    expect(journal.find(temp.root().string()) == std::string::npos,
           "publication journal owns typed identities, never host paths");
    writeText(journalFile, journal);

    SkinPackageCatalog catalog(roots.privateCatalog);
    FakeProfileSnapshots profiles;
    NoAliases aliases;
    SkinPackageStore store(roots, catalog, aliases, profiles);
    store.recover();
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
    expect(readText(catalogFile) == (expectNew
                                         ? catalogDocument(8, kNewTreeDigest)
                                         : catalogDocument(7, kOldTreeDigest)),
           "recovery repairs catalog metadata to the selected generation");
    expect(!fs::exists(visibleStaging) && !fs::exists(visibleBackup) &&
               !fs::exists(newRevisionStaging) && !fs::exists(journalFile),
           "recovery cleans journal-owned staging and backup capabilities");
  }
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
  testReplacementPublishesExactlyTheNewWholePackage();

  if (failures != 0) {
    std::cerr << failures
              << " expected Task 7 RED assertion(s); implementation pending\n";
  }
  return failures == 0 ? 0 : 1;
}
