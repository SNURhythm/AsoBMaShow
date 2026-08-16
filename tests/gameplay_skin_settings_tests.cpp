#include "scene/GameplaySkinSettingsController.h"

#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPackageCatalog.h"
#include "skin/package/SkinPackageStore.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
            ("asobmashow-gameplay-skin-settings-" +
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
  fs::create_directories(root / "Documents");
  return {.visiblePackages = root / "Documents/Skins",
          .privateRevisions = root / "ApplicationSupport/revisions",
          .privateCatalog = root / "ApplicationSupport/catalog",
          .profileOverlays = root / "ApplicationSupport/overlays"};
}

void writeText(const fs::path &path, std::string_view text) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

bool hasVisibleImportStaging(const fs::path &skinsRoot) {
  std::error_code error;
  if (!fs::exists(skinsRoot, error)) {
    return false;
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(skinsRoot,
                                                                  error)) {
    if (entry.is_directory(error) &&
        entry.path().filename().string().starts_with("import-")) {
      return true;
    }
  }
  return false;
}

fs::path makeZip(const fs::path &path, std::string_view member,
                 std::string_view contents) {
  archive *writer = archive_write_new();
  archive_write_set_format_zip(writer);
  archive_write_zip_set_compression_store(writer);
  archive_write_open_filename(writer, path.string().c_str());
  archive_entry *entry = archive_entry_new();
  archive_entry_set_pathname(entry, std::string(member).c_str());
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0600);
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  archive_write_header(writer, entry);
  archive_write_data(writer, contents.data(), contents.size());
  archive_entry_free(entry);
  archive_write_close(writer);
  archive_write_free(writer);
  return path;
}

class NoAliases final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class SelectableValidator final : public SkinEntryValidator {
public:
  SkinValidationResult validate(SkinRevisionReadView, const SkinEntryId &,
                                const EntryProfileSettings *desired,
                                std::stop_token) override {
    if (invalidConfiguration) {
      return {
          .disposition = SkinValidationDisposition::Invalid,
          .diagnostics = {{.code = "controller.test.invalid_configuration",
                           .message = "injected invalid configuration",
                           .severity = DiagnosticSeverity::Error}},
      };
    }
    EntryProfileSettings reconciled =
        desired != nullptr ? *desired : EntryProfileSettings{};
    SkinEntryMetadataSnapshot metadata;
    metadata.displayName = "Controller Fixture";
    metadata.author = "AsoBMaShow tests";
    metadata.skinType = skinType.load();
    metadata.authoredWidth = 1280;
    metadata.authoredHeight = 720;
    metadata.options.push_back({
        .category = "Layout",
        .name = "Play Side",
        .choices = {{.label = "1P", .value = 920},
                    {.label = "2P", .value = 921}},
        .defaultLabel = "1P",
    });
    return {.disposition = SkinValidationDisposition::SelectableGameplay,
            .reconciledSettings = reconciled,
            .metadata = std::move(metadata),
            .configurationDigest = skinConfigurationDigest(reconciled)};
  }

  std::atomic_bool invalidConfiguration = false;
  std::atomic_int skinType = 0;
};

class FakeProfileOwner final : public ISkinProfileSettingsOwner,
                               public ISkinProfileSnapshotProvider {
public:
  explicit FakeProfileOwner(std::vector<SkinProfileId> profiles) {
    for (auto &profile : profiles) {
      snapshots_.emplace(
          profile.opaque,
          VersionedSkinProfileSettings{.profileId = profile, .generation = 1});
    }
  }

  VersionedSkinProfileSettings
  snapshot(const SkinProfileId &profile) const override {
    return snapshots_.at(profile.opaque);
  }

  SkinProfileCommitResult beginCommit(const SkinProfileId &profile,
                                      std::uint64_t expectedGeneration,
                                      SkinProfileSettings candidate) override {
    auto &current = snapshots_.at(profile.opaque);
    if (rejectNextCommitGeneration) {
      rejectNextCommitGeneration = false;
      return {.status = SkinProfileCommitResult::Status::GenerationChanged,
              .generationChanged = true,
              .snapshot = current};
    }
    if (current.generation != expectedGeneration) {
      return {.status = SkinProfileCommitResult::Status::GenerationChanged,
              .generationChanged = true,
              .snapshot = current};
    }
    const std::uint64_t ticket = ++nextCommitTicket_;
    ++current.generation;
    current.settings = std::move(candidate);
    commits_.emplace(ticket, profile);
    return {.status = SkinProfileCommitResult::Status::Pending,
            .ticket = ticket,
            .snapshot = current};
  }

  SkinProfileCommitResult pollCommit(std::uint64_t ticket) override {
    const auto found = commits_.find(ticket);
    if (found == commits_.end()) {
      return {.status = SkinProfileCommitResult::Status::RetryableFailure,
              .ticket = ticket};
    }
    if (!commitsReady) {
      return {.status = SkinProfileCommitResult::Status::Pending,
              .ticket = ticket};
    }
    return {.status = SkinProfileCommitResult::Status::Persisted,
            .ticket = ticket,
            .snapshot = snapshots_.at(found->second.opaque)};
  }

  void acknowledgeCommit(std::uint64_t ticket) noexcept override {
    commits_.erase(ticket);
    ++acknowledgedCommits;
  }

  std::uint64_t beginSnapshotAllProfiles() override {
    ++inventorySnapshotsStarted;
    const std::uint64_t ticket = ++nextInventoryTicket_;
    inventories_.push_back(ticket);
    return ticket;
  }

  std::optional<AllSkinProfileSnapshotsResult>
  pollSnapshotAllProfiles(std::uint64_t ticket) override {
    if (!inventoryReady) {
      return std::nullopt;
    }
    if (std::ranges::find(inventories_, ticket) == inventories_.end()) {
      return std::nullopt;
    }
    if (!inventoryComplete) {
      return AllSkinProfileSnapshotsResult{
          .complete = false,
          .diagnostics = {{.code = "controller.test.inventory_incomplete",
                           .message = "injected incomplete inventory",
                           .severity = DiagnosticSeverity::Error}}};
    }
    if (!inventoryPresent) {
      return AllSkinProfileSnapshotsResult{
          .complete = true,
          .diagnostics = {{.code = "controller.test.inventory_missing",
                           .message = "injected missing inventory payload",
                           .severity = DiagnosticSeverity::Error}}};
    }
    ProfileInventorySnapshot inventory{.inventoryGeneration =
                                           ++inventoryGeneration_};
    for (const auto &[_, snapshot] : snapshots_) {
      inventory.profiles.push_back(snapshot);
    }
    return AllSkinProfileSnapshotsResult{.complete = true,
                                         .inventory = std::move(inventory)};
  }

  void cancelSnapshotAllProfiles(std::uint64_t ticket) noexcept override {
    std::erase(inventories_, ticket);
  }

  std::optional<ProfileInventoryCommitFence>
  tryAcquireInventoryCommitFence(const ProfileInventorySnapshot &) override {
    if (inventoryFenceRejections > 0) {
      --inventoryFenceRejections;
      if (pauseInventoryAfterFenceRejection) {
        inventoryReady = false;
      }
      return std::nullopt;
    }
    return makeInventoryCommitFence([] {});
  }

  ProfileInventoryMutationBarrier beginInventoryMutation() override {
    throw std::logic_error("not used by settings controller tests");
  }

  void finishInventoryMutation(
      ProfileInventoryMutationBarrier &&) noexcept override {}

  std::atomic_bool inventoryReady = true;
  std::atomic_bool pauseInventoryAfterFenceRejection = false;
  bool inventoryComplete = true;
  bool inventoryPresent = true;
  bool commitsReady = true;
  bool rejectNextCommitGeneration = false;
  int inventoryFenceRejections = 0;
  int inventorySnapshotsStarted = 0;
  int acknowledgedCommits = 0;

private:
  std::uint64_t nextCommitTicket_ = 0;
  std::uint64_t nextInventoryTicket_ = 0;
  std::uint64_t inventoryGeneration_ = 0;
  std::map<std::string, VersionedSkinProfileSettings> snapshots_;
  std::map<std::uint64_t, SkinProfileId> commits_;
  std::vector<std::uint64_t> inventories_;
};

