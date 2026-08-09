#include "../src/AppSettingsStore.h"
#include "../src/PlayerProfileManager.h"
#include "../src/ProfileSettingsPersistenceCoordinator.h"
#include "../src/skin/package/SkinPathPolicy.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("asobmashow-skin-profile-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

skin::SkinEntryId sampleEntry(std::string_view packageName = "Pack") {
  const auto package = skin::normalizePackageId(packageName);
  return *skin::normalizeEntryPath(*package.package, "play/main.luaskin").entry;
}

skin::SkinProfileSettings selectedSettings(int option) {
  skin::SkinProfileSettings settings;
  const auto entry = sampleEntry();
  settings.gameplayCompatibilityEnabled = true;
  settings.selected7KeyEntry = entry;
  settings.entries[entry].options["variant"] = option;
  return settings;
}

PlayerProfileManagerDependencies managerDependencies() {
  auto ids = std::make_shared<std::vector<std::string>>(
      std::initializer_list<std::string>{
          "11111111-1111-4111-8111-111111111111",
          "22222222-2222-4222-8222-222222222222"});
  auto index = std::make_shared<std::size_t>(0);
  PlayerProfileManagerDependencies dependencies;
  dependencies.generateUuid = [ids, index] { return ids->at((*index)++); };
  dependencies.utcNow = [] { return "2026-08-03T12:00:00Z"; };
  return dependencies;
}

struct BlockingStore {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  bool failFirst = false;
  std::size_t failCall = 0;
  std::vector<std::filesystem::path> paths;
  std::vector<AppSettings> candidates;

  bool save(const std::filesystem::path &path, const AppSettings &candidate,
            std::string &error) {
    std::unique_lock lock(mutex);
    entered = true;
    paths.push_back(path);
    candidates.push_back(candidate);
    const std::size_t call = candidates.size();
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    if (failFirst || failCall == call) {
      failFirst = false;
      error = "injected failure";
      return false;
    }
    lock.unlock();
    return AppSettingsStore::Save(path, candidate, error);
  }

  void waitUntilEntered() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return entered; });
  }
  void unblock() {
    std::lock_guard lock(mutex);
    release = true;
    cv.notify_all();
  }
};

class TestSignal {
public:
  void arrive() {
    std::lock_guard lock(mutex_);
    arrived_ = true;
    cv_.notify_all();
  }

  void wait() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return arrived_; });
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool arrived_ = false;
};

class TestBlockingPoint {
public:
  void arriveAndWait() {
    std::unique_lock lock(mutex_);
    arrived_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return released_; });
  }

  void waitUntilArrived() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] { return arrived_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool arrived_ = false;
  bool released_ = false;
};

skin::SkinProfileCommitResult
waitForCommit(ProfileSettingsPersistenceCoordinator &coordinator,
              std::uint64_t ticket) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    auto result = coordinator.pollCommit(ticket);
    if (result.status != skin::SkinProfileCommitResult::Status::Pending) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return {};
}

