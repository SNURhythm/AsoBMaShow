#include "skin/GameplaySkinLifecycle.h"

#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace skin;

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto base = fs::temp_directory_path();
    for (int attempt = 0; attempt != 1'000; ++attempt) {
      path_ = base / ("asobmashow-lifecycle-" + std::to_string(attempt) + "-" +
                      std::to_string(std::rand()));
      std::error_code error;
      if (fs::create_directory(path_, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create lifecycle test directory");
  }
  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

struct PendingPrepare {
  std::uint64_t ticket = 0;
  VersionedSkinProfileSettings base;
  SkinEntryId entry;
  SkinProfileSettings candidate;
};

class LifecycleFake {
public:
  LifecycleFake()
      : roots{.visiblePackages = temp.path() / "visible",
              .privateRevisions = temp.path() / "revisions",
              .privateCatalog = temp.path() / "catalog",
              .profileOverlays = temp.path() / "overlays"},
        profile(*makeSkinProfileId("99999999-9999-4999-8999-999999999999")),
        package(*normalizePackageId("LifecycleSkin").package),
        entry(*normalizeEntryPath(package, "skin/main.luaskin").entry) {
    fs::create_directories(temp.path() / "source/skin");
    std::ofstream(temp.path() / "source/skin/main.luaskin")
        << "return { type = 0 }\n";
    SkinTreeSnapshotter snapshotter(roots, aliases);
    auto first = snapshotter.snapshot(temp.path() / "source", package, {}, {});
    require(first.prepared.has_value(), "first lifecycle revision snapshots");
    std::string error;
    firstLease = std::move(*first.prepared).publish(error);
    require(firstLease.has_value(), "first lifecycle revision publishes");

    std::ofstream(temp.path() / "source/skin/extra.txt") << "revision two\n";
    auto second = snapshotter.snapshot(temp.path() / "source", package, {}, {});
    require(second.prepared.has_value(), "second lifecycle revision snapshots");
    secondLease = std::move(*second.prepared).publish(error);
    require(secondLease.has_value(), "second lifecycle revision publishes");

    currentLease = &*firstLease;
    owner.profileId = profile;
    owner.generation = 5;
    owner.settings.gameplayCompatibilityEnabled = true;
    owner.settings.selected7KeyEntry = entry;
    owner.settings.entries[entry].options["choice"] = 0;
    activeEntrySettings = owner.settings.entries.at(entry);
    catalog = std::make_shared<SkinPackageCatalogSnapshot>();
  }

  GameplaySkinLifecycleDependencies dependencies() {
    return {
        .roots = roots,
        .ensureVisibleRoot =
            [this] {
              ++ensureRootCalls;
              return true;
            },
        .snapshotProfile =
            [this](const SkinProfileId &requested) {
              require(requested == profile,
                      "lifecycle snapshots the active profile");
              return owner;
            },
        .acquireActivation =
            [this](const SkinProfileId &requested, const SkinEntryId &selected,
                   std::string_view digest) {
              ++acquireCalls;
              operationEvents.emplace_back("acquire");
              AcquireActivationResult result;
              if (requested != profile || selected != entry || !currentLease ||
                  digest != skinConfigurationDigest(activeEntrySettings)) {
                return result;
              }
              result.activation.emplace(ValidatedSkinActivation{
                  .revision = currentLease->clone(),
                  .entry = entry,
                  .reconciledSettings = activeEntrySettings,
                  .configurationDigest = std::string(digest)});
              return result;
            },
        .submitPrepareActivation =
            [this](VersionedSkinProfileSettings base, SkinEntryId selected,
                   SkinProfileSettings candidate) {
              const auto ticket = ++nextOperationTicket;
              prepares.push_back(
                  PendingPrepare{.ticket = ticket,
                                 .base = std::move(base),
                                 .entry = std::move(selected),
                                 .candidate = std::move(candidate)});
              return GameplaySkinLifecycleOperationSubmission{.ticket = ticket};
            },
        .submitReconcileProfileActivations =
            [this](std::vector<SkinProfileId> profiles) {
              operationEvents.emplace_back("reconcile");
              reconciledProfiles.push_back(std::move(profiles));
              if (rejectReconcile) {
                return GameplaySkinLifecycleOperationSubmission{};
              }
              const auto ticket = ++nextOperationTicket;
              reconcileTickets.push_back(ticket);
              return GameplaySkinLifecycleOperationSubmission{.ticket = ticket};
            },
        .submitRescan =
            [this](ProfileInventorySnapshot inventory) {
              operationEvents.emplace_back("rescan");
              rescannedInventories.push_back(std::move(inventory));
              if (rejectRescan) {
                return GameplaySkinLifecycleOperationSubmission{};
              }
              const auto ticket = ++nextOperationTicket;
              rescanTickets.push_back(ticket);
              return GameplaySkinLifecycleOperationSubmission{.ticket = ticket};
            },
        .pollOperation = [this](std::uint64_t ticket)
            -> std::optional<GameplaySkinLifecycleOperationCompletion> {
          const auto found = operationCompletions.find(ticket);
          if (found == operationCompletions.end()) {
            return std::nullopt;
          }
          auto completion = std::move(found->second);
          operationCompletions.erase(found);
          return completion;
        },
        .cancelOperation =
            [this](std::uint64_t ticket) {
              shutdownEvents.emplace_back("cancel");
              cancelledOperations.push_back(ticket);
              if (throwOnCancel) {
                throw std::runtime_error("injected cancellation failure");
              }
            },
        .beginProfileInventory =
            [this] {
              operationEvents.emplace_back("inventory");
              return rejectInventory ? 0 : ++nextInventoryTicket;
            },
        .pollProfileInventory =
            [this](std::uint64_t ticket)
            -> std::optional<AllSkinProfileSnapshotsResult> {
          const auto found = inventoryCompletions.find(ticket);
          if (found == inventoryCompletions.end()) {
            return std::nullopt;
          }
          auto result = std::move(found->second);
          inventoryCompletions.erase(found);
          return result;
        },
        .cancelProfileInventory =
            [this](std::uint64_t ticket) {
              cancelledInventories.push_back(ticket);
            },
        .submitActivation =
            [this](PreparedSkinActivation prepared) {
              const auto ticket = ++nextCommitTicket;
              ++owner.generation;
              owner.settings = prepared.candidateProfileSettings;
              pendingCommits.emplace(ticket, std::move(prepared));
              return SkinActivationSubmissionResult{.accepted = true,
                                                    .ticket = ticket};
            },
        .takeActivationCompletions =
            [this] { return std::exchange(activationCompletions, {}); },
        .takeRevalidationRequests =
            [] { return std::vector<VersionedSkinProfileSettings>{}; },
        .submitProfileSettings =
            [this](const VersionedSkinProfileSettings &base,
                   SkinProfileSettings candidate) {
              const auto ticket = ++nextProfileTicket;
              ++owner.generation;
              owner.settings = candidate;
              pendingProfiles.emplace(ticket,
                                      std::pair{base, std::move(candidate)});
              return SkinProfileCommitSubmissionResult{.accepted = true,
                                                       .ticket = ticket};
            },
        .takeProfileCompletions =
            [this] { return std::exchange(profileCompletions, {}); },
        .drainConfigurationWrites =
            [this] {
              shutdownEvents.emplace_back("drain");
              if (throwOnDrain) {
                throw std::runtime_error("injected drain failure");
              }
              return std::exchange(writes, {});
            },
        .closeConfigurationWrites =
            [this] {
              shutdownEvents.emplace_back("close");
              ++closeWriteCalls;
            },
        .catalogSnapshot = [this] { return catalog; },
        .detachCommitClient =
            [this] {
              shutdownEvents.emplace_back("detach");
              ++detachCalls;
            },
        .appendHistory =
            [this](SkinDiagnosticHistoryRecord record) {
              diagnostics.push_back(std::move(record));
            },
    };
  }

  void completePrepare(std::size_t index = 0) {
    require(index < prepares.size(), "prepare completion exists");
    PendingPrepare pending = std::move(prepares[index]);
    prepares.erase(prepares.begin() + static_cast<std::ptrdiff_t>(index));
    const auto &settings = pending.candidate.entries.at(entry);
    PrepareActivationResult result;
    result.prepared.emplace(PreparedSkinActivation{
        .sourceGeneration = 10,
        .catalogGeneration = 20,
        .expectedProfileGeneration = pending.base.generation,
        .profileId = profile,
        .activation = {.revision = currentLease->clone(),
                       .entry = entry,
                       .reconciledSettings = settings,
                       .configurationDigest =
                           skinConfigurationDigest(settings)},
        .candidateProfileSettings = pending.candidate});
    operationCompletions.emplace(
        pending.ticket,
        GameplaySkinLifecycleOperationCompletion{.ticket = pending.ticket,
                                                 .payload = std::move(result)});
  }

  void completeInventory(bool complete = true, bool cancelled = false,
                         bool includeInventory = true) {
    require(nextInventoryTicket != 0, "inventory request exists");
    AllSkinProfileSnapshotsResult result{.complete = complete,
                                         .cancelled = cancelled};
    if (includeInventory) {
      result.inventory.emplace(ProfileInventorySnapshot{
          .inventoryGeneration = 41, .profiles = {owner}});
    }
    inventoryCompletions.insert_or_assign(nextInventoryTicket,
                                          std::move(result));
  }

  void completeReconcile(bool completed = true) {
    require(!reconcileTickets.empty(), "reconcile request exists");
    const auto ticket = reconcileTickets.front();
    reconcileTickets.erase(reconcileTickets.begin());
    ReconcileProfileActivationsResult result{.completed = completed};
    operationCompletions.emplace(
        ticket,
        GameplaySkinLifecycleOperationCompletion{.ticket = ticket,
                                                 .payload = std::move(result)});
  }

  void completeRescan(bool succeeded = true, bool cancelled = false) {
    require(!rescanTickets.empty(), "rescan request exists");
    const auto ticket = rescanTickets.front();
    rescanTickets.erase(rescanTickets.begin());
    ScanPackagesResult result{.cancelled = cancelled,
                              .retryableInventoryRace = !succeeded,
                              .sourceGeneration = succeeded ? 52U : 0U};
    operationCompletions.emplace(
        ticket,
        GameplaySkinLifecycleOperationCompletion{.ticket = ticket,
                                                 .payload = std::move(result)});
  }

  void completeReadiness(GameplaySkinLifecycle &lifecycle) {
    lifecycle.poll();
    completeInventory();
    lifecycle.poll();
    completeReconcile();
    lifecycle.poll();
    completeRescan();
    const bool compatibility = owner.settings.gameplayCompatibilityEnabled;
    owner.settings.gameplayCompatibilityEnabled = false;
    lifecycle.poll();
    owner.settings.gameplayCompatibilityEnabled = compatibility;
  }

  bool hasDiagnostic(std::string_view code) const {
    return std::ranges::any_of(
        diagnostics, [code](const SkinDiagnosticHistoryRecord &record) {
          return record.diagnostic.code == code;
        });
  }

  void completeActivation(std::uint64_t ticket,
                          const SkinRevisionLease *resultLease = nullptr,
                          bool mismatchConfiguration = false) {
    const auto found = pendingCommits.find(ticket);
    require(found != pendingCommits.end(), "activation commit exists");
    auto prepared = std::move(found->second);
    pendingCommits.erase(found);
    activeEntrySettings = prepared.activation.reconciledSettings;
    if (resultLease) {
      prepared.activation.revision = resultLease->clone();
    }
    if (mismatchConfiguration) {
      prepared.activation.reconciledSettings.options["choice"] = 99;
      prepared.activation.configurationDigest =
          skinConfigurationDigest(prepared.activation.reconciledSettings);
    }
    CommitActivationResult result{
        .disposition = ActivationCommitDisposition::ActivatedRequested,
        .ticket = ticket,
        .activation = std::move(prepared.activation),
        .profileSnapshot = owner};
    activationCompletions.push_back(
        {.client = 1, .ticket = ticket, .result = std::move(result)});
  }

  void completeProfile(std::uint64_t ticket) {
    const auto found = pendingProfiles.find(ticket);
    require(found != pendingProfiles.end(), "profile commit exists");
    pendingProfiles.erase(found);
    profileCompletions.push_back(
        {.client = 1,
         .ticket = ticket,
         .result = {.status = SkinProfileCommitResult::Status::Persisted,
                    .ticket = ticket,
                    .snapshot = owner}});
  }

  void advanceOwnerGenerationWithoutChangingSettings() { ++owner.generation; }

  SkinConfigurationWriteRequest
  request(const GameplaySkinActivationRequest &chart, std::uint64_t frame,
          std::vector<PersistedSkinConfigurationWrite> ordered) const {
    return {.sessionSerial = chart.sessionSerial,
            .profileId = chart.profileId,
            .entry = chart.activation.entry,
            .expectedRevisionDigest =
                chart.activation.revision.revision().lowercaseSha256,
            .expectedConfigurationDigest = chart.activation.configurationDigest,
            .frameSerial = frame,
            .orderedWrites = std::move(ordered)};
  }

  TempDirectory temp;
  AcceptFiles aliases;
  SkinStorageRoots roots;
  SkinProfileId profile;
  SkinPackageId package;
  SkinEntryId entry;
  std::optional<SkinRevisionLease> firstLease;
  std::optional<SkinRevisionLease> secondLease;
  SkinRevisionLease *currentLease = nullptr;
  VersionedSkinProfileSettings owner;
  EntryProfileSettings activeEntrySettings;
  std::shared_ptr<SkinPackageCatalogSnapshot> catalog;
  std::vector<PendingPrepare> prepares;
  std::map<std::uint64_t, AllSkinProfileSnapshotsResult> inventoryCompletions;
  std::map<std::uint64_t, GameplaySkinLifecycleOperationCompletion>
      operationCompletions;
  std::map<std::uint64_t, PreparedSkinActivation> pendingCommits;
  std::map<std::uint64_t,
           std::pair<VersionedSkinProfileSettings, SkinProfileSettings>>
      pendingProfiles;
  std::vector<SkinActivationCompletion> activationCompletions;
  std::vector<SkinProfileCommitCompletion> profileCompletions;
  std::vector<SkinConfigurationWriteRequest> writes;
  std::vector<SkinDiagnosticHistoryRecord> diagnostics;
  std::vector<std::uint64_t> cancelledOperations;
  std::vector<std::uint64_t> cancelledInventories;
  std::vector<std::uint64_t> reconcileTickets;
  std::vector<std::uint64_t> rescanTickets;
  std::vector<std::vector<SkinProfileId>> reconciledProfiles;
  std::vector<ProfileInventorySnapshot> rescannedInventories;
  std::vector<std::string> operationEvents;
  std::vector<std::string> shutdownEvents;
  std::uint64_t nextOperationTicket = 0;
  std::uint64_t nextInventoryTicket = 0;
  std::uint64_t nextCommitTicket = 0;
  std::uint64_t nextProfileTicket = 0;
  int ensureRootCalls = 0;
  int closeWriteCalls = 0;
  int detachCalls = 0;
  int acquireCalls = 0;
  bool rejectInventory = false;
  bool rejectReconcile = false;
  bool rejectRescan = false;
  bool throwOnCancel = false;
  bool throwOnDrain = false;
};

void testStartupAcquisitionWaitsForInventoryReconcileAndRescanInOrder() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  require(!lifecycle.acquireForNextChart() && fake.acquireCalls == 0,
          "startup acquisition stays built-in-only before inventory begins");

  lifecycle.poll();
  require(fake.operationEvents == std::vector<std::string>{"inventory"} &&
              !lifecycle.acquireForNextChart() && fake.acquireCalls == 0,
          "inventory admission alone cannot expose acquisition");

  fake.completeInventory();
  lifecycle.poll();
  require(fake.operationEvents ==
                  std::vector<std::string>{"inventory", "reconcile"} &&
              fake.reconciledProfiles.size() == 1 &&
              fake.reconciledProfiles.front() ==
                  std::vector<SkinProfileId>{fake.profile} &&
              fake.rescannedInventories.empty() &&
              !lifecycle.acquireForNextChart() && fake.acquireCalls == 0,
          "a complete inventory submits value-owned typed profile IDs before "
          "any rescan or acquisition");

  fake.completeReconcile();
  lifecycle.poll();
  require(fake.operationEvents ==
                  std::vector<std::string>{"inventory", "reconcile", "rescan"} &&
              fake.rescannedInventories.size() == 1 &&
              fake.rescannedInventories.front().inventoryGeneration == 41 &&
              fake.rescannedInventories.front().profiles ==
                  std::vector<VersionedSkinProfileSettings>{fake.owner} &&
              !lifecycle.acquireForNextChart() && fake.acquireCalls == 0,
          "successful reconciliation submits the same complete inventory to "
          "the serialized rescan while acquisition remains unavailable");

  fake.completeRescan();
  lifecycle.poll();
  require(lifecycle.acquireForNextChart().has_value() &&
              fake.operationEvents.back() == "acquire" &&
              fake.acquireCalls == 1,
          "a successful scan is the first point that enables acquisition");
}

void testStartupReadinessFailuresRemainBuiltInOnlyAndDiagnosed() {
  const auto inventoryFailure = [](bool complete, bool cancelled,
                                   bool includeInventory) {
    LifecycleFake fake;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    fake.completeInventory(complete, cancelled, includeInventory);
    lifecycle.poll();
    return !lifecycle.acquireForNextChart() && fake.acquireCalls == 0 &&
           fake.hasDiagnostic("skin.lifecycle.inventory_failed");
  };
  require(inventoryFailure(false, false, true) &&
              inventoryFailure(true, true, true) &&
              inventoryFailure(true, false, false),
          "incomplete, cancelled, and missing all-profile inventories fail "
          "closed with diagnostics");

  {
    LifecycleFake fake;
    fake.rejectInventory = true;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    require(!lifecycle.acquireForNextChart() &&
                fake.hasDiagnostic("skin.lifecycle.inventory_rejected"),
            "a zero inventory ticket leaves startup unavailable");
  }
  {
    LifecycleFake fake;
    fake.rejectReconcile = true;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    fake.completeInventory();
    lifecycle.poll();
    require(!lifecycle.acquireForNextChart() &&
                fake.hasDiagnostic("skin.lifecycle.reconcile_rejected") &&
                fake.rescannedInventories.empty(),
            "a zero reconcile ticket diagnoses startup without scanning");
  }
  {
    LifecycleFake fake;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    fake.completeInventory();
    lifecycle.poll();
    fake.completeReconcile(false);
    lifecycle.poll();
    require(!lifecycle.acquireForNextChart() &&
                fake.hasDiagnostic("skin.lifecycle.reconcile_failed") &&
                fake.rescannedInventories.empty(),
            "a failed reconciliation cannot advance startup to rescan");
  }
  {
    LifecycleFake fake;
    fake.rejectRescan = true;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    fake.completeInventory();
    lifecycle.poll();
    fake.completeReconcile();
    lifecycle.poll();
    require(!lifecycle.acquireForNextChart() &&
                fake.hasDiagnostic("skin.lifecycle.rescan_rejected"),
            "a zero rescan ticket leaves startup unavailable");
  }
  {
    LifecycleFake fake;
    GameplaySkinLifecycle lifecycle(fake.dependencies());
    lifecycle.startAfterProfileInitialization(fake.profile);
    lifecycle.poll();
    fake.completeInventory();
    lifecycle.poll();
    fake.completeReconcile();
    lifecycle.poll();
    fake.completeRescan(false);
    lifecycle.poll();
    require(!lifecycle.acquireForNextChart() &&
                fake.hasDiagnostic("skin.lifecycle.rescan_failed"),
            "a failed scan leaves startup unavailable and diagnosed");
  }
}

void testLaterFailedRescanPreservesReadyAcquisitionAndCatalog() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  require(lifecycle.acquireForNextChart().has_value(),
          "later-rescan fixture first reaches startup readiness");
  const auto oldCatalog = lifecycle.catalogSnapshot();

  lifecycle.requestRescan(SkinRescanReason::SettingsOpened);
  lifecycle.requestRescan(SkinRescanReason::Explicit);
  lifecycle.poll();
  fake.completeInventory();
  lifecycle.poll();
  fake.completeReconcile();
  lifecycle.poll();
  fake.completeRescan(false);
  lifecycle.poll();

  require(lifecycle.acquireForNextChart().has_value() &&
              lifecycle.catalogSnapshot() == oldCatalog &&
              fake.hasDiagnostic("skin.lifecycle.rescan_failed"),
          "a later failed rescan keeps the last immutable catalog and existing "
          "acquisition available");
}