class FailNthAdmissionObserver final : public SkinPackageOperationTestObserver {
public:
  explicit FailNthAdmissionObserver(int failureCall)
      : failureCall_(failureCall) {}

  bool failAdmissionAllocation() const noexcept override {
    return ++calls_ == failureCall_;
  }
  void beforeCompletion(std::uint64_t) const noexcept override {}
  void completed(std::uint64_t) const noexcept override {}
  void disposing(std::uint64_t) const noexcept override {}

private:
  int failureCall_ = 0;
  mutable std::atomic_int calls_ = 0;
};

class FailFirstAndBlockReservedDisposal final
    : public SkinPackageOperationTestObserver {
public:
  explicit FailFirstAndBlockReservedDisposal(bool failFirstAdmission = true)
      : failFirstAdmission_(failFirstAdmission) {}

  bool failAdmissionAllocation() const noexcept override {
    return failFirstAdmission_ && ++admissionCalls_ == 1;
  }
  void beforeCompletion(std::uint64_t) const noexcept override {}
  void completed(std::uint64_t) const noexcept override {}
  void disposing(std::uint64_t ticket) const noexcept override {
    if (ticket != 0) {
      return;
    }
    std::unique_lock lock(mutex_);
    ++reservedDisposals_;
    disposalStarted_ = true;
    started_.notify_all();
    release_.wait(lock, [&] { return releaseDisposal_; });
  }

  bool waitForReservedDisposal() const {
    std::unique_lock lock(mutex_);
    return started_.wait_for(lock, std::chrono::seconds(2),
                             [&] { return disposalStarted_; });
  }

  void releaseDisposal() {
    {
      std::scoped_lock lock(mutex_);
      releaseDisposal_ = true;
    }
    release_.notify_all();
  }

  int reservedDisposals() const noexcept { return reservedDisposals_.load(); }

private:
  bool failFirstAdmission_ = true;
  mutable std::atomic_int admissionCalls_ = 0;
  mutable std::atomic_int reservedDisposals_ = 0;
  mutable std::mutex mutex_;
  mutable std::condition_variable started_;
  mutable std::condition_variable release_;
  mutable bool disposalStarted_ = false;
  mutable bool releaseDisposal_ = false;
};

PlatformDocumentHandoffResult picked(std::filesystem::path path,
                                     std::string original,
                                     PlatformTemporaryPathKind kind) {
  return {.status = PlatformDocumentHandoffStatus::Succeeded,
          .localPath = std::move(path),
          .originalSourceName = std::move(original),
          .temporaryPathKind = kind};
}

std::pair<fs::path, PlatformDocumentHandoffResult>
ownedPickedFolder(std::string original,
                  PlatformDocumentHandoffStatus status =
                      PlatformDocumentHandoffStatus::Succeeded,
                  std::string message = {}) {
  std::string allocationError;
  const auto path =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          fs::temp_directory_path(), allocationError);
  writeText(path / "play/play7.luaskin", "return { type = 0 }");
  auto result = platform_document_handoff::detail::ParseBridgeResult(
      path.string(), true, true, PlatformTemporaryPathKind::Directory,
      std::move(original));
  result.status = status;
  result.message = std::move(message);
  return {path, std::move(result)};
}

platform_document_handoff::PlatformDocumentHandoffOperation
completedHandoff(PlatformDocumentHandoffResult result) {
  return platform_document_handoff::detail::StartOperation(
      [result = std::move(result)](const std::atomic_bool &) mutable {
        return std::move(result);
      },
      [] {});
}

struct Fixture {
  explicit Fixture(
      std::shared_ptr<const SkinPackageOperationTestObserver> observer = {})
      : roots(rootsBelow(temp.root())), catalog(roots.privateCatalog),
        owner({profileA, profileB}), store(roots, catalog, aliases, owner),
        history(catalog), commits(store, owner) {
    expect(store.recoverBeforeServiceStart().disposition ==
               SkinRecoveryDisposition::Recovered,
           "controller fixture recovers storage before service startup");
    operations = std::make_unique<SkinPackageOperationService>(
        store, validator, std::move(observer));
  }

  ~Fixture() {
    operations->shutdown();
    commits.shutdown();
    history.flush();
    catalog.flush();
    catalog.shutdown();
  }

  std::unique_ptr<GameplaySkinSettingsController> makeController() {
    return std::make_unique<GameplaySkinSettingsController>(
        GameplaySkinSettingsControllerDependencies{
            .operations = *operations,
            .history = history,
            .profileId = profileA,
            .profileOwner = owner,
            .profileSnapshots = owner,
            .commits = commits,
            .clientId = commits.createClient(),
            .requestRescan = [&] { ++rescanRequests; },
            .rescanProgress = [&] { return rescanProgress; },
            .requestRevalidation =
                [&](const SkinEntryId &) { ++revalidationRequests; },
            .catalogSnapshot =
                [&] {
                  ++catalogSnapshotCalls;
                  return catalogOverride ? catalogOverride
                                         : operations->catalogSnapshot();
                },
            .beginArchiveHandoff =
                [&] {
                  if (archiveResults.empty()) {
                    return platform_document_handoff::
                        PlatformDocumentHandoffOperation{};
                  }
                  auto result = std::move(archiveResults.front());
                  archiveResults.pop_front();
                  return completedHandoff(std::move(result));
                },
            .beginFolderHandoff =
                [&](PlatformDirectoryImportRequest request) {
                  lastFolderRequest = request;
                  if (folderResults.empty()) {
                    return platform_document_handoff::
                        PlatformDocumentHandoffOperation{};
                  }
                  auto result = std::move(folderResults.front());
                  folderResults.pop_front();
                  return completedHandoff(std::move(result));
                },
        });
  }

