#include "ProfileSettingsPersistenceCoordinator.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

skin::SkinDiagnostic failureDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = skin::DiagnosticSeverity::Error};
}

skin::SkinProfileCommitResult unknownCommit(std::uint64_t ticket) {
  return {.status = skin::SkinProfileCommitResult::Status::RetryableFailure,
          .ticket = ticket,
          .failure = failureDiagnostic("skin_profile_ticket_unknown",
                                       "Skin profile ticket is not available")};
}

} // namespace

namespace skin {

ProfileInventoryMutationBarrier::ProfileInventoryMutationBarrier(
    std::function<void()> release)
    : release_(std::move(release)) {}

ProfileInventoryMutationBarrier::ProfileInventoryMutationBarrier(
    ProfileInventoryMutationBarrier &&other) noexcept
    : release_(std::move(other.release_)) {
  other.release_ = {};
}

ProfileInventoryMutationBarrier &ProfileInventoryMutationBarrier::operator=(
    ProfileInventoryMutationBarrier &&other) noexcept {
  if (this != &other) {
    release();
    release_ = std::move(other.release_);
    other.release_ = {};
  }
  return *this;
}

ProfileInventoryMutationBarrier::~ProfileInventoryMutationBarrier() {
  release();
}

void ProfileInventoryMutationBarrier::release() noexcept {
  if (!release_) {
    return;
  }
  auto release = std::move(release_);
  try {
    release();
  } catch (...) {
  }
}

} // namespace skin

namespace {
struct InventoryGateState {
  std::mutex mutex;
  std::condition_variable cv;
  std::uint64_t generation = 1;
  std::size_t activeFences = 0;
  bool mutationActive = false;
  bool stopping = false;
};
} // namespace

struct ProfileSettingsPersistenceCoordinator::Impl {
  struct ProfileState {
    std::uint64_t generation = 1;
    std::uint64_t highWaterGeneration = 1;
    skin::SkinProfileSettings settings;
    AppSettings durableSettings;
    std::optional<std::uint64_t> unresolvedTicket;
  };