void testOrdinarySaveBeforeFailedSkinCommitKeepsLatestFullDocumentDurable() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for reverse-order merge");
  const auto second = manager.createProfile("Transaction Blocker");
  expect(second.ok() && second.profile,
         "reverse-order merge creates an inactive blocker profile");
  AppSettings active;
  active.skin = selectedSettings(1);
  active.irProviders["tachi"].enabled = false;
  BlockingStore store;
  store.failCall = 3;
  TestSignal ordinarySaveAdmitted;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic =
           [&](const auto &path, const auto &settings, std::string &error) {
             return store.save(path, settings, error);
           },
       .afterFullSaveAdmitted = [&] { ordinarySaveAdmitted.arrive(); }});
  const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
  const auto snapshotTicket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> inventory;
  for (int attempt = 0; attempt < 500 && !inventory; ++attempt) {
    inventory = coordinator.pollSnapshotAllProfiles(snapshotTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(inventory && inventory->complete,
         "reverse-order merge discovers the inactive blocker profile");
  const auto blockerProfile =
      *skin::makeSkinProfileId(second.profile ? second.profile->id : "");
  const auto blocker = coordinator.beginCommit(
      blockerProfile, coordinator.snapshot(blockerProfile).generation,
      selectedSettings(99));
  store.waitUntilEntered();

  AppSettings ordinary = active;
  ordinary.irProviders["tachi"].enabled = true;
  std::string ordinaryError;
  std::thread ordinarySave([&] {
    expect(
        coordinator.saveActiveSettingsAndWait(profile, ordinary, ordinaryError),
        "ordinary predecessor save succeeds: " + ordinaryError);
  });
  ordinarySaveAdmitted.wait();

  const auto pending = coordinator.beginCommit(
      profile, coordinator.snapshot(profile).generation, selectedSettings(2));
  expect(pending.status == skin::SkinProfileCommitResult::Status::Pending,
         "skin successor is accepted behind the ordinary save");
  store.unblock();
  ordinarySave.join();

  const auto blockerResult = waitForCommit(coordinator, blocker.ticket);
  const auto failed = waitForCommit(coordinator, pending.ticket);
  const auto loaded =
      AppSettingsStore::Load(manager.activePaths().settingsJson);
  expect(failed.status ==
                 skin::SkinProfileCommitResult::Status::RetryableFailure &&
             loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings.irProviders.at("tachi").enabled &&
             loaded.settings.skin.entries.at(sampleEntry())
                     .options.at("variant") == 1,
         "failed skin successor leaves the ordinary edit and prior skin "
         "durable");
  expect(blockerResult.status ==
                 skin::SkinProfileCommitResult::Status::Persisted &&
             store.candidates.size() == 3 &&
             !store.candidates[1].skin.entries.empty() &&
             store.candidates[1]
                     .skin.entries.at(sampleEntry())
                     .options.at("variant") == 1 &&
             store.candidates.back().irProviders.at("tachi").enabled,
         "FIFO writes merge at their own execution sequence point");
}

void testCommitIsAsyncCasAndTerminalResultIsAcknowledgedExplicitly() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(), "profile manager initializes");
  AppSettings active;
  BlockingStore store;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic = [&](const auto &path, const auto &settings,
                         std::string &error) {
        return store.save(path, settings, error);
      }});
  const auto profile = skin::makeSkinProfileId(manager.activeProfile().id);
  expect(profile.has_value(), "active profile has a typed skin profile ID");
  const auto base = coordinator.snapshot(*profile);
  const auto pending =
      coordinator.beginCommit(*profile, base.generation, selectedSettings(7));
  expect(pending.status == skin::SkinProfileCommitResult::Status::Pending &&
             pending.ticket != 0 && !store.entered,
         "beginCommit reserves a nonzero ticket without performing I/O");
  const auto stale =
      coordinator.beginCommit(*profile, base.generation, selectedSettings(8));
  expect(stale.status ==
             skin::SkinProfileCommitResult::Status::GenerationChanged,
         "stale generation is rejected");
  const auto unresolved = coordinator.beginCommit(
      *profile, pending.snapshot->generation, selectedSettings(9));
  expect(unresolved.status ==
             skin::SkinProfileCommitResult::Status::RetryableFailure,
         "a second unresolved commit for one profile is rejected");
  store.waitUntilEntered();
  store.unblock();
  const auto persisted = waitForCommit(coordinator, pending.ticket);
  expect(persisted.status == skin::SkinProfileCommitResult::Status::Persisted &&
             persisted.snapshot->settings.entries.at(sampleEntry())
                     .options.at("variant") == 7,
         "worker completion is observed through pollCommit");
  expect(coordinator.pollCommit(pending.ticket).status ==
             skin::SkinProfileCommitResult::Status::Persisted,
         "terminal result remains idempotently pollable");
  coordinator.acknowledgeCommit(pending.ticket);
  expect(coordinator.pollCommit(pending.ticket).status ==
             skin::SkinProfileCommitResult::Status::RetryableFailure,
         "acknowledging a terminal ticket releases it");
}