void testAcquisitionUsesOwningActivationAndMonotonicSessionSerial() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto first = lifecycle.acquireForNextChart();
  require(first && first->sessionSerial != 0 &&
              first->activation.revision.revision().lowercaseSha256 ==
                  fake.firstLease->revision().lowercaseSha256,
          "first chart acquires one owning validated activation");
  const auto firstSerial = first->sessionSerial;
  fake.currentLease = &*fake.secondLease;
  require(first->activation.revision.revision().lowercaseSha256 ==
              fake.firstLease->revision().lowercaseSha256,
          "a running chart remains pinned when the desired revision changes");
  auto second = lifecycle.acquireForNextChart();
  require(second && second->sessionSerial > firstSerial &&
              second->activation.revision.revision().lowercaseSha256 ==
                  fake.secondLease->revision().lowercaseSha256,
          "the next chart receives a never-reused serial and new owning lease");
}

void testWriterChainRebasesBOnlyAfterASuccess() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "writer fixture acquires chart");
  fake.writes.push_back(
      fake.request(*chart, 10,
                   {SetSkinOption{.key = "choice", .value = 1},
                    SetSkinOption{.key = "choice", .value = 2}}));
  lifecycle.poll();
  require(fake.prepares.size() == 1 &&
              fake.prepares.front().base.generation == 5 &&
              fake.prepares.front()
                      .candidate.entries.at(fake.entry)
                      .options.at("choice") == 2,
          "same-frame authored writes fold in order into one prepare");

  fake.writes.push_back(
      fake.request(*chart, 11, {SetSkinOption{.key = "choice", .value = 3}}));
  lifecycle.poll();
  require(fake.prepares.size() == 1,
          "B remains lifecycle-owned while A preparation is pending");
  fake.completePrepare();
  lifecycle.poll();
  require(fake.pendingCommits.size() == 1,
          "A preparation submits exactly one durable activation commit");
  const auto aTicket = fake.pendingCommits.begin()->first;
  lifecycle.poll();
  require(fake.prepares.empty(), "B does not bypass A's pending commit");
  fake.completeActivation(aTicket);
  lifecycle.poll();
  require(fake.prepares.size() == 1 &&
              fake.prepares.front().base.generation == 6 &&
              fake.prepares.front()
                      .candidate.entries.at(fake.entry)
                      .options.at("choice") == 3,
          "B rebases only onto A's exact successor snapshot");
}