  TempDirectory temp;
  SkinStorageRoots roots;
  SkinPackageCatalog catalog;
  NoAliases aliases;
  const SkinProfileId profileA{.opaque =
                                   "11111111-1111-1111-1111-111111111111"};
  const SkinProfileId profileB{.opaque =
                                   "22222222-2222-2222-2222-222222222222"};
  FakeProfileOwner owner;
  SkinPackageStore store;
  SelectableValidator validator;
  std::unique_ptr<SkinPackageOperationService> operations;
  SkinDiagnosticHistory history;
  SkinCommitCoordinator commits;
  std::deque<PlatformDocumentHandoffResult> archiveResults;
  std::deque<PlatformDocumentHandoffResult> folderResults;
  PlatformDirectoryImportRequest lastFolderRequest;
  int rescanRequests = 0;
  SkinRescanProgress rescanProgress;
  int revalidationRequests = 0;
  int catalogSnapshotCalls = 0;
  std::shared_ptr<const SkinPackageCatalogSnapshot> catalogOverride;
};

template <typename Predicate>
bool pumpUntil(Fixture &fixture, GameplaySkinSettingsController &controller,
               Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    controller.poll();
    fixture.commits.poll();
    controller.poll();
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

void testSourceNameSuggestionPreservesTypedSemantics() {
  const auto archive =
      suggestSkinPackageName("ModernChic.ZIP", PlatformTemporaryPathKind::File);
  expect(archive.ok() && archive.suggestedPackageName == "ModernChic",
         "archive suggestion strips one case-insensitive zip suffix");
  const auto repeated = suggestSkinPackageName("ModernChic.zip.zip",
                                               PlatformTemporaryPathKind::File);
  expect(repeated.ok() && repeated.suggestedPackageName == "ModernChic.zip",
         "archive suggestion strips exactly one zip suffix");
  const auto folder = suggestSkinPackageName(
      "ModernChic.zip", PlatformTemporaryPathKind::Directory);
  expect(folder.ok() && folder.suggestedPackageName == "ModernChic.zip",
         "folder suggestion preserves its selected basename");
  const auto invalid = suggestSkinPackageName("../ModernChic.zip",
                                              PlatformTemporaryPathKind::File);
  expect(!invalid.ok(), "source-name path components fail typed validation");
}

void testSnapshotUsesCachedSettingsProjectionUntilTheNextPoll() {
  Fixture fixture;
  auto controller = fixture.makeController();
  expect(fixture.catalogSnapshotCalls == 1,
         "controller creation captures the initial catalog projection once");
  (void)controller->snapshot();
  (void)controller->snapshot();
  expect(fixture.catalogSnapshotCalls == 1,
         "repeated Settings reads reuse the cached skin title projection");
  controller->poll();
  expect(fixture.catalogSnapshotCalls == 2,
         "the next controller poll checks for a newer catalog generation");
}

void installQueuedImport(Fixture &fixture,
                         GameplaySkinSettingsController &controller,
                         bool archive) {
  const auto begun = archive ? controller.beginArchiveImport()
                             : controller.beginFolderImport();
  expect(begun.accepted && begun.asynchronous,
         "document import starts asynchronously");
  expect(pumpUntil(fixture, controller,
                   [&] {
                     return controller.snapshot().state ==
                                GameplaySkinSettingsState::Ready &&
                            !controller.snapshot().entries.empty();
                   }),
         "picked source prepares and publishes without a package-name gate");
}

void testArchiveFolderSelectionAndDurableLayoutFlow() {
  Fixture fixture;
  const auto archive = makeZip(fixture.temp.root() / "ModernChic.ZIP",
                               "play/play7.luaskin", "return { type = 0 }");
  const auto folder = fixture.temp.root() / "UnpackedSkin.zip";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.archiveResults.push_back(
      picked(archive, "ModernChic.ZIP", PlatformTemporaryPathKind::File));
  fixture.folderResults.push_back(
      picked(folder, "UnpackedSkin.zip", PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();

  installQueuedImport(fixture, *controller, true);
  expect(controller->snapshot().entries.front().entry.package.directoryName ==
             "ModernChic",
         "archive publication uses the editable stripped package name");
  installQueuedImport(fixture, *controller, false);
  expect(fixture.lastFolderRequest.maxBytes ==
                 SkinPackagePolicy::maxExpandedBytes &&
             fixture.lastFolderRequest.maxFiles == SkinPackagePolicy::maxFiles,
         "folder picker receives the bounded package policy");
  expect(controller->snapshot().entries.size() == 2,
         "archive and unarchived folder packages coexist in the catalog");

  const SkinEntryId entry = controller->snapshot().entries.front().entry;
  const auto selected = controller->select(entry);
  expect(selected.accepted && selected.asynchronous,
         "validated 7-key selection enters activation preparation");
  expect(pumpUntil(fixture, *controller,
                    [&] {
                      return fixture.owner.snapshot(fixture.profileA)
                                 .settings.selected7KeyEntry == entry &&
                             controller->snapshot().state ==
                                 GameplaySkinSettingsState::Ready;
                    }),
         "selection commits through the durable profile owner");

  const auto enabled = controller->setCompatibilityEnabled(true);
  expect(enabled.accepted && enabled.asynchronous,
         "a validated selected activation can enable compatibility");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                         .settings.gameplayCompatibilityEnabled;
                   }),
         "compatibility enablement persists durably");

  ViewportSettings stretch{.mode = ViewportMode::Stretch};
  expect(controller->setViewport(entry, stretch).accepted,
         "Stretch layout enters a profile-only commit");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                .settings.entries.at(entry)
                                .viewport.mode == ViewportMode::Stretch;
                   }),
         "Stretch layout persists without Lua revalidation");
  expect(controller->resetLayout(entry).accepted,
         "Reset Layout submits a Fit viewport");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                .settings.entries.at(entry)
                                .viewport.mode == ViewportMode::Fit;
                   }),
         "Reset Layout durably restores Fit");

  expect(controller->setCompatibilityEnabled(false).accepted,
         "legacy compatibility API can clear every trait selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     const auto settings =
                         fixture.owner.snapshot(fixture.profileA).settings;
                     return !settings.gameplayCompatibilityEnabled &&
                            settings.selectedGameplayEntries.empty();
                   }),
         "clearing compatibility removes trait selections");

  const auto clientB = fixture.commits.createClient();
  controller->profileChanged(fixture.profileB, clientB);
  expect(controller->setViewport(entry, stretch).accepted,
         "profile rebinding allows edits against the new owner snapshot");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileB)
                                .settings.entries.at(entry)
                                .viewport.mode == ViewportMode::Stretch;
                   }),
         "profile switch never writes the prior profile");
  expect(fixture.owner.snapshot(fixture.profileA)
                 .settings.entries.at(entry)
                 .viewport.mode == ViewportMode::Fit,
         "new-profile edits leave the old profile unchanged");
  controller->close();
}

