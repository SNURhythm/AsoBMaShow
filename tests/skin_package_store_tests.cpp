#include "skin/SkinCommitCoordinator.h"
#include "skin/package/SkinPackageOperationService.h"
#include "skin/package/SkinPackageStore.h"
#include "skin/package/SkinPathPolicy.h"

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

std::string catalogDocument(std::string_view packageName,
                            std::uint64_t generation) {
  const std::string_view collisionKey =
      packageName == "OldSkin" ? "oldskin" : "newskin";
  return "{\n"
         "  \"schemaVersion\": 1,\n"
         "  \"catalogGeneration\": " +
         std::to_string(generation) +
         ",\n  \"sourceGeneration\": " + std::to_string(generation) +
         ",\n  \"packages\": [{\"directoryName\": \"" +
         std::string(packageName) + "\", \"collisionKey\": \"" +
         std::string(collisionKey) + "\"}],\n  \"entries\": []\n}\n";
}

std::string journalDocument(std::string_view phase) {
  return "{\n"
         "  \"schemaVersion\": 1,\n"
         "  \"operation\": \"replace-package\",\n"
         "  \"phase\": \"" +
         std::string(phase) +
         "\",\n  \"packageDirectoryName\": \"FixtureSkin\",\n"
         "  \"oldCatalog\": " +
         catalogDocument("OldSkin", 7) +
         ",\n  \"newCatalog\": " + catalogDocument("NewSkin", 8) + "\n}\n";
}

bool snapshotIsExactly(const SkinPackageCatalogSnapshot &snapshot,
                       std::string_view packageName, std::uint64_t generation) {
  return snapshot.catalogGeneration == generation &&
         snapshot.sourceGeneration == generation &&
         snapshot.packages.size() == 1 &&
         snapshot.packages.front().directoryName == packageName &&
         snapshot.entries.empty();
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
  struct Boundary {
    std::string_view phase;
    bool newCatalogIsDurable;
  };
  constexpr Boundary boundaries[] = {
      {"intent-written", false},
      {"intent-parent-synced", false},
      {"visible-backup-renamed", false},
      {"visible-backup-parent-synced", false},
      {"visible-published", true},
      {"visible-parent-synced", true},
      {"revision-published", true},
      {"revision-parent-synced", true},
      {"catalog-published", true},
      {"catalog-parent-synced", true},
  };

  for (const Boundary &boundary : boundaries) {
    TempDirectory temp;
    const fs::path catalogRoot = temp.root() / "catalog";
    writeText(catalogRoot / "catalog.json", catalogDocument("OldSkin", 7));
    writeText(catalogRoot / "publication-journal.json",
              journalDocument(boundary.phase));

    SkinPackageCatalog catalog(catalogRoot);
    catalog.recover();
    const auto recovered = catalog.snapshot();
    const bool expected = boundary.newCatalogIsDurable
                              ? snapshotIsExactly(*recovered, "NewSkin", 8)
                              : snapshotIsExactly(*recovered, "OldSkin", 7);
    expect(expected,
           std::string("journal recovery selects one whole generation after ") +
               std::string(boundary.phase));
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
  testCatalogJournalReplayAtEveryDurabilityBoundary();
  testReplacementPublishesExactlyTheNewWholePackage();

  if (failures != 0) {
    std::cerr << failures
              << " expected Task 7 RED assertion(s); implementation pending\n";
  }
  return failures == 0 ? 0 : 1;
}