void testWriterRejectsAPreparedActivationFromADifferentRevision() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "writer revision fixture acquires chart");
  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  require(fake.prepares.size() == 1,
          "writer revision fixture starts one preparation");

  fake.currentLease = &*fake.secondLease;
  fake.completePrepare();
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code ==
               "skin.lifecycle.writer_prepared_identity_stale";
      });
  require(fake.pendingCommits.empty() && diagnosed,
          "a source-revision successor cannot consume an old session writer");
}

void testWriterRejectsACommitCompletionThatIsNotItsExactSuccessor() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "writer completion fixture acquires chart");
  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  fake.completePrepare();
  lifecycle.poll();
  const auto ticket = fake.pendingCommits.begin()->first;

  fake.completeActivation(ticket, &*fake.secondLease);
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code == "skin.lifecycle.writer_commit_failed";
      });
  require(diagnosed,
          "writer commit completion must match its exact prepared successor");
}

void testWriterRejectsANonDirectOwnerSuccessorGeneration() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "writer generation fixture acquires chart");
  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  fake.completePrepare();
  lifecycle.poll();
  const auto ticket = fake.pendingCommits.begin()->first;

  fake.advanceOwnerGenerationWithoutChangingSettings();
  fake.completeActivation(ticket);
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code == "skin.lifecycle.writer_commit_failed";
      });
  require(diagnosed,
          "writer completion rejects a later equal-settings owner generation");
}