void testPickedArchiveAndFolderInstallWithoutPackageNameConfirmation() {
  Fixture fixture;
  const auto archive = makeZip(fixture.temp.root() / "AutomaticArchive.zip",
                               "play/play7.luaskin", "return { type = 0 }");
  const auto folder = fixture.temp.root() / "AutomaticFolder";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.archiveResults.push_back(
      picked(archive, "AutomaticArchive.zip", PlatformTemporaryPathKind::File));
  fixture.folderResults.push_back(
      picked(folder, "AutomaticFolder", PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();

  expect(controller->beginArchiveImport().accepted,
         "automatic archive fixture opens the source picker");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready &&
                            controller->snapshot().entries.size() == 1;
                   }),
         "a picked archive installs without a package-name confirmation");
  expect(!controller->snapshot().preparedName.has_value() &&
             controller->snapshot().entries.front().entry.package.directoryName ==
                 "AutomaticArchive",
         "archive identity is derived from its source name without exposing a gate");

  expect(controller->beginFolderImport().accepted,
         "automatic folder fixture opens the source picker");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready &&
                            controller->snapshot().entries.size() == 2;
                   }),
         "a picked folder installs without a package-name confirmation");
  expect(!controller->snapshot().preparedName.has_value(),
         "folder installation leaves no package-name confirmation state");
}

void testSelectionIsScopedToTheSkinDeclaredGameplayTrait() {
  Fixture fixture;
  fixture.validator.skinType = 1;
  const auto folder = fixture.temp.root() / "FiveKey";
  writeText(folder / "play/play5.luaskin", "return { type = 1 }");
  fixture.folderResults.push_back(
      picked(folder, "FiveKey", PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;

  expect(!controller->selectGameplayTrait(0, entry).accepted,
         "a 5K skin cannot be selected for the 7K trait");
  expect(controller->selectGameplayTrait(1, entry).accepted,
         "a 5K skin selects its matching trait");
  expect(pumpUntil(
             fixture, *controller,
             [&] {
               const auto settings =
                   fixture.owner.snapshot(fixture.profileA).settings;
               const auto selected = settings.selectedGameplayEntries.find(1);
               return selected != settings.selectedGameplayEntries.end() &&
                      selected->second == entry && !settings.selected7KeyEntry;
             }),
         "the durable profile retains the 5K mapping without replacing 7K");
}

void testRejectedPublishTransfersReservedStagingOffControllerThread() {
  auto observer = std::make_shared<FailNthAdmissionObserver>(2);
  Fixture fixture(observer);
  const auto folder = fixture.temp.root() / "RejectedPublish";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(
      picked(folder, "RejectedPublish", PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "rejected-publish fixture starts its folder picker");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                            GameplaySkinSettingsState::Error;
                   }),
         "publish admission failure becomes a terminal controller error");

  const auto closeStarted = std::chrono::steady_clock::now();
  controller->close();
  const auto closeElapsed = std::chrono::steady_clock::now() - closeStarted;
  expect(closeElapsed < std::chrono::milliseconds(100),
         "controller close never waits for recursive staging deletion");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return !hasVisibleImportStaging(
                         fixture.roots.visiblePackages);
                   }),
         "reserved disposal eventually removes rejected staging off-thread");
  expect(fixture.operations->catalogSnapshot()->packages.empty(),
         "failed publication never exposes a partial package");
}

void testRejectedPrepareTransfersExactTemporaryCleanupOffControllerThread() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  std::string allocationError;
  const auto ownedFolder =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          fs::temp_directory_path(), allocationError);
  writeText(ownedFolder / "play/play7.luaskin", "return { type = 0 }");
  auto ownedResult = platform_document_handoff::detail::ParseBridgeResult(
      ownedFolder.string(), true, true, PlatformTemporaryPathKind::Directory,
      "RejectedPrepare");
  expect(ownedResult.ok() && ownedResult.temporaryOwnership,
         "prepare-rejection fixture owns a real private folder capability");
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "prepare-rejection fixture starts folder selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                            GameplaySkinSettingsState::Error;
                   }),
         "automatic preparation reports a zero-ticket rejection without a gate");

  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "the exact cleanup waits on the reserved service worker lane");
  const auto closeStarted = std::chrono::steady_clock::now();
  controller->close();
  expect(std::chrono::steady_clock::now() - closeStarted <
             std::chrono::milliseconds(100),
         "controller close does not wait for rejected-prepare cleanup");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "reserved worker eventually consumes the temporary ownership");
  expect(observer->reservedDisposals() == 1,
         "rejected prepare cleanup transfers exactly once");
}

void testNameReadyCancelTransfersOwnedSourceThroughReservedLane() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  auto [ownedFolder, ownedResult] = ownedPickedFolder("CancelOwnedSource");
  expect(ownedResult.ok() && ownedResult.temporaryOwnership,
         "cancel fixture owns a private picked folder capability");
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "cancel fixture starts owned folder selection");
  expect(pumpUntil(
             fixture, *controller,
             [&] { return controller->snapshot().preparedName.has_value(); }),
         "owned folder reaches package-name confirmation");
  const auto cancelStarted = std::chrono::steady_clock::now();
  controller->cancelOperation();
  expect(std::chrono::steady_clock::now() - cancelStarted <
             std::chrono::milliseconds(100),
         "NameReady cancellation does not run owned cleanup on the caller");

  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "NameReady cancellation transfers the exact cleanup to its reserved "
         "lane");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "reserved cancellation cleanup removes the owned folder after release");
  expect(observer->reservedDisposals() == 1,
         "NameReady cancellation consumes the reservation exactly once");
}

void testNameReadyCloseTransfersOwnedSourceThroughReservedLane() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  auto [ownedFolder, ownedResult] = ownedPickedFolder("CloseOwnedSource");
  expect(ownedResult.ok() && ownedResult.temporaryOwnership,
         "close fixture owns a private picked folder capability");
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "close fixture starts owned folder selection");
  expect(pumpUntil(
             fixture, *controller,
             [&] { return controller->snapshot().preparedName.has_value(); }),
         "owned folder reaches NameReady before close");
  const auto closeStarted = std::chrono::steady_clock::now();
  controller->close();
  expect(std::chrono::steady_clock::now() - closeStarted <
             std::chrono::milliseconds(100),
         "NameReady close does not run owned cleanup on the caller");

  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "NameReady close transfers the exact cleanup to its reserved lane");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "reserved close cleanup removes the owned folder after release");
  expect(observer->reservedDisposals() == 1,
         "NameReady close consumes the reservation exactly once");
}