  struct WaitResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool success = false;
    AppSettings settings;
    std::string error;
  };

  enum class JobKind { SkinCommit, FullSave, Flush, SnapshotAll };

  struct SnapshotInput {
    skin::SkinProfileId id;
    std::filesystem::path path;
    bool active = false;
  };

  struct Job {
    JobKind kind = JobKind::Flush;
    std::uint64_t ticket = 0;
    skin::SkinProfileId profileId;
    std::filesystem::path path;
    AppSettings settings;
    skin::SkinProfileSettings previousSkin;
    std::shared_ptr<WaitResult> waiter;
    std::uint64_t inventoryGeneration = 0;
    std::uint64_t skinGeneration = 0;
    std::vector<SnapshotInput> snapshotInputs;
  };

  PlayerProfileManager &manager;
  AppSettings &activeSettings;
  ProfileSettingsPersistenceDependencies dependencies;
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<Job> jobs;
  std::map<std::string, ProfileState, std::less<>> profiles;
  std::map<std::uint64_t, skin::SkinProfileCommitResult> commits;
  std::map<std::uint64_t, skin::AllSkinProfileSnapshotsResult> snapshots;
  std::set<std::uint64_t> cancelledSnapshots;
  std::set<std::uint64_t> pendingSnapshots;
  std::string activeProfileId;
  std::uint64_t nextTicket = 1;
  std::shared_ptr<InventoryGateState> inventoryGate =
      std::make_shared<InventoryGateState>();
  bool stopping = false;
  bool stopped = false;
  // Deliberately last: reverse destruction cannot tear down borrowed state
  // before shutdown has joined the worker.
  std::thread worker;

  Impl(PlayerProfileManager &managerValue, AppSettings &settingsValue,
       ProfileSettingsPersistenceDependencies dependencyValue)
      : manager(managerValue), activeSettings(settingsValue),
        dependencies(std::move(dependencyValue)),
        activeProfileId(manager.activeProfile().id) {
    profiles[activeProfileId].settings = activeSettings.skin;
    profiles[activeProfileId].durableSettings = activeSettings;
    worker = std::thread([this] { run(); });
  }

  ~Impl() { shutdown(); }

  std::uint64_t allocateTicket() {
    if (nextTicket == 0) {
      ++nextTicket;
    }
    return nextTicket++;
  }

  skin::VersionedSkinProfileSettings
  snapshotLocked(const skin::SkinProfileId &profileId) const {
    const auto found = profiles.find(profileId.opaque);
    if (found == profiles.end()) {
      return {.profileId = profileId};
    }
    return {.profileId = profileId,
            .generation = found->second.generation,
            .settings = found->second.settings};
  }

  void enqueue(Job job) {
    jobs.push_back(std::move(job));
    cv.notify_one();
  }

  void completeWaiter(const std::shared_ptr<WaitResult> &waiter, bool success,
                      AppSettings settings, std::string error) {
    if (!waiter) {
      return;
    }
    std::lock_guard lock(waiter->mutex);
    waiter->success = success;
    waiter->settings = std::move(settings);
    waiter->error = std::move(error);
    waiter->done = true;
    waiter->cv.notify_all();
  }

  void processSkinCommit(Job &job) {
    AppSettings merged;
    {
      std::lock_guard lock(mutex);
      const auto state = profiles.find(job.profileId.opaque);
      if (state == profiles.end()) {
        return;
      }
      merged = state->second.durableSettings;
      merged.skin = job.settings.skin;
    }
    merged.sanitize();
    std::string error;
    const bool saved = dependencies.saveAtomic(job.path, merged, error);
    std::lock_guard lock(mutex);
    auto &state = profiles[job.profileId.opaque];
    skin::SkinProfileCommitResult result{
        .status = saved
                      ? skin::SkinProfileCommitResult::Status::Persisted
                      : skin::SkinProfileCommitResult::Status::RetryableFailure,
        .ticket = job.ticket,
        .snapshot = snapshotLocked(job.profileId),
    };
    if (!saved) {
      state.settings = std::move(job.previousSkin);
      result.snapshot = snapshotLocked(job.profileId);
      result.failure = failureDiagnostic(
          "skin_profile_save_failed",
          error.empty() ? "Skin profile settings could not be saved" : error);
    } else {
      state.durableSettings = std::move(merged);
    }
    commits[job.ticket] = std::move(result);
  }

  void processFullSave(Job &job) {
    {
      std::lock_guard lock(mutex);
      const auto state = profiles.find(job.profileId.opaque);
      if (state == profiles.end()) {
        completeWaiter(job.waiter, false, {}, "Skin profile is not bound");
        return;
      }
      if (state->second.generation == job.skinGeneration) {
        job.settings.skin = state->second.settings;
      }
    }
    job.settings.sanitize();
    std::string error;
    const bool saved = dependencies.saveAtomic(job.path, job.settings, error);
    if (saved) {
      std::lock_guard lock(mutex);
      const auto state = profiles.find(job.profileId.opaque);
      if (state != profiles.end()) {
        state->second.durableSettings = job.settings;
      }
    }
    completeWaiter(job.waiter, saved, std::move(job.settings),
                   saved ? std::string() : std::move(error));
  }

  bool snapshotWasCancelled(std::uint64_t ticket) {
    std::lock_guard lock(mutex);
    return cancelledSnapshots.contains(ticket);
  }

  void processSnapshotAll(Job &job) {
    skin::AllSkinProfileSnapshotsResult result;
    skin::ProfileInventorySnapshot inventory{.inventoryGeneration =
                                                 job.inventoryGeneration};
    for (const auto &input : job.snapshotInputs) {
      if (snapshotWasCancelled(job.ticket)) {
        result.cancelled = true;
        break;
      }
      if (input.active) {
        std::lock_guard lock(mutex);
        inventory.profiles.push_back(snapshotLocked(input.id));
        continue;
      }
      const auto loaded = dependencies.loadSettings(input.path);
      if (loaded.status != AppSettingsLoadStatus::Loaded) {
        result.diagnostics.push_back(failureDiagnostic(
            "skin_profile_snapshot_load_failed",
            "Profile settings could not be loaded for " + input.id.opaque));
        continue;
      }
      std::lock_guard lock(mutex);
      auto [state, inserted] = profiles.try_emplace(input.id.opaque);
      if (inserted) {
        state->second.settings = loaded.settings.skin;
        state->second.durableSettings = loaded.settings;
      }
      inventory.profiles.push_back(snapshotLocked(input.id));
    }
    if (!result.cancelled && result.diagnostics.empty() &&
        inventory.profiles.size() == job.snapshotInputs.size()) {
      std::sort(inventory.profiles.begin(), inventory.profiles.end(),
                [](const auto &left, const auto &right) {
                  return left.profileId < right.profileId;
                });
      result.complete = true;
      result.inventory = std::move(inventory);
    }
    std::lock_guard lock(mutex);
    pendingSnapshots.erase(job.ticket);
    cancelledSnapshots.erase(job.ticket);
    snapshots[job.ticket] = std::move(result);
  }

  void terminalizeWorkerFailure(Job &job, std::string message) {
    switch (job.kind) {
    case JobKind::SkinCommit: {
      std::lock_guard lock(mutex);
      auto state = profiles.find(job.profileId.opaque);
      if (state != profiles.end()) {
        state->second.settings = std::move(job.previousSkin);
      }
      commits[job.ticket] = {
          .status = skin::SkinProfileCommitResult::Status::RetryableFailure,
          .ticket = job.ticket,
          .snapshot = snapshotLocked(job.profileId),
          .failure = failureDiagnostic("skin_profile_worker_failure",
                                       std::move(message)),
      };
      return;
    }
    case JobKind::SnapshotAll: {
      std::lock_guard lock(mutex);
      pendingSnapshots.erase(job.ticket);
      cancelledSnapshots.erase(job.ticket);
      snapshots[job.ticket] = {
          .diagnostics = {failureDiagnostic("skin_snapshot_worker_failure",
                                            std::move(message))}};
      return;
    }
    case JobKind::FullSave:
    case JobKind::Flush:
      completeWaiter(job.waiter, false, {}, std::move(message));
      return;
    }
  }

  void run() noexcept {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return stopping || !jobs.empty(); });
        if (jobs.empty()) {
          if (stopping) {
            return;
          }
          continue;
        }
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      try {
        switch (job.kind) {
        case JobKind::SkinCommit:
          processSkinCommit(job);
          break;
        case JobKind::FullSave:
          processFullSave(job);
          break;
        case JobKind::Flush:
          completeWaiter(job.waiter, true, {}, {});
          break;
        case JobKind::SnapshotAll:
          processSnapshotAll(job);
          break;
        }
      } catch (const std::exception &error) {
        terminalizeWorkerFailure(job, error.what());
      } catch (...) {
        terminalizeWorkerFailure(job, "Unknown persistence failure");
      }
    }
  }

  void shutdown() noexcept {
    {
      std::lock_guard gateLock(inventoryGate->mutex);
      inventoryGate->stopping = true;
      inventoryGate->cv.notify_all();
    }
    {
      std::lock_guard lock(mutex);
      if (stopped || stopping) {
        return;
      }
      stopping = true;
      cancelledSnapshots.insert(pendingSnapshots.begin(),
                                pendingSnapshots.end());
      cv.notify_all();
    }
    if (worker.joinable()) {
      worker.join();
    }
    std::lock_guard lock(mutex);
    stopped = true;
  }
};