void testWriterRejectsMismatchedSuccessorConfiguration() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "writer configuration fixture acquires chart");
  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  fake.completePrepare();
  lifecycle.poll();
  const auto ticket = fake.pendingCommits.begin()->first;

  fake.completeActivation(ticket, nullptr, true);
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code == "skin.lifecycle.writer_commit_failed";
      });
  require(diagnosed,
          "writer completion rejects mismatched reconciled settings and digest");
}

void testStaleSessionIsDiagnosedAndCannotEnterWriterChain() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "stale fixture acquires chart");
  auto stale =
      fake.request(*chart, 10, {SetSkinOption{.key = "choice", .value = 4}});
  ++stale.sessionSerial;
  fake.writes.push_back(std::move(stale));
  lifecycle.poll();
  require(fake.prepares.empty() && !fake.diagnostics.empty() &&
              fake.owner.settings.entries.at(fake.entry).options.at("choice") ==
                  0,
          "a stale session is discarded without changing desired settings");
}

void testWriterIngressIsBoundedPerSession() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "bounded writer fixture acquires chart");
  for (std::uint64_t frame = 1;
       frame <= GameplaySkinLifecycle::maxPendingSessionWrites + 1; ++frame) {
    fake.writes.push_back(fake.request(
        *chart, frame,
        {SetSkinOption{.key = "choice", .value = static_cast<int>(frame)}}));
  }
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code == "skin.lifecycle.writer_fifo_full";
      });
  require(fake.prepares.size() == 1 && diagnosed,
          "writer ingress keeps a bounded FIFO and diagnoses overflow");
}