void testProfileSwitchTransfersNameReadyOwnedSourceThroughReservedLane() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  auto [ownedFolder, ownedResult] = ownedPickedFolder("ProfileOwnedSource");
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "profile-switch fixture starts owned folder selection");
  expect(pumpUntil(
             fixture, *controller,
             [&] { return controller->snapshot().preparedName.has_value(); }),
         "owned folder reaches NameReady before profile switch");
  const auto switchStarted = std::chrono::steady_clock::now();
  controller->profileChanged(fixture.profileB, fixture.commits.createClient());
  expect(std::chrono::steady_clock::now() - switchStarted <
             std::chrono::milliseconds(100),
         "profile switch does not run NameReady cleanup on the caller");

  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "profile switch transfers the exact cleanup to the reserved lane");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "profile-switch cleanup removes the owned folder after release");
  expect(observer->reservedDisposals() == 1,
         "profile switch consumes the reservation exactly once");
}

void testCancelledPickerTransfersOwnedResultThroughReservedLane() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  auto [ownedFolder, ownedResult] = ownedPickedFolder(
      "CancelledOwnedSource", PlatformDocumentHandoffStatus::Cancelled);
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "cancelled-picker fixture starts owned folder selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().statusMessage ==
                            "Skin import cancelled.";
                   }),
         "owned cancelled result returns the controller to idle");
  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "owned cancelled result waits on the pre-acquired reserved lane");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "cancelled-result cleanup removes the exact owned folder");
  expect(observer->reservedDisposals() == 1,
         "owned cancelled result consumes the reservation exactly once");
}

void testFailedPickerTransfersOwnedResultThroughReservedLane() {
  auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>();
  Fixture fixture(observer);
  auto [ownedFolder, ownedResult] = ownedPickedFolder(
      "FailedOwnedSource", PlatformDocumentHandoffStatus::Failed,
      "injected picker failure");
  fixture.folderResults.push_back(std::move(ownedResult));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "failed-picker fixture starts owned folder selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                                GameplaySkinSettingsState::Error &&
                            controller->snapshot().statusMessage ==
                                "injected picker failure";
                   }),
         "owned failed result preserves its picker diagnostic");
  const bool disposalStarted = observer->waitForReservedDisposal();
  expect(disposalStarted && fs::exists(ownedFolder),
         "owned failed result waits on the pre-acquired reserved lane");
  observer->releaseDisposal();
  expect(
      pumpUntil(fixture, *controller, [&] { return !fs::exists(ownedFolder); }),
      "failed-result cleanup removes the exact owned folder");
  expect(observer->reservedDisposals() == 1,
         "owned failed result consumes the reservation exactly once");
}

void testReservationPrecedesPickerAndCancelTransfersLocalStaging() {
  Fixture fixture;
  const auto firstFolder = fixture.temp.root() / "HeldFolder";
  const auto secondArchive =
      makeZip(fixture.temp.root() / "Later.zip", "play/play7.luaskin",
              "return { type = 0 }");
  fixture.folderResults.push_back(
      picked(firstFolder, "HeldFolder", PlatformTemporaryPathKind::Directory));
  fixture.archiveResults.push_back(
      picked(secondArchive, "Later.zip", PlatformTemporaryPathKind::File));
  auto first = fixture.makeController();
  auto second = fixture.makeController();

  expect(first->beginFolderImport().accepted,
         "the first controller reserves disposal before opening its picker");
  expect(!second->beginArchiveImport().accepted &&
             fixture.archiveResults.size() == 1,
         "reservation exhaustion rejects before invoking a second picker");
  first->close();
  expect(second->beginArchiveImport().accepted,
         "closing the first picker releases its unused reservation");
  second->close();

  writeText(firstFolder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(
      picked(firstFolder, "HeldFolder", PlatformTemporaryPathKind::Directory));
  fixture.owner.inventoryReady = false;
  auto staging = fixture.makeController();
  expect(staging->beginFolderImport().accepted,
         "staging-cancel fixture starts folder selection");
  expect(pumpUntil(fixture, *staging,
                   [&] {
                     return staging->snapshot().statusMessage ==
                            "Loading profile inventory…";
                   }),
         "prepared staging waits behind the asynchronous profile inventory");

  const auto closeStarted = std::chrono::steady_clock::now();
  staging->close();
  expect(std::chrono::steady_clock::now() - closeStarted <
             std::chrono::milliseconds(100),
         "closing inventory-held staging is nonblocking");
  expect(pumpUntil(fixture, *staging,
                   [&] {
                     return !hasVisibleImportStaging(
                         fixture.roots.visiblePackages);
                   }),
         "cancel transfers prepared staging to reserved worker disposal");
}

void testProfileSwitchDetachesPendingActivationAndUnlocksNewProfile() {
  Fixture fixture;
  const auto folder = fixture.temp.root() / "PendingActivation";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "PendingActivation",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;

  fixture.owner.commitsReady = false;
  expect(controller->select(entry).accepted,
         "pending-activation fixture starts validated selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                .settings.selected7KeyEntry == entry;
                   }),
         "old-profile activation reaches its accepted durable commit");

  controller->profileChanged(fixture.profileB, fixture.commits.createClient());
  expect(controller->snapshot().state == GameplaySkinSettingsState::Ready,
         "profile switch clears detached activation wait state locally");
  const ViewportSettings stretch{.mode = ViewportMode::Stretch};
  expect(controller->setViewport(entry, stretch).accepted,
         "new profile accepts edits while old activation remains pending");

  fixture.owner.commitsReady = true;
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     const auto settings =
                         fixture.owner.snapshot(fixture.profileB).settings;
                     const auto found = settings.entries.find(entry);
                     return found != settings.entries.end() &&
                            found->second.viewport.mode ==
                                ViewportMode::Stretch &&
                            controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready;
                   }),
         "new-profile edit completes independently of detached activation");
  expect(fixture.owner.snapshot(fixture.profileA).settings.selected7KeyEntry ==
             entry,
         "detached old activation remains durable after profile switch");
}

void testProfileSwitchDetachesPendingProfileSaveAndUnlocksNewProfile() {
  Fixture fixture;
  const auto folder = fixture.temp.root() / "PendingProfileSave";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "PendingProfileSave",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;

  fixture.owner.commitsReady = false;
  const ViewportSettings stretch{.mode = ViewportMode::Stretch};
  expect(controller->setViewport(entry, stretch).accepted,
         "pending-profile fixture starts profile-only persistence");
  expect(fixture.owner.snapshot(fixture.profileA)
                 .settings.entries.at(entry)
                 .viewport.mode == ViewportMode::Stretch,
         "old profile owns its accepted optimistic settings snapshot");

  controller->profileChanged(fixture.profileB, fixture.commits.createClient());
  expect(controller->snapshot().state == GameplaySkinSettingsState::Ready,
         "profile switch clears detached profile-save wait state locally");
  const ViewportSettings custom{.mode = ViewportMode::Custom,
                                .customBase = CustomViewportBase::Fit,
                                .scaleX = 1.25F,
                                .scaleY = 0.75F,
                                .translateX = 20.0F,
                                .translateY = -12.0F};
  expect(controller->setViewport(entry, custom).accepted,
         "new profile accepts a save while old profile save remains pending");

  fixture.owner.commitsReady = true;
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     const auto settings =
                         fixture.owner.snapshot(fixture.profileB).settings;
                     const auto found = settings.entries.find(entry);
                     return found != settings.entries.end() &&
                            found->second.viewport == custom &&
                            controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready;
                   }),
         "new profile save completes without receiving the old completion");
  expect(fixture.owner.snapshot(fixture.profileA)
                 .settings.entries.at(entry)
                 .viewport.mode == ViewportMode::Stretch,
         "detached old profile save remains durable and profile-scoped");
}