void testPostAdmissionFailureRollsBackWithoutPublishingOrEnqueueing() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for admission rollback");
  AppSettings active;
  int saveCalls = 0;
  bool failAdmission = true;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic =
           [&](const auto &path, const auto &settings, std::string &error) {
             ++saveCalls;
             return AppSettingsStore::Save(path, settings, error);
           },
       .afterSkinCommitStatePublished =
           [&] {
             if (std::exchange(failAdmission, false)) {
               throw std::runtime_error("synthetic post-admission failure");
             }
           }});
  const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
  const auto base = coordinator.snapshot(profile);

  const auto failed =
      coordinator.beginCommit(profile, base.generation, selectedSettings(31));
  const auto afterFailure = coordinator.snapshot(profile);
  const auto unknownFailedTicket = coordinator.pollCommit(failed.ticket);
  expect(failed.status ==
                 skin::SkinProfileCommitResult::Status::RetryableFailure &&
             failed.ticket == 0 && failed.failure &&
             failed.failure->code == "skin_profile_commit_admission_failed" &&
             unknownFailedTicket.status ==
                 skin::SkinProfileCommitResult::Status::RetryableFailure &&
             unknownFailedTicket.failure &&
             unknownFailedTicket.failure->code ==
                 "skin_profile_ticket_unknown" &&
             afterFailure.generation == base.generation &&
             afterFailure.settings == base.settings &&
             active.skin == base.settings && saveCalls == 0,
         "post-admission exception rolls back state and leaves no pollable "
         "ticket or queued I/O");

  const auto accepted =
      coordinator.beginCommit(profile, base.generation, selectedSettings(32));
  const auto persisted = waitForCommit(coordinator, accepted.ticket);
  expect(accepted.status == skin::SkinProfileCommitResult::Status::Pending &&
             accepted.snapshot &&
             accepted.snapshot->generation > base.generation + 1 &&
             persisted.status ==
                 skin::SkinProfileCommitResult::Status::Persisted &&
             saveCalls == 1,
         "rollback releases unresolved admission while never reusing its "
         "reserved generation");
  coordinator.acknowledgeCommit(accepted.ticket);
  coordinator.shutdown();
  expect(
      saveCalls == 1,
      "shutdown cannot later execute the rolled-back admission's removed job");
}

void testFailureRollsBackWithoutReusingGenerationAndPathIsCaptured() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(), "profile manager initializes for failure");
  const std::string oldId = manager.activeProfile().id;
  const auto created = manager.createProfile("Second");
  expect(created.ok() && created.profile.has_value(), "second profile creates");
  AppSettings active;
  BlockingStore store;
  store.failFirst = true;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic = [&](const auto &path, const auto &settings,
                         std::string &error) {
        return store.save(path, settings, error);
      }});
  const auto oldProfile = *skin::makeSkinProfileId(oldId);
  const auto base = coordinator.snapshot(oldProfile);
  const auto pending =
      coordinator.beginCommit(oldProfile, base.generation, selectedSettings(1));
  store.waitUntilEntered();
  expect(manager.commitActiveProfile(created.profile->id).ok(),
         "manager switches while old save is blocked");
  store.unblock();
  const auto failed = waitForCommit(coordinator, pending.ticket);
  expect(failed.status ==
                 skin::SkinProfileCommitResult::Status::RetryableFailure &&
             failed.snapshot->generation > base.generation &&
             !failed.snapshot->settings.gameplayCompatibilityEnabled,
         "failed save rolls settings back while advancing the generation");
  expect(store.paths.front() == manager.pathsFor(oldId).settingsJson,
         "queued work writes only its captured old-profile path");
  coordinator.acknowledgeCommit(pending.ticket);
  coordinator.bindCommittedActiveProfile(oldProfile, active);
  const auto rebound = coordinator.snapshot(oldProfile);
  expect(rebound.generation > failed.snapshot->generation,
         "successful active-profile bind advances without ABA reuse");
}