void testDisabledNextChartClearsThePreviousSessionIdentity() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "disable fixture acquires its original chart");
  PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};
  fake.owner.settings.gameplayCompatibilityEnabled = false;
  ++fake.owner.generation;
  require(!lifecycle.acquireForNextChart() &&
              lifecycle.requestViewportReset(identity, {}).disposition ==
                  GameplayViewportPersistenceDisposition::Rejected,
          "a built-in-only next chart cannot reuse the preceding identity");
}

void testViewportResetValidatesAllIdentityFieldsAndCoalescesLatest() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "viewport fixture acquires chart");
  PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};
  const ViewportSettings stretch{.mode = ViewportMode::Stretch};
  const ViewportSettings custom{
      .mode = ViewportMode::Custom, .scaleX = 1.25F, .scaleY = 1.25F};
  require(lifecycle.requestViewportReset(identity, stretch).disposition ==
                  GameplayViewportPersistenceDisposition::Queued &&
              lifecycle.requestViewportReset(identity, custom).disposition ==
                  GameplayViewportPersistenceDisposition::Deferred &&
              fake.pendingProfiles.size() == 1,
          "one viewport commit is queued and a latest value coalesces");
  const auto ticket = fake.pendingProfiles.begin()->first;
  fake.completeProfile(ticket);
  lifecycle.poll();
  require(fake.pendingProfiles.size() == 1 &&
              fake.pendingProfiles.begin()
                      ->second.second.entries.at(fake.entry)
                      .viewport == custom,
          "deferred viewport state retries from the exact persisted successor");
  auto stale = identity;
  stale.revisionDigest += "stale";
  require(lifecycle.requestViewportReset(stale, {}).disposition ==
              GameplayViewportPersistenceDisposition::Rejected,
          "all five stale identity fields fail closed");
}