ProfileSettingsPersistenceCoordinator::ProfileSettingsPersistenceCoordinator(
    PlayerProfileManager &manager, AppSettings &activeSettings,
    ProfileSettingsPersistenceDependencies dependencies)
    : impl_(std::make_unique<Impl>(manager, activeSettings,
                                   std::move(dependencies))) {}

ProfileSettingsPersistenceCoordinator::
    ~ProfileSettingsPersistenceCoordinator() {
  shutdown();
}

skin::VersionedSkinProfileSettings
ProfileSettingsPersistenceCoordinator::snapshot(
    const skin::SkinProfileId &profileId) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->snapshotLocked(profileId);
}

skin::SkinProfileCommitResult
ProfileSettingsPersistenceCoordinator::beginCommit(
    const skin::SkinProfileId &profileId, std::uint64_t expectedGeneration,
    skin::SkinProfileSettings candidate) {
  candidate.sanitize();
  const auto gate = impl_->inventoryGate;
  std::unique_lock gateLock(gate->mutex);
  gate->cv.wait(gateLock, [&] {
    return gate->stopping || (gate->activeFences == 0 && !gate->mutationActive);
  });
  std::lock_guard lock(impl_->mutex);
  auto found = impl_->profiles.find(profileId.opaque);
  if (gate->stopping || impl_->stopping || found == impl_->profiles.end()) {
    return {.status = skin::SkinProfileCommitResult::Status::RetryableFailure,
            .failure = failureDiagnostic("skin_profile_not_bound",
                                         "Skin profile is not bound")};
  }
  auto &state = found->second;
  if (state.generation != expectedGeneration) {
    return {.status = skin::SkinProfileCommitResult::Status::GenerationChanged,
            .generationChanged = true,
            .snapshot = impl_->snapshotLocked(profileId)};
  }
  if (state.unresolvedTicket) {
    return {.status = skin::SkinProfileCommitResult::Status::RetryableFailure,
            .snapshot = impl_->snapshotLocked(profileId),
            .failure = failureDiagnostic("skin_profile_commit_unresolved",
                                         "A profile commit is unresolved")};
  }

  const std::uint64_t ticket = impl_->allocateTicket();
  if (state.highWaterGeneration == std::numeric_limits<std::uint64_t>::max()) {
    return {.status = skin::SkinProfileCommitResult::Status::RetryableFailure,
            .failure =
                failureDiagnostic("skin_profile_generation_exhausted",
                                  "Skin profile generations are exhausted")};
  }
  const std::uint64_t previousGeneration = state.generation;
  const std::uint64_t reservedGeneration = state.highWaterGeneration + 1;

  skin::SkinProfileSettings previous;
  skin::SkinProfileSettings activePrevious;
  skin::SkinProfileSettings activeCandidate;
  AppSettings full;
  Impl::Job job;
  skin::SkinProfileCommitResult pending;
  skin::SkinProfileCommitResult callerResult;
  skin::SkinProfileCommitResult admissionFailure;
  try {
    previous = state.settings;
    activePrevious = impl_->activeSettings.skin;
    activeCandidate = candidate;
    full = impl_->activeSettings;
    full.skin = candidate;
    const skin::VersionedSkinProfileSettings nextSnapshot{
        .profileId = profileId,
        .generation = reservedGeneration,
        .settings = candidate,
    };
    pending = {
        .status = skin::SkinProfileCommitResult::Status::Pending,
        .ticket = ticket,
        .snapshot = nextSnapshot,
    };
    callerResult = pending;
    admissionFailure = {
        .status = skin::SkinProfileCommitResult::Status::RetryableFailure,
        .snapshot = impl_->snapshotLocked(profileId),
        .failure = failureDiagnostic(
            "skin_profile_commit_admission_failed",
            "Skin profile commit admission failed before worker handoff"),
    };
    job = {.kind = Impl::JobKind::SkinCommit,
           .ticket = ticket,
           .profileId = profileId,
           .path = impl_->manager.pathsFor(profileId.opaque).settingsJson,
           .settings = std::move(full),
           .previousSkin = previous};
    impl_->commits.emplace(ticket, std::move(pending));
    try {
      impl_->jobs.push_back(std::move(job));
    } catch (...) {
      impl_->commits.erase(ticket);
      throw;
    }
  } catch (...) {
    return admissionFailure.failure
               ? std::move(admissionFailure)
               : skin::SkinProfileCommitResult{
                     .status = skin::SkinProfileCommitResult::Status::
                         RetryableFailure,
                     .failure = failureDiagnostic(
                         "skin_profile_commit_admission_failed",
                         "Skin profile commit admission could not be "
                         "allocated")};
  }

  static_assert(std::is_nothrow_move_assignable_v<skin::SkinProfileSettings>);
  state.highWaterGeneration = reservedGeneration;
  state.generation = reservedGeneration;
  state.settings = std::move(candidate);
  state.unresolvedTicket = ticket;
  const bool activeProfile = impl_->activeProfileId == profileId.opaque;
  if (activeProfile) {
    impl_->activeSettings.skin = std::move(activeCandidate);
  }
  try {
    if (impl_->dependencies.afterSkinCommitStatePublished) {
      impl_->dependencies.afterSkinCommitStatePublished();
    }
  } catch (...) {
    if (activeProfile) {
      impl_->activeSettings.skin = std::move(activePrevious);
    }
    state.settings = std::move(previous);
    state.generation = previousGeneration;
    state.unresolvedTicket.reset();
    impl_->jobs.pop_back();
    impl_->commits.erase(ticket);
    return admissionFailure;
  }

  impl_->cv.notify_one();
  return callerResult;
}

