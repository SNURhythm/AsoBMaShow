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
#include <string>
#include <thread>
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
  std::vector<std::filesystem::path> paths;
  std::vector<AppSettings> candidates;

  bool save(const std::filesystem::path &path, const AppSettings &candidate,
            std::string &error) {
    std::unique_lock lock(mutex);
    entered = true;
    paths.push_back(path);
    candidates.push_back(candidate);
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    if (failFirst) {
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
} // namespace

int main() {
  testCommitIsAsyncCasAndTerminalResultIsAcknowledgedExplicitly();
  testFailureRollsBackWithoutReusingGenerationAndPathIsCaptured();
  testOrdinarySaveMergesPendingSkinAndLatestIrCandidate();
  testSnapshotTicketsAndInventoryFenceRejectAba();
  if (failures != 0) {
    std::cerr << failures
              << " profile settings persistence assertion(s) failed\n";
    return 1;
  }
  std::cout << "profile settings persistence tests passed\n";
  return 0;
}