void testViewportResetRejectsEachStaleIdentityField() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "stale viewport identity fixture acquires chart");
  const PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};

  auto staleSession = identity;
  ++staleSession.sessionSerial;
  auto staleProfile = identity;
  staleProfile.profileId =
      *makeSkinProfileId("88888888-8888-4888-8888-888888888888");
  auto staleEntry = identity;
  staleEntry.entry =
      *normalizeEntryPath(fake.package, "skin/other.luaskin").entry;
  auto staleRevision = identity;
  staleRevision.revisionDigest += "stale";
  auto staleConfiguration = identity;
  staleConfiguration.configurationDigest += "stale";

  const auto rejected = [&](const PlaySkinSessionIdentity &candidate) {
    return lifecycle.requestViewportReset(candidate, {}).disposition ==
           GameplayViewportPersistenceDisposition::Rejected;
  };
  require(rejected(staleSession) && rejected(staleProfile) &&
              rejected(staleEntry) && rejected(staleRevision) &&
              rejected(staleConfiguration) && fake.pendingProfiles.empty(),
          "session, profile, entry, revision, and configuration staleness each "
          "reject viewport persistence");
}

void testRevalidationWaitsForViewportCommit() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "viewport revalidation fixture acquires chart");
  const PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};
  lifecycle.requestRevalidation(fake.entry);
  require(lifecycle
                  .requestViewportReset(identity,
                                        {.mode = ViewportMode::Stretch})
                  .disposition ==
              GameplayViewportPersistenceDisposition::Queued,
          "viewport owner commit is admitted while revalidation is queued");
  lifecycle.poll();
  require(fake.prepares.empty(),
          "revalidation preparation waits for viewport persistence");

  const auto ticket = fake.pendingProfiles.begin()->first;
  fake.completeProfile(ticket);
  lifecycle.poll();
  require(fake.prepares.size() == 1 &&
              fake.prepares.front().base.generation == 6 &&
              fake.prepares.front()
                      .base.settings.entries.at(fake.entry)
                      .viewport.mode == ViewportMode::Stretch,
          "revalidation starts from the completed viewport successor");
}