skin::SkinProfileCommitResult
ProfileSettingsPersistenceCoordinator::pollCommit(std::uint64_t ticket) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->commits.find(ticket);
  if (found == impl_->commits.end()) {
    return unknownCommit(ticket);
  }
  if (found->second.status != skin::SkinProfileCommitResult::Status::Pending &&
      found->second.snapshot &&
      found->second.snapshot->profileId.opaque == impl_->activeProfileId) {
    impl_->activeSettings.skin = found->second.snapshot->settings;
  }
  return found->second;
}

void ProfileSettingsPersistenceCoordinator::acknowledgeCommit(
    std::uint64_t ticket) noexcept {
  try {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->commits.find(ticket);
    if (found == impl_->commits.end() ||
        found->second.status ==
            skin::SkinProfileCommitResult::Status::Pending) {
      return;
    }
    if (found->second.snapshot) {
      auto state =
          impl_->profiles.find(found->second.snapshot->profileId.opaque);
      if (state != impl_->profiles.end() &&
          state->second.unresolvedTicket == ticket) {
        state->second.unresolvedTicket.reset();
      }
    }
    impl_->commits.erase(found);
  } catch (...) {
  }
}

std::uint64_t
ProfileSettingsPersistenceCoordinator::beginSnapshotAllProfiles() {
  const auto gate = impl_->inventoryGate;
  std::unique_lock gateLock(gate->mutex);
  std::unique_lock lock(impl_->mutex);
  const std::uint64_t ticket = impl_->allocateTicket();
  if (gate->stopping || impl_->stopping) {
    impl_->snapshots[ticket] = {.cancelled = true};
    return ticket;
  }
  const std::uint64_t generation = gate->generation;
  const std::string activeProfileId = impl_->activeProfileId;
  lock.unlock();
  gateLock.unlock();

  std::vector<Impl::SnapshotInput> inputs;
  for (const auto &profile : impl_->manager.listProfiles()) {
    auto id = skin::makeSkinProfileId(profile.id);
    if (!id) {
      continue;
    }
    inputs.push_back({.id = *id,
                      .path = impl_->manager.pathsFor(profile.id).settingsJson,
                      .active = profile.id == activeProfileId});
  }
  try {
    if (impl_->dependencies.afterSnapshotInputsCaptured) {
      impl_->dependencies.afterSnapshotInputsCaptured();
    }
  } catch (...) {
  }
  gateLock.lock();
  lock.lock();
  if (gate->stopping || impl_->stopping) {
    impl_->snapshots[ticket] = {.cancelled = true};
    return ticket;
  }
  impl_->pendingSnapshots.insert(ticket);
  impl_->enqueue({.kind = Impl::JobKind::SnapshotAll,
                  .ticket = ticket,
                  .inventoryGeneration = generation,
                  .snapshotInputs = std::move(inputs)});
  return ticket;
}