void testOrdinarySaveMergesPendingSkinAndLatestIrCandidate() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(), "profile manager initializes for merge");
  AppSettings active;
  BlockingStore store;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic = [&](const auto &path, const auto &settings,
                         std::string &error) {
        return store.save(path, settings, error);
      }});
  const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
  const auto pending = coordinator.beginCommit(
      profile, coordinator.snapshot(profile).generation, selectedSettings(42));
  store.waitUntilEntered();

  AppSettings ordinary = active;
  ordinary.irProviders["tachi"].enabled = true;
  std::string ordinaryError;
  std::atomic<bool> ordinaryFinished = false;
  std::thread waiter([&] {
    const bool saved =
        coordinator.saveActiveSettingsAndWait(profile, ordinary, ordinaryError);
    expect(saved, "ordinary full save succeeds after pending skin save: " +
                      ordinaryError);
    ordinaryFinished = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!ordinaryFinished.load(),
         "ordinary save waits behind the earlier serialized skin save");
  store.unblock();
  waiter.join();
  const auto committed = waitForCommit(coordinator, pending.ticket);
  expect(committed.status == skin::SkinProfileCommitResult::Status::Persisted,
         "pending skin commit remains independently pollable");
  expect(ordinary.skin.gameplayCompatibilityEnabled &&
             ordinary.skin.entries.at(sampleEntry()).options.at("variant") ==
                 42 &&
             ordinary.irProviders.at("tachi").enabled,
         "ordinary candidate merges durable skin and keeps the IR edit");
  const auto loaded =
      AppSettingsStore::Load(manager.activePaths().settingsJson);
  expect(loaded.status == AppSettingsLoadStatus::Loaded &&
             loaded.settings == ordinary,
         "merged full candidate is the durable settings document");
  coordinator.acknowledgeCommit(pending.ticket);
}

void testSnapshotTicketsAndInventoryFenceRejectAba() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(), "profile manager initializes for snapshot");
  const auto created = manager.createProfile("Second");
  expect(created.ok(), "snapshot fixture creates inactive profile");
  AppSettings active;
  ProfileSettingsPersistenceCoordinator coordinator(manager, active);
  const auto firstTicket = coordinator.beginSnapshotAllProfiles();
  coordinator.cancelSnapshotAllProfiles(firstTicket);
  const auto secondTicket = coordinator.beginSnapshotAllProfiles();
  expect(firstTicket != 0 && secondTicket > firstTicket,
         "snapshot tickets are nonzero monotonic and never reused");
  std::optional<skin::AllSkinProfileSnapshotsResult> all;
  for (int attempt = 0; attempt < 500 && !all; ++attempt) {
    all = coordinator.pollSnapshotAllProfiles(secondTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(all && all->complete && all->inventory &&
             all->inventory->profiles.size() == 2,
         "all-profile snapshot includes active and inactive profiles");
  if (all && all->inventory) {
    auto fence = coordinator.tryAcquireInventoryCommitFence(*all->inventory);
    expect(fence.has_value(),
           "exact inventory epoch/generations acquire fence");
    fence.reset();
    auto barrier = coordinator.beginInventoryMutation();
    expect(!coordinator.tryAcquireInventoryCommitFence(*all->inventory),
           "inventory mutation invalidates old snapshots before publication");
    coordinator.finishInventoryMutation(std::move(barrier));
  }
}

void testSnapshotAdmissionAfterShutdownIsTerminallyCancelled() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for snapshot shutdown race");
  AppSettings active;
  TestBlockingPoint inventoryCaptured;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active, {.afterSnapshotInputsCaptured = [&] {
        inventoryCaptured.arriveAndWait();
      }});

  std::uint64_t ticket = 0;
  std::thread admission(
      [&] { ticket = coordinator.beginSnapshotAllProfiles(); });
  inventoryCaptured.waitUntilArrived();
  coordinator.shutdown();
  inventoryCaptured.release();
  admission.join();
  const auto result = coordinator.pollSnapshotAllProfiles(ticket);
  expect(ticket != 0 && result && result->cancelled && !result->complete,
         "snapshot admission losing the shutdown race returns a terminal "
         "cancellation");
}