void testViewportRejectsANonDirectOwnerSuccessorGeneration() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "viewport generation fixture acquires chart");
  const PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};
  require(lifecycle
                  .requestViewportReset(identity,
                                        {.mode = ViewportMode::Stretch})
                  .disposition ==
              GameplayViewportPersistenceDisposition::Queued,
          "viewport generation fixture admits persistence");
  const auto ticket = fake.pendingProfiles.begin()->first;

  fake.advanceOwnerGenerationWithoutChangingSettings();
  fake.completeProfile(ticket);
  lifecycle.poll();
  const bool diagnosed = std::ranges::any_of(
      fake.diagnostics, [](const SkinDiagnosticHistoryRecord &record) {
        return record.diagnostic.code ==
               "skin.lifecycle.viewport_successor_mismatch";
      });
  require(diagnosed,
          "viewport completion rejects a later equal-settings owner generation");
}

void testWriterWaitsForViewportCommitAndRebasesOntoItsSuccessor() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "viewport-first writer fixture acquires chart");
  PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};

  require(lifecycle
                  .requestViewportReset(identity,
                                        {.mode = ViewportMode::Stretch})
                  .disposition ==
              GameplayViewportPersistenceDisposition::Queued &&
              fake.pendingProfiles.size() == 1,
          "viewport persistence starts before the writer arrives");
  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  require(fake.prepares.empty(),
          "writer preparation waits for the viewport owner commit");

  const auto viewportTicket = fake.pendingProfiles.begin()->first;
  fake.completeProfile(viewportTicket);
  lifecycle.poll();
  require(fake.prepares.size() == 1 &&
              fake.prepares.front().base.generation == 6 &&
              fake.prepares.front()
                      .base.settings.entries.at(fake.entry)
                      .viewport.mode == ViewportMode::Stretch &&
              fake.prepares.front()
                      .candidate.entries.at(fake.entry)
                      .options.at("choice") == 1 &&
              fake.prepares.front()
                      .candidate.entries.at(fake.entry)
                      .viewport.mode == ViewportMode::Stretch,
          "writer preparation rebases onto the exact viewport successor");
}