std::optional<skin::AllSkinProfileSnapshotsResult>
ProfileSettingsPersistenceCoordinator::pollSnapshotAllProfiles(
    std::uint64_t ticket) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->snapshots.find(ticket);
  return found == impl_->snapshots.end()
             ? std::nullopt
             : std::optional<skin::AllSkinProfileSnapshotsResult>(
                   found->second);
}

void ProfileSettingsPersistenceCoordinator::cancelSnapshotAllProfiles(
    std::uint64_t ticket) noexcept {
  try {
    std::lock_guard lock(impl_->mutex);
    if (impl_->pendingSnapshots.contains(ticket)) {
      impl_->cancelledSnapshots.insert(ticket);
    }
  } catch (...) {
  }
}

std::optional<skin::ProfileInventoryCommitFence>
ProfileSettingsPersistenceCoordinator::tryAcquireInventoryCommitFence(
    const skin::ProfileInventorySnapshot &inventory) {
  const auto gate = impl_->inventoryGate;
  std::lock_guard gateLock(gate->mutex);
  std::lock_guard lock(impl_->mutex);
  if (gate->stopping || impl_->stopping || gate->mutationActive ||
      inventory.inventoryGeneration != gate->generation ||
      inventory.profiles.size() != impl_->profiles.size()) {
    return std::nullopt;
  }
  for (const auto &profile : inventory.profiles) {
    const auto found = impl_->profiles.find(profile.profileId.opaque);
    if (found == impl_->profiles.end() ||
        found->second.generation != profile.generation) {
      return std::nullopt;
    }
  }
  ++gate->activeFences;
  return skin::ProfileInventoryCommitFence([gate] {
    std::lock_guard lock(gate->mutex);
    if (gate->activeFences != 0) {
      --gate->activeFences;
    }
    gate->cv.notify_all();
  });
}