void testCollisionRejectRetainsSourceAndReplacePublishesAtomically() {
  Fixture fixture;
  const auto original = fixture.temp.root() / "CollisionOriginal";
  const auto replacement = fixture.temp.root() / "CollisionReplacement";
  writeText(original / "play/play7.luaskin", "return { type = 0 }");
  writeText(replacement / "play/play7.luaskin",
            "return { type = 0, name = 'replacement' }");
  fixture.folderResults.push_back(
      picked(original, "Collision", PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const auto oldRevision =
      controller->snapshot().entries.front().revisionDigest;

  fixture.folderResults.push_back(
      picked(replacement, "Collision", PlatformTemporaryPathKind::Directory));
  expect(controller->beginFolderImport().accepted,
         "collision fixture starts replacement folder selection");
  expect(pumpUntil(
             fixture, *controller,
             [&] { return controller->snapshot().preparedName.has_value(); }),
         "collision fixture retains editable selected source");
  const auto rejectedCollision =
      controller->confirmPreparedImport(PackageCollisionPolicy::Reject);
  expect(!rejectedCollision.accepted &&
             controller->snapshot().collisionPackage.has_value() &&
             controller->snapshot().entries.front().revisionDigest ==
                 oldRevision,
         "Reject reports collision without replacing or consuming source");

  const auto replacementStarted =
      controller->confirmPreparedImport(PackageCollisionPolicy::Replace);
  expect(replacementStarted.accepted && replacementStarted.asynchronous,
         "Replace reuses the held source after collision confirmation");
  expect(pumpUntil(
             fixture, *controller,
             [&] {
               return controller->snapshot().state ==
                          GameplaySkinSettingsState::Ready &&
                      controller->snapshot().entries.size() == 1 &&
                      controller->snapshot().entries.front().revisionDigest !=
                          oldRevision;
             }),
         "Replace publishes one complete new revision without merging trees");
}

void testInventoryRaceRetriesThenSucceedsAndCancelDisposesRetry() {
  {
    Fixture fixture;
    const auto folder = fixture.temp.root() / "InventoryRetrySuccess";
    writeText(folder / "play/play7.luaskin", "return { type = 0 }");
    fixture.owner.inventoryFenceRejections = 2;
    fixture.folderResults.push_back(picked(
        folder, "InventoryRetrySuccess", PlatformTemporaryPathKind::Directory));
    auto controller = fixture.makeController();
    installQueuedImport(fixture, *controller, false);
    expect(fixture.owner.inventorySnapshotsStarted == 3 &&
               controller->snapshot().entries.size() == 1,
           "retryable inventory races reload bounded snapshots until success");
    controller->close();
  }

  Fixture fixture;
  const auto folder = fixture.temp.root() / "InventoryRetryCancel";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.owner.inventoryFenceRejections = 1;
  fixture.owner.pauseInventoryAfterFenceRejection = true;
  fixture.folderResults.push_back(picked(folder, "InventoryRetryCancel",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  expect(controller->beginFolderImport().accepted,
         "inventory-cancel fixture starts folder selection");
  expect(
      pumpUntil(fixture, *controller,
                [&] {
                  return fixture.owner.inventorySnapshotsStarted == 2 &&
                         controller->snapshot().statusMessage ==
                             "Loading profile inventory…";
                }),
      "retryable publication race returns exact staging for fresh inventory");
  controller->close();
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return !hasVisibleImportStaging(
                         fixture.roots.visiblePackages);
                   }),
         "cancelling a retry-held package disposes staging on service worker");
  expect(fixture.operations->catalogSnapshot()->packages.empty(),
         "cancelled inventory retry publishes no partial package");
}

void testMalformedProfileInventoriesDisposePreparedStagingOnReservedLane() {
  const auto exercise = [](bool complete, bool present,
                           std::string_view expectedMessage) {
    auto observer = std::make_shared<FailFirstAndBlockReservedDisposal>(false);
    Fixture fixture(observer);
    fixture.owner.inventoryComplete = complete;
    fixture.owner.inventoryPresent = present;
    const auto folder = fixture.temp.root() / "MalformedInventory";
    writeText(folder / "play/play7.luaskin", "return { type = 0 }");
    fixture.folderResults.push_back(picked(
        folder, "MalformedInventory", PlatformTemporaryPathKind::Directory));
    auto controller = fixture.makeController();

    expect(controller->beginFolderImport().accepted,
           "malformed-inventory fixture starts folder selection");
    expect(pumpUntil(fixture, *controller,
                     [&] {
                       return controller->snapshot().state ==
                                  GameplaySkinSettingsState::Error &&
                              controller->snapshot().statusMessage ==
                                  expectedMessage;
                     }),
           "malformed profile inventory becomes a terminal controller error");

    const bool disposalStarted = observer->waitForReservedDisposal();
    expect(disposalStarted && hasVisibleImportStaging(fixture.roots.visiblePackages),
           "prepared staging remains owned while reserved disposal is blocked");
    observer->releaseDisposal();
    expect(pumpUntil(fixture, *controller,
                     [&] {
                       return !hasVisibleImportStaging(
                           fixture.roots.visiblePackages);
                     }),
           "reserved disposal drains malformed-inventory staging");
    expect(
        observer->reservedDisposals() == 1 &&
            fixture.operations->catalogSnapshot()->packages.empty(),
        "malformed inventory consumes one reservation and publishes nothing");
  };

  exercise(false, true, "injected incomplete inventory");
  exercise(true, false, "injected missing inventory payload");
}

void testPermanentPublishFailureCleansStagingAndPublishesNothing() {
  Fixture fixture;
  fixture.owner.inventoryReady = false;
  const auto folder = fixture.temp.root() / "PermanentPublishFailure";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "PermanentPublishFailure",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();

  expect(controller->beginFolderImport().accepted,
         "permanent-publish fixture starts folder selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().statusMessage ==
                            "Loading profile inventory…";
                   }),
         "prepared package waits before publication");

  const auto blockedDestination =
      fixture.roots.visiblePackages / "PermanentPublishFailure";
  writeText(blockedDestination, "blocked by deterministic test");
  fixture.owner.inventoryReady = true;
  expect(
      pumpUntil(fixture, *controller,
                [&] {
                  return controller->snapshot().state ==
                             GameplaySkinSettingsState::Error &&
                         controller->snapshot().statusMessage ==
                             "visible skin package destination cannot be "
                             "inspected";
                }),
      "unavailable destination produces a permanent publication failure");

  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return !hasVisibleImportStaging(
                         fixture.roots.visiblePackages);
                   }),
         "terminal publication failure leaves no prepared staging");
  expect(fixture.operations->catalogSnapshot()->packages.empty(),
         "terminal publication failure exposes no partial catalog package");
  std::error_code storageError;
  fs::remove(blockedDestination, storageError);
}