void testDeferredViewportWaitsForTheCompleteWriterChain() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  fake.completeReadiness(lifecycle);
  auto chart = lifecycle.acquireForNextChart();
  require(chart.has_value(), "viewport ordering fixture acquires chart");
  PlaySkinSessionIdentity identity{
      .sessionSerial = chart->sessionSerial,
      .profileId = chart->profileId,
      .entry = chart->activation.entry,
      .revisionDigest = chart->activation.revision.revision().lowercaseSha256,
      .configurationDigest = chart->activation.configurationDigest};

  fake.writes.push_back(
      fake.request(*chart, 1, {SetSkinOption{.key = "choice", .value = 1}}));
  lifecycle.poll();
  require(fake.prepares.size() == 1,
          "writer transaction A starts before viewport persistence");

  fake.writes.push_back(
      fake.request(*chart, 2, {SetSkinOption{.key = "choice", .value = 2}}));
  lifecycle.poll();
  fake.completePrepare();
  lifecycle.poll();
  require(
      lifecycle.requestViewportReset(identity, {.mode = ViewportMode::Stretch})
              .disposition == GameplayViewportPersistenceDisposition::Deferred,
      "viewport persistence defers while A's owner successor is pending");
  const auto aTicket = fake.pendingCommits.begin()->first;
  fake.completeActivation(aTicket);
  lifecycle.poll();
  require(fake.prepares.size() == 1 && fake.pendingProfiles.empty(),
          "queued writer B starts before deferred viewport persistence");

  fake.completePrepare();
  lifecycle.poll();
  const auto bTicket = fake.pendingCommits.begin()->first;
  fake.completeActivation(bTicket);
  lifecycle.poll();
  require(fake.pendingProfiles.size() == 1 &&
              fake.pendingProfiles.begin()
                      ->second.second.entries.at(fake.entry)
                      .options.at("choice") == 2 &&
              fake.pendingProfiles.begin()
                      ->second.second.entries.at(fake.entry)
                      .viewport.mode == ViewportMode::Stretch,
          "deferred viewport persistence starts from the writer chain's exact "
          "digest-changing successor");
}

void testShutdownCancelsOwnedWorkAndClosesProducersOnce() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  lifecycle.requestRevalidation(fake.entry);
  lifecycle.poll();
  require(!fake.prepares.empty(), "shutdown fixture owns prepare work");
  lifecycle.shutdown();
  lifecycle.shutdown();
  require(
      !fake.cancelledOperations.empty() && fake.closeWriteCalls == 1 &&
          fake.detachCalls == 1,
      "idempotent shutdown cancels operations, closes writes, and detaches");
}

void testShutdownClosesBeforeDrainAndIsolatesCleanupFailures() {
  LifecycleFake fake;
  GameplaySkinLifecycle lifecycle(fake.dependencies());
  lifecycle.startAfterProfileInitialization(fake.profile);
  lifecycle.requestRevalidation(fake.entry);
  lifecycle.poll();
  require(!fake.prepares.empty(), "faulted shutdown fixture owns prepare work");
  fake.shutdownEvents.clear();
  fake.throwOnCancel = true;
  fake.throwOnDrain = true;

  lifecycle.shutdown();
  lifecycle.shutdown();
  require(fake.shutdownEvents ==
                  std::vector<std::string>{"cancel", "close", "drain",
                                           "detach"} &&
              fake.closeWriteCalls == 1 && fake.detachCalls == 1,
          "shutdown closes writer ingress before final drain and independently "
          "attempts every cleanup phase");
}

} // namespace

int main() {
  testStartupAcquisitionWaitsForInventoryReconcileAndRescanInOrder();
  testStartupReadinessFailuresRemainBuiltInOnlyAndDiagnosed();
  testLaterFailedRescanPreservesReadyAcquisitionAndCatalog();
  testAcquisitionUsesOwningActivationAndMonotonicSessionSerial();
  testWriterChainRebasesBOnlyAfterASuccess();
  testWriterRejectsAPreparedActivationFromADifferentRevision();
  testWriterRejectsACommitCompletionThatIsNotItsExactSuccessor();
  testWriterRejectsANonDirectOwnerSuccessorGeneration();
  testWriterRejectsMismatchedSuccessorConfiguration();
  testStaleSessionIsDiagnosedAndCannotEnterWriterChain();
  testWriterIngressIsBoundedPerSession();
  testDisabledNextChartClearsThePreviousSessionIdentity();
  testWriterWaitsForViewportCommitAndRebasesOntoItsSuccessor();
  testViewportResetValidatesAllIdentityFieldsAndCoalescesLatest();
  testViewportResetRejectsEachStaleIdentityField();
  testRevalidationWaitsForViewportCommit();
  testViewportRejectsANonDirectOwnerSuccessorGeneration();
  testDeferredViewportWaitsForTheCompleteWriterChain();
  testShutdownCancelsOwnedWorkAndClosesProducersOnce();
  testShutdownClosesBeforeDrainAndIsolatesCleanupFailures();
  std::cout << "gameplay skin lifecycle tests passed\n";
  return 0;
}