skin::ProfileInventoryMutationBarrier
ProfileSettingsPersistenceCoordinator::beginInventoryMutation() {
  const auto gate = impl_->inventoryGate;
  std::unique_lock lock(gate->mutex);
  gate->cv.wait(lock, [&] { return gate->stopping || !gate->mutationActive; });
  if (gate->stopping) {
    return skin::ProfileInventoryMutationBarrier([] {});
  }
  gate->mutationActive = true;
  ++gate->generation;
  gate->cv.wait(lock,
                [&] { return gate->stopping || gate->activeFences == 0; });
  return skin::ProfileInventoryMutationBarrier([gate] {
    std::lock_guard lock(gate->mutex);
    if (gate->mutationActive) {
      gate->mutationActive = false;
      ++gate->generation;
    }
    gate->cv.notify_all();
  });
}

void ProfileSettingsPersistenceCoordinator::finishInventoryMutation(
    skin::ProfileInventoryMutationBarrier &&barrier) noexcept {
  barrier.release();
}

bool ProfileSettingsPersistenceCoordinator::saveActiveSettingsAndWait(
    const skin::SkinProfileId &profileId, AppSettings &settings,
    std::string &error) {
  auto waiter = std::make_shared<Impl::WaitResult>();
  {
    const auto gate = impl_->inventoryGate;
    std::unique_lock gateLock(gate->mutex);
    gate->cv.wait(gateLock, [&] {
      return gate->stopping ||
             (gate->activeFences == 0 && !gate->mutationActive);
    });
    std::lock_guard lock(impl_->mutex);
    if (gate->stopping || impl_->stopping ||
        profileId.opaque != impl_->activeProfileId) {
      error = "The requested profile is not the bound active profile";
      return false;
    }
    impl_->enqueue(
        {.kind = Impl::JobKind::FullSave,
         .profileId = profileId,
         .path = impl_->manager.pathsFor(profileId.opaque).settingsJson,
         .settings = settings,
         .waiter = waiter,
         .skinGeneration = impl_->profiles.at(profileId.opaque).generation});
  }
  try {
    if (impl_->dependencies.afterFullSaveAdmitted) {
      impl_->dependencies.afterFullSaveAdmitted();
    }
  } catch (...) {
  }
  std::unique_lock waitLock(waiter->mutex);
  waiter->cv.wait(waitLock, [&] { return waiter->done; });
  if (!waiter->success) {
    error = std::move(waiter->error);
    return false;
  }
  settings = waiter->settings;
  impl_->activeSettings = settings;
  return true;
}

bool ProfileSettingsPersistenceCoordinator::flushProfileAndWait(
    const skin::SkinProfileId &profileId, std::string &error) {
  auto waiter = std::make_shared<Impl::WaitResult>();
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || !impl_->profiles.contains(profileId.opaque)) {
      error = "Skin profile is not available";
      return false;
    }
    impl_->enqueue({.kind = Impl::JobKind::Flush,
                    .profileId = profileId,
                    .waiter = waiter});
  }
  std::unique_lock waitLock(waiter->mutex);
  waiter->cv.wait(waitLock, [&] { return waiter->done; });
  error = std::move(waiter->error);
  return waiter->success;
}

void ProfileSettingsPersistenceCoordinator::bindCommittedActiveProfile(
    skin::SkinProfileId profileId, AppSettings &settings) {
  settings.sanitize();
  const auto gate = impl_->inventoryGate;
  std::unique_lock gateLock(gate->mutex);
  gate->cv.wait(gateLock, [&] {
    return gate->stopping || (gate->activeFences == 0 && !gate->mutationActive);
  });
  if (gate->stopping) {
    return;
  }
  std::lock_guard lock(impl_->mutex);
  auto &state = impl_->profiles[profileId.opaque];
  state.highWaterGeneration =
      std::max(state.highWaterGeneration, state.generation);
  state.generation = ++state.highWaterGeneration;
  state.settings = settings.skin;
  state.durableSettings = settings;
  impl_->activeProfileId = profileId.opaque;
  impl_->activeSettings = settings;
  ++gate->generation;
}

void ProfileSettingsPersistenceCoordinator::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}