void testNonStandardSkinWorkerFailureRollsBackAndTerminatesTicket() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for non-standard skin failure");
  AppSettings active;
  TestSignal workerEntered;
  std::atomic<int> saveCalls = 0;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.saveAtomic = [&](const auto &path, const auto &settings,
                         std::string &error) -> bool {
        if (++saveCalls == 1) {
          workerEntered.arrive();
          throw 17;
        }
        return AppSettingsStore::Save(path, settings, error);
      }});
  const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
  const auto base = coordinator.snapshot(profile);
  const auto pending =
      coordinator.beginCommit(profile, base.generation, selectedSettings(71));
  workerEntered.wait();
  AppSettings ordinary = active;
  ordinary.irProviders["tachi"].enabled = true;
  std::string saveError;
  expect(coordinator.saveActiveSettingsAndWait(profile, ordinary, saveError),
         "ordinary save continues after non-standard skin failure: " +
             saveError);
  coordinator.shutdown();

  const auto failed = coordinator.pollCommit(pending.ticket);
  expect(failed.status ==
                 skin::SkinProfileCommitResult::Status::RetryableFailure &&
             failed.failure &&
             failed.failure->code == "skin_profile_worker_failure" &&
             failed.snapshot && failed.snapshot->settings == base.settings &&
             coordinator.snapshot(profile).settings == base.settings &&
             active.skin == base.settings && ordinary.skin == base.settings,
         "non-standard skin worker failure rolls back optimistic state and "
         "cannot leak its candidate into a later ordinary save");
}

void testNonStandardSnapshotWorkerFailureTerminatesTicket() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for non-standard snapshot failure");
  expect(manager.createProfile("Snapshot Failure").ok(),
         "non-standard snapshot fixture creates an inactive profile");
  AppSettings active;
  TestSignal workerEntered;
  ProfileSettingsPersistenceCoordinator coordinator(
      manager, active,
      {.loadSettings = [&](const auto &) -> AppSettingsLoadResult {
        workerEntered.arrive();
        throw 29;
      }});
  const auto ticket = coordinator.beginSnapshotAllProfiles();
  workerEntered.wait();
  std::string flushError;
  const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
  expect(coordinator.flushProfileAndWait(profile, flushError),
         "worker continues after non-standard snapshot failure: " + flushError);
  coordinator.shutdown();

  const auto failed = coordinator.pollSnapshotAllProfiles(ticket);
  expect(failed && !failed->complete && !failed->cancelled &&
             !failed->inventory && failed->diagnostics.size() == 1 &&
             failed->diagnostics.front().code == "skin_snapshot_worker_failure",
         "non-standard snapshot worker failure clears pending admission and "
         "publishes a terminal result after shutdown");
}