void testEditableConfigurationValidationAndDiagnosticProjection() {
  Fixture fixture;
  auto controller = fixture.makeController();
  const SkinEntryId unknown{
      .package = {.directoryName = "Unknown", .collisionKey = "unknown"},
      .packageRelativePath = "play/play7.luaskin",
      .collisionKey = "unknown/play/play7.luaskin"};
  expect(!controller->select(unknown).accepted,
         "selection rejects an entry absent from the validated catalog");
  expect(!controller->setCompatibilityEnabled(true).accepted,
         "enablement rejects a profile without a validated selection");

  const auto folder = fixture.temp.root() / "EditableConfiguration";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(
      picked(folder, "OriginalName", PlatformTemporaryPathKind::Directory));
  expect(controller->beginFolderImport().accepted,
         "editable-configuration fixture starts folder selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready &&
                            !controller->snapshot().entries.empty();
                   }),
         "automatic package publishes successfully");
  const SkinEntryId entry = controller->snapshot().entries.front().entry;
  expect(entry.package.directoryName == "OriginalName",
         "publication uses the source-derived package name without editing");

  expect(
      controller->setFileChoice(entry, "Judge image", "judge/2p.png").accepted,
      "file-choice edit enters activation validation");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     const auto settings =
                         fixture.owner.snapshot(fixture.profileA).settings;
                     const auto configured = settings.entries.find(entry);
                     if (configured == settings.entries.end()) {
                       return false;
                     }
                     const auto file =
                         configured->second.filePaths.find("Judge image");
                     return file != configured->second.filePaths.end() &&
                            file->second == "judge/2p.png" &&
                            controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready;
                   }),
         "validated file choice persists durably");
  const ConfigOffset offset{.x = 12, .y = -8, .w = 4, .h = -2, .r = 3, .a = 7};
  expect(controller->setOffset(entry, "Judge position", offset).accepted,
         "offset edit enters activation validation");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     const auto settings =
                         fixture.owner.snapshot(fixture.profileA).settings;
                     const auto configured = settings.entries.find(entry);
                     if (configured == settings.entries.end()) {
                       return false;
                     }
                     const auto savedOffset =
                         configured->second.offsets.find("Judge position");
                     return savedOffset != configured->second.offsets.end() &&
                            savedOffset->second == offset &&
                            controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready;
                   }),
         "validated offset persists durably");

  auto catalogWithDiagnostic = std::make_shared<SkinPackageCatalogSnapshot>(
      *fixture.operations->catalogSnapshot());
  catalogWithDiagnostic->entries.front().diagnostics.push_back(
      {.code = "controller.test.current",
       .message = "current validation warning",
       .virtualPath = "play/play7.luaskin",
       .severity = DiagnosticSeverity::Warning,
       .source = SkinSourceLocation{
           .virtualPath = "play/play7.luaskin", .line = 17, .column = 3}});
  fixture.catalogOverride = std::move(catalogWithDiagnostic);
  fixture.history.append(
      {.entry = entry,
       .revisionDigest = controller->snapshot().entries.front().revisionDigest,
       .configurationDigest =
           controller->snapshot().entries.front().configurationDigest,
       .phase = SkinDiagnosticPhase::Session,
       .diagnostic = {.code = "controller.test.history",
                      .message = "historical session warning",
                      .virtualPath = "play/play7.luaskin",
                      .severity = DiagnosticSeverity::Warning},
       .luaLine = 29,
       .frameSerial = 41});
  controller->poll();
  const auto &snapshot = controller->snapshot();
  expect(snapshot.entries.front().diagnostics.size() == 1 &&
             snapshot.entries.front().diagnostics.front().source &&
             snapshot.entries.front().diagnostics.front().source->line == 17,
         "current catalog diagnostic preserves its Lua source location");
  expect(snapshot.history.size() == 1 &&
             snapshot.history.front().luaLine == 29 &&
             snapshot.history.front().frameSerial == 41,
         "bounded diagnostic history remains distinct with Lua/frame context");
}

void testCloseDetachesAcceptedProfileAndActivationCommitsDurably() {
  {
    Fixture fixture;
    const auto folder = fixture.temp.root() / "CloseProfileCommit";
    writeText(folder / "play/play7.luaskin", "return { type = 0 }");
    fixture.folderResults.push_back(picked(
        folder, "CloseProfileCommit", PlatformTemporaryPathKind::Directory));
    auto controller = fixture.makeController();
    installQueuedImport(fixture, *controller, false);
    const SkinEntryId entry = controller->snapshot().entries.front().entry;
    fixture.owner.commitsReady = false;
    const int acknowledgementsBefore = fixture.owner.acknowledgedCommits;
    expect(controller
               ->setViewport(entry,
                             ViewportSettings{.mode = ViewportMode::Stretch})
               .accepted,
           "close-race fixture accepts a profile-only commit");
    const auto closeStarted = std::chrono::steady_clock::now();
    controller->close();
    expect(std::chrono::steady_clock::now() - closeStarted <
               std::chrono::milliseconds(100),
           "close detaches an accepted profile commit without waiting");
    fixture.owner.commitsReady = true;
    for (int attempt = 0;
         attempt < 1'000 &&
         fixture.owner.acknowledgedCommits == acknowledgementsBefore;
         ++attempt) {
      fixture.commits.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(fixture.owner.acknowledgedCommits == acknowledgementsBefore + 1 &&
               fixture.owner.snapshot(fixture.profileA)
                       .settings.entries.at(entry)
                       .viewport.mode == ViewportMode::Stretch,
           "detached profile commit reaches durable acknowledgement");
  }

  Fixture fixture;
  const auto folder = fixture.temp.root() / "CloseActivationCommit";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "CloseActivationCommit",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;
  fixture.owner.commitsReady = false;
  const int acknowledgementsBefore = fixture.owner.acknowledgedCommits;
  expect(controller->select(entry).accepted,
         "close-race fixture starts activation preparation");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                .settings.selected7KeyEntry == entry;
                   }),
         "activation reaches an accepted durable commit before close");
  const auto closeStarted = std::chrono::steady_clock::now();
  controller->close();
  expect(std::chrono::steady_clock::now() - closeStarted <
             std::chrono::milliseconds(100),
         "close detaches an accepted activation commit without waiting");
  fixture.owner.commitsReady = true;
  for (int attempt = 0; attempt < 1'000 && fixture.owner.acknowledgedCommits ==
                                               acknowledgementsBefore;
       ++attempt) {
    fixture.commits.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(
      fixture.owner.acknowledgedCommits == acknowledgementsBefore + 1 &&
          fixture.owner.snapshot(fixture.profileA).settings.selected7KeyEntry ==
              entry,
      "detached activation commit reaches durable acknowledgement");
}