void testInventoryRaiiTokensCanOutliveCoordinatorShutdown() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for inventory token lifetime");
  AppSettings active;
  std::optional<skin::ProfileInventoryCommitFence> fence;
  {
    auto coordinator = std::make_unique<ProfileSettingsPersistenceCoordinator>(
        manager, active);
    const auto ticket = coordinator->beginSnapshotAllProfiles();
    std::optional<skin::AllSkinProfileSnapshotsResult> snapshot;
    for (int attempt = 0; attempt < 500 && !snapshot; ++attempt) {
      snapshot = coordinator->pollSnapshotAllProfiles(ticket);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(snapshot && snapshot->inventory,
           "inventory token fixture captures a complete snapshot");
    if (snapshot && snapshot->inventory) {
      fence = coordinator->tryAcquireInventoryCommitFence(*snapshot->inventory);
    }
    expect(fence.has_value(), "inventory token fixture acquires a fence");
    coordinator->shutdown();
  }
  auto replacement =
      std::make_unique<ProfileSettingsPersistenceCoordinator>(manager, active);
  const auto replacementTicket = replacement->beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> replacementSnapshot;
  for (int attempt = 0; attempt < 500 && !replacementSnapshot; ++attempt) {
    replacementSnapshot =
        replacement->pollSnapshotAllProfiles(replacementTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  auto replacementFence = replacementSnapshot && replacementSnapshot->inventory
                              ? replacement->tryAcquireInventoryCommitFence(
                                    *replacementSnapshot->inventory)
                              : std::nullopt;
  expect(replacementFence.has_value(),
         "replacement coordinator acquires its own fence");
  fence.reset();
  std::atomic<bool> replacementCommitReturned = false;
  std::thread replacementCommit([&] {
    const auto profile = *skin::makeSkinProfileId(manager.activeProfile().id);
    replacement->beginCommit(profile, replacement->snapshot(profile).generation,
                             selectedSettings(17));
    replacementCommitReturned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  expect(!replacementCommitReturned.load(),
         "releasing an old fence cannot release a replacement owner's fence");
  replacementFence.reset();
  replacementCommit.join();
  replacement->shutdown();
  replacement.reset();

  std::optional<skin::ProfileInventoryMutationBarrier> barrier;
  {
    auto coordinator = std::make_unique<ProfileSettingsPersistenceCoordinator>(
        manager, active);
    barrier.emplace(coordinator->beginInventoryMutation());
    coordinator->shutdown();
  }
  barrier.reset();
  expect(true, "fences and barriers release safely after owner destruction");
}

void testSnapshotPollConsumesTerminalResult() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for terminal snapshot consumption");
  AppSettings active;
  ProfileSettingsPersistenceCoordinator coordinator(manager, active);

  const auto ticket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> terminal;
  for (int attempt = 0; attempt < 500 && !terminal; ++attempt) {
    terminal = coordinator.pollSnapshotAllProfiles(ticket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(terminal && terminal->complete && terminal->inventory,
         "terminal snapshot remains available until its first completed poll");
  expect(!coordinator.pollSnapshotAllProfiles(ticket),
         "completed snapshot is consumed so repeated scans do not retain it");
}

void testSnapshotReconcilesDeletedInactiveProfileCache() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for snapshot cache reconciliation");
  const auto created = manager.createProfile("Disposable");
  expect(created.ok() && created.profile,
         "snapshot cache reconciliation fixture creates an inactive profile");
  AppSettings active;
  ProfileSettingsPersistenceCoordinator coordinator(manager, active);

  const auto initialTicket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> initial;
  for (int attempt = 0; attempt < 500 && !initial; ++attempt) {
    initial = coordinator.pollSnapshotAllProfiles(initialTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(initial && initial->complete && initial->inventory &&
             initial->inventory->profiles.size() == 2,
         "initial snapshot caches the inactive profile");

  auto mutation = coordinator.beginInventoryMutation();
  expect(manager.deleteProfile(created.profile->id).ok(),
         "inactive profile deletion succeeds under the inventory mutation");
  coordinator.finishInventoryMutation(std::move(mutation));

  const auto refreshedTicket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> refreshed;
  for (int attempt = 0; attempt < 500 && !refreshed; ++attempt) {
    refreshed = coordinator.pollSnapshotAllProfiles(refreshedTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(refreshed && refreshed->complete && refreshed->inventory &&
             refreshed->inventory->profiles.size() == 1 &&
             coordinator.tryAcquireInventoryCommitFence(*refreshed->inventory),
         "a refreshed snapshot drops the deleted cache entry and can publish");
}

void testSnapshotRefreshesOverwrittenInactiveProfileSettings() {
  TempDirectory temp;
  PlayerProfileManager manager(temp.path(), managerDependencies());
  expect(manager.Initialize().ok(),
         "profile manager initializes for overwritten snapshot refresh");
  const auto created = manager.createProfile("Replaceable");
  expect(created.ok() && created.profile,
         "overwritten snapshot fixture creates an inactive profile");
  AppSettings active;
  ProfileSettingsPersistenceCoordinator coordinator(manager, active);

  const auto typedId = skin::makeSkinProfileId(created.profile->id);
  const auto initialTicket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> initial;
  for (int attempt = 0; attempt < 500 && !initial; ++attempt) {
    initial = coordinator.pollSnapshotAllProfiles(initialTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::uint64_t initialGeneration = 0;
  if (initial && initial->inventory && typedId) {
    for (const auto &profile : initial->inventory->profiles) {
      if (profile.profileId == *typedId) {
        initialGeneration = profile.generation;
      }
    }
  }

  const auto sourcePaths = manager.pathsFor(created.profile->id);
  const auto beforeReplacement = AppSettingsStore::Load(sourcePaths.settingsJson);
  expect(beforeReplacement.status == AppSettingsLoadStatus::Loaded,
         "overwritten snapshot fixture loads its initial inactive settings");
  AppSettings replacement = beforeReplacement.settings;
  const auto replacementEntry = sampleEntry("Replacement");
  replacement.skin.selectedGameplayEntries[0] = replacementEntry;
  replacement.skin.entries[replacementEntry].options["variant"] = 73;
  replacement.skin.sanitize();
  expect(replacement.skin != beforeReplacement.settings.skin,
         "replacement fixture changes the inactive skin settings");
  auto mutation = coordinator.beginInventoryMutation();
  const auto overwritten = manager.installProfile(
      *created.profile, created.profile->id,
      [sourcePaths, replacement](const PlayerProfilePaths &staging,
                                 std::string &stagingError) {
        std::error_code copyError;
        for (const auto &source :
             std::filesystem::directory_iterator(sourcePaths.root, copyError)) {
          if (copyError) {
            break;
          }
          std::filesystem::copy(
              source.path(), staging.root / source.path().filename(),
              std::filesystem::copy_options::recursive, copyError);
          if (copyError) {
            break;
          }
        }
        if (copyError) {
          stagingError = copyError.message();
          return false;
        }
        return AppSettingsStore::Save(staging.settingsJson, replacement,
                                      stagingError);
      });
  expect(overwritten.ok(), "inactive profile overwrite succeeds: " +
                               overwritten.message);
  coordinator.finishInventoryMutation(std::move(mutation));
  const auto durable =
      AppSettingsStore::Load(manager.pathsFor(created.profile->id).settingsJson);
  expect(durable.status == AppSettingsLoadStatus::Loaded,
         "overwritten inactive profile settings remain loadable");
  expect(durable.settings.skin == replacement.skin,
         "overwritten inactive profile has replacement settings on disk");

  const auto refreshedTicket = coordinator.beginSnapshotAllProfiles();
  std::optional<skin::AllSkinProfileSnapshotsResult> refreshed;
  for (int attempt = 0; attempt < 500 && !refreshed; ++attempt) {
    refreshed = coordinator.pollSnapshotAllProfiles(refreshedTicket);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const auto cached = typedId ? coordinator.snapshot(*typedId)
                              : skin::VersionedSkinProfileSettings{};
  expect(refreshed && refreshed->complete && refreshed->inventory,
         "overwritten profile refresh completes with an inventory");
  expect(typedId && cached.settings == replacement.skin,
         "overwritten profile refresh updates cached skin settings");
  expect(cached.generation > initialGeneration,
         "overwritten profile refresh advances its generation");
  expect(refreshed && refreshed->inventory &&
             coordinator.tryAcquireInventoryCommitFence(*refreshed->inventory),
         "overwritten profile refresh can publish its refreshed inventory");
}
} // namespace

int main() {
  testCommitIsAsyncCasAndTerminalResultIsAcknowledgedExplicitly();
  testPostAdmissionFailureRollsBackWithoutPublishingOrEnqueueing();
  testFailureRollsBackWithoutReusingGenerationAndPathIsCaptured();
  testOrdinarySaveMergesPendingSkinAndLatestIrCandidate();
  testOrdinarySaveBeforeFailedSkinCommitKeepsLatestFullDocumentDurable();
  testSnapshotTicketsAndInventoryFenceRejectAba();
  testSnapshotAdmissionAfterShutdownIsTerminallyCancelled();
  testNonStandardSkinWorkerFailureRollsBackAndTerminatesTicket();
  testNonStandardSnapshotWorkerFailureTerminatesTicket();
  testInventoryRaiiTokensCanOutliveCoordinatorShutdown();
  testSnapshotPollConsumesTerminalResult();
  testSnapshotReconcilesDeletedInactiveProfileCache();
  testSnapshotRefreshesOverwrittenInactiveProfileSettings();
  if (failures != 0) {
    std::cerr << failures
              << " profile settings persistence assertion(s) failed\n";
    return 1;
  }
  std::cout << "profile settings persistence tests passed\n";
  return 0;
}