void testInvalidAndStaleActivationRetainPreviousProfileSettings() {
  Fixture fixture;
  const auto folder = fixture.temp.root() / "ActivationFailures";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "ActivationFailures",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;
  expect(controller->select(entry).accepted,
         "activation-failure fixture establishes a valid selection");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                    .settings.selected7KeyEntry == entry &&
                            controller->snapshot().state ==
                                GameplaySkinSettingsState::Ready;
                   }),
         "activation-failure fixture commits its baseline activation");
  const auto baseline = fixture.owner.snapshot(fixture.profileA).settings;

  fixture.validator.invalidConfiguration = true;
  expect(controller->setOption(entry, "Play Side", 921).accepted,
         "invalid configuration still enters asynchronous validation");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                            GameplaySkinSettingsState::Error;
                   }),
         "invalid activation yields a typed controller error");
  expect(fixture.owner.snapshot(fixture.profileA).settings == baseline,
         "invalid configuration never overwrites last valid profile settings");

  fixture.validator.invalidConfiguration = false;
  fixture.owner.rejectNextCommitGeneration = true;
  expect(controller->setOption(entry, "Play Side", 921).accepted,
         "stale-generation fixture enters activation preparation");
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return controller->snapshot().state ==
                            GameplaySkinSettingsState::Error;
                   }),
         "generation-changed activation becomes a terminal controller error");
  expect(fixture.owner.snapshot(fixture.profileA).settings == baseline,
         "stale activation retains the prior durable profile settings");
}

void testLifecycleCallbacksCustomViewportAndRemoval() {
  Fixture fixture;
  const auto folder = fixture.temp.root() / "LifecycleAndRemoval";
  writeText(folder / "play/play7.luaskin", "return { type = 0 }");
  fixture.folderResults.push_back(picked(folder, "LifecycleAndRemoval",
                                         PlatformTemporaryPathKind::Directory));
  auto controller = fixture.makeController();
  installQueuedImport(fixture, *controller, false);
  const SkinEntryId entry = controller->snapshot().entries.front().entry;

  expect(controller->requestRescan().accepted && fixture.rescanRequests == 1 &&
             controller->snapshot().state == GameplaySkinSettingsState::Busy,
         "manual rescans stay busy until the lifecycle reports a terminal state");
  fixture.rescanProgress.phase = SkinRescanProgressPhase::Succeeded;
  controller->poll();
  expect(controller->snapshot().state == GameplaySkinSettingsState::Ready &&
             controller->requestRevalidation(entry).accepted &&
             fixture.revalidationRequests == 1,
         "manual lifecycle actions use only injected rescan/revalidation owners");

  const ViewportSettings excessiveCustom{.mode = ViewportMode::Custom,
                                         .customBase =
                                             CustomViewportBase::Stretch,
                                         .scaleX = 100.0F,
                                         .scaleY = 0.01F,
                                         .translateX = 20'000.0F,
                                         .translateY = -20'000.0F};
  expect(controller->setViewport(entry, excessiveCustom).accepted,
         "Custom viewport enters profile-only persistence");
  const ViewportSettings sanitizedCustom{.mode = ViewportMode::Custom,
                                         .customBase =
                                             CustomViewportBase::Stretch,
                                         .scaleX = 10.0F,
                                         .scaleY = 0.1F,
                                         .translateX = 8'192.0F,
                                         .translateY = -8'192.0F};
  expect(pumpUntil(fixture, *controller,
                   [&] {
                     return fixture.owner.snapshot(fixture.profileA)
                                .settings.entries.at(entry)
                                .viewport == sanitizedCustom;
                   }),
         "Custom viewport values persist with policy bounds applied");

  auto profileB = fixture.owner.snapshot(fixture.profileB);
  profileB.settings.selectedGameplayEntries.emplace(0, entry);
  expect(fixture.owner
                 .beginCommit(fixture.profileB, profileB.generation,
                              profileB.settings)
                 .status == SkinProfileCommitResult::Status::Pending,
         "removal fixture records a selection owned by another profile");

  const auto removal = controller->requestRemoval(entry.package);
  expect(removal.accepted && removal.asynchronous,
         "package removal first checks every profile selection");
  expect(pumpUntil(
             fixture, *controller,
             [&] {
               return controller->snapshot().state !=
                          GameplaySkinSettingsState::Busy;
             }),
         "package-removal selection check reaches a terminal result");
  expect(controller->snapshot().state == GameplaySkinSettingsState::Error &&
             !fixture.operations->catalogSnapshot()->packages.empty(),
         "removal rejects a package selected by any profile and retains its "
         "catalog entry");

  profileB = fixture.owner.snapshot(fixture.profileB);
  profileB.settings.selectedGameplayEntries.clear();
  expect(fixture.owner
                 .beginCommit(fixture.profileB, profileB.generation,
                              profileB.settings)
                 .status == SkinProfileCommitResult::Status::Pending &&
             controller->requestRemoval(entry.package).accepted,
         "removal retries after every profile has cleared the package");
  expect(pumpUntil(
             fixture, *controller,
             [&] {
               return controller->snapshot().state ==
                          GameplaySkinSettingsState::Empty &&
                      fixture.operations->catalogSnapshot()->packages.empty();
             }),
         "unreferenced package removal refreshes the immutable catalog "
         "snapshot");
}

} // namespace

int main() {
  testSourceNameSuggestionPreservesTypedSemantics();
  testSnapshotUsesCachedSettingsProjectionUntilTheNextPoll();
  testArchiveFolderSelectionAndDurableLayoutFlow();
  testPickedArchiveAndFolderInstallWithoutPackageNameConfirmation();
  testSelectionIsScopedToTheSkinDeclaredGameplayTrait();
  testRejectedPublishTransfersReservedStagingOffControllerThread();
  testRejectedPrepareTransfersExactTemporaryCleanupOffControllerThread();
  testCancelledPickerTransfersOwnedResultThroughReservedLane();
  testFailedPickerTransfersOwnedResultThroughReservedLane();
  testReservationPrecedesPickerAndCancelTransfersLocalStaging();
  testProfileSwitchDetachesPendingActivationAndUnlocksNewProfile();
  testProfileSwitchDetachesPendingProfileSaveAndUnlocksNewProfile();
  testCollisionRejectRetainsSourceAndReplacePublishesAtomically();
  testInventoryRaceRetriesThenSucceedsAndCancelDisposesRetry();
  testMalformedProfileInventoriesDisposePreparedStagingOnReservedLane();
  testPermanentPublishFailureCleansStagingAndPublishesNothing();
  testEditableConfigurationValidationAndDiagnosticProjection();
  testCloseDetachesAcceptedProfileAndActivationCommitsDurably();
  testInvalidAndStaleActivationRetainPreviousProfileSettings();
  testLifecycleCallbacksCustomViewportAndRemoval();
  if (failures == 0) {
    std::cout << "gameplay skin settings tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
