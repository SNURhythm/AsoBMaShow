#include "SkinCommitCoordinator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

constexpr std::size_t kMaxDeliveryRecordsPerClient = 128;
constexpr std::size_t kMaxRevalidationProfiles = 128;

static_assert(std::is_nothrow_move_constructible_v<CommitActivationResult>);
static_assert(std::is_nothrow_move_assignable_v<CommitActivationResult>);
static_assert(std::is_nothrow_move_constructible_v<SkinProfileCommitResult>);
static_assert(std::is_nothrow_move_assignable_v<SkinProfileCommitResult>);

std::atomic_uint64_t nextClientId{0};
std::atomic_uint64_t nextCoordinatorTicket{0};
std::atomic_uint64_t nextBarrierToken{0};

std::uint64_t allocateNeverReused(std::atomic_uint64_t &counter) noexcept {
  auto current = counter.load(std::memory_order_relaxed);
  while (current != std::numeric_limits<std::uint64_t>::max()) {
    if (counter.compare_exchange_weak(current, current + 1,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return current + 1;
    }
  }
  return 0;
}

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

struct ProfileGateRegistry {
  struct Gate {
    std::uint64_t token = 0;
  };

  bool block(const SkinProfileId &profile, std::uint64_t token) {
    std::scoped_lock lock(mutex);
    return gates.emplace(profile.opaque, Gate{token}).second;
  }

  bool isBlocked(const SkinProfileId &profile) const {
    std::scoped_lock lock(mutex);
    return gates.contains(profile.opaque);
  }

  void resume(const SkinProfileId &profile, std::uint64_t token) noexcept {
    try {
      std::scoped_lock lock(mutex);
      const auto found = gates.find(profile.opaque);
      if (found != gates.end() && found->second.token == token) {
        gates.erase(found);
      }
    } catch (...) {
    }
  }

  mutable std::mutex mutex;
  std::map<std::string, Gate> gates;
};

template <class Value, std::size_t Capacity> class FixedBoundedQueue {
public:
  static_assert(std::is_nothrow_move_constructible_v<Value>);

  bool full() const noexcept { return size_ == Capacity; }
  bool empty() const noexcept { return size_ == 0; }

  bool push(Value value) noexcept {
    if (size_ == Capacity) {
      return false;
    }

    const auto index = (start_ + size_) % Capacity;
    slots_[index].emplace(std::move(value));
    ++size_;
    return true;
  }

  std::vector<Value> take() {
    std::vector<Value> result;
    result.reserve(size_);
    while (size_ != 0) {
      result.push_back(std::move(*slots_[start_]));
      slots_[start_].reset();
      start_ = (start_ + 1) % Capacity;
      --size_;
    }
    return result;
  }

private:
  std::array<std::optional<Value>, Capacity> slots_;
  std::size_t start_ = 0;
  std::size_t size_ = 0;
};

void boundedBackoff(std::size_t &attempt) {
  ++attempt;
  if (attempt <= 16) {
    std::this_thread::yield();
    return;
  }
  const auto delay = std::min<std::size_t>((attempt - 16) * 50, 1'000);
  std::this_thread::sleep_for(std::chrono::microseconds(delay));
}

class ProfileGateRollback {
public:
  ProfileGateRollback(std::shared_ptr<ProfileGateRegistry> gates,
                      const SkinProfileId &profile,
                      std::uint64_t token) noexcept
      : gates_(std::move(gates)), profile_(&profile), token_(token) {}

  ~ProfileGateRollback() {
    if (armed_) {
      gates_->resume(*profile_, token_);
    }
  }

  void release() noexcept { armed_ = false; }

private:
  std::shared_ptr<ProfileGateRegistry> gates_;
  const SkinProfileId *profile_ = nullptr;
  std::uint64_t token_ = 0;
  bool armed_ = true;
};

} // namespace

struct SkinProfileMutationBarrier::State {
  SkinProfileId profile;
  std::uint64_t token = 0;
  std::weak_ptr<ProfileGateRegistry> registry;
  std::function<void()> resume;

  ~State() {
    if (resume) {
      resume();
    }
  }
};

SkinProfileMutationBarrier::SkinProfileMutationBarrier(
    SkinProfileId profile, std::function<void()> resume)
    : state_(std::make_unique<State>()) {
  state_->profile = std::move(profile);
  state_->resume = std::move(resume);
}

SkinProfileMutationBarrier::SkinProfileMutationBarrier(
    SkinProfileMutationBarrier &&) noexcept = default;

SkinProfileMutationBarrier &SkinProfileMutationBarrier::operator=(
    SkinProfileMutationBarrier &&) noexcept = default;

SkinProfileMutationBarrier::~SkinProfileMutationBarrier() = default;

const SkinProfileId &SkinProfileMutationBarrier::profileId() const noexcept {
  static const SkinProfileId empty;
  return state_ ? state_->profile : empty;
}

struct SkinCommitCoordinator::Impl {
  struct ActivationTransaction {
    SkinActivationClientId client = 0;
    SkinProfileId profile;
    std::uint64_t storeTicket = 0;
    std::optional<CommitActivationResult> terminal;
  };

  struct ProfileTransaction {
    SkinActivationClientId client = 0;
    SkinProfileId profile;
    std::uint64_t ownerTicket = 0;
    std::optional<SkinProfileCommitResult> terminal;
  };

  struct ClientDeliveries {
    FixedBoundedQueue<SkinActivationCompletion, kMaxDeliveryRecordsPerClient>
        activations;
    FixedBoundedQueue<SkinProfileCommitCompletion, kMaxDeliveryRecordsPerClient>
        profiles;
    std::size_t activationOutstanding = 0;
    std::size_t profileOutstanding = 0;
  };

  struct RevalidationSlot {
    std::size_t reservations = 0;
    std::optional<VersionedSkinProfileSettings> request;
  };

  Impl(SkinActivationCommitStore &storeValue,
       ISkinProfileSettingsOwner &ownerValue)
      : store(storeValue), owner(ownerValue),
        mainThread(std::this_thread::get_id()),
        gates(std::make_shared<ProfileGateRegistry>()) {}

  ~Impl() noexcept {
    // Accepted transactions may only be terminalized and acknowledged through
    // their main-thread Store/owner APIs. Never turn wrong-thread teardown or
    // incomplete owning-thread shutdown into silent ticket/lease abandonment.
    if (hasOwnedAcceptedState()) {
      std::terminate();
    }
  }

  bool hasOwnedAcceptedState() const noexcept {
    if (!activations.empty() || !profiles.empty()) {
      return true;
    }
    for (const auto &[client, delivery] : deliveries) {
      (void)client;
      if (delivery.activationOutstanding != 0 ||
          delivery.profileOutstanding != 0 || !delivery.activations.empty() ||
          !delivery.profiles.empty()) {
        return true;
      }
    }
    for (const auto &[profile, slot] : revalidation) {
      (void)profile;
      if (slot.reservations != 0 || slot.request) {
        return true;
      }
    }
    return false;
  }

  bool onMainThread() const noexcept {
    return std::this_thread::get_id() == mainThread;
  }

  bool accepts(SkinActivationClientId client,
               const SkinProfileId &profile) const {
    return !stopping && client != 0 && deliveries.contains(client) &&
           !gates->isBlocked(profile);
  }

  bool recordActivation(SkinActivationClientId client,
                        std::uint64_t coordinatorTicket,
                        CommitActivationResult &&result) noexcept {
    const auto found = deliveries.find(client);
    if (found == deliveries.end()) {
      return true;
    }
    if (found->second.activations.full()) {
      return false;
    }
    return found->second.activations.push(
        SkinActivationCompletion{.client = client,
                                 .ticket = coordinatorTicket,
                                 .result = std::move(result)});
  }

  bool recordProfile(SkinActivationClientId client,
                     std::uint64_t coordinatorTicket,
                     SkinProfileCommitResult &&result) noexcept {
    const auto found = deliveries.find(client);
    if (found == deliveries.end()) {
      return true;
    }
    if (found->second.profiles.full()) {
      return false;
    }
    return found->second.profiles.push(
        SkinProfileCommitCompletion{.client = client,
                                    .ticket = coordinatorTicket,
                                    .result = std::move(result)});
  }

  void recordRevalidation(const SkinProfileId &profile,
                          const CommitActivationResult &result) {
    if (stopping ||
        result.disposition !=
            ActivationCommitDisposition::ProfileCommittedNeedsRevalidation ||
        !result.profileSnapshot) {
      return;
    }
    // Copy before replacing the existing request. An allocation failure leaves
    // both the older repair and the transaction's terminal result intact for
    // retry on the next application poll.
    static_assert(std::is_nothrow_swappable_v<
                  std::optional<VersionedSkinProfileSettings>>);
    std::optional<VersionedSkinProfileSettings> latest{*result.profileSnapshot};
    revalidation.at(profile.opaque).request.swap(latest);
  }

  void releaseRevalidationReservation(const SkinProfileId &profile) noexcept {
    const auto found = revalidation.find(profile.opaque);
    if (found == revalidation.end()) {
      return;
    }
    if (found->second.reservations != 0) {
      --found->second.reservations;
    }
    if (found->second.reservations == 0 && !found->second.request) {
      revalidation.erase(found);
    }
  }

  void pollOnce() {
    for (auto iterator = activations.begin(); iterator != activations.end();) {
      const auto coordinatorTicket = iterator->first;
      const auto client = iterator->second.client;
      const auto storeTicket = iterator->second.storeTicket;
      if (!iterator->second.terminal) {
        CommitActivationResult result;
        try {
          result = store.pollPreparedActivationCommit(storeTicket, owner);
        } catch (...) {
          // The Store still owns the prepared activation unless it returns a
          // terminal value. Retain coordinator ownership and retry next poll.
          ++iterator;
          continue;
        }
        if (result.disposition ==
            ActivationCommitDisposition::PendingProfileSave) {
          ++iterator;
          continue;
        }
        iterator->second.terminal.emplace(std::move(result));
      }

      try {
        recordRevalidation(iterator->second.profile,
                           *iterator->second.terminal);
      } catch (...) {
        ++iterator;
        continue;
      }
      if (!recordActivation(client, coordinatorTicket,
                            std::move(*iterator->second.terminal))) {
        ++iterator;
        continue;
      }
      releaseRevalidationReservation(iterator->second.profile);
      iterator = activations.erase(iterator);
    }

    for (auto iterator = profiles.begin(); iterator != profiles.end();) {
      const auto coordinatorTicket = iterator->first;
      const auto client = iterator->second.client;
      const auto ownerTicket = iterator->second.ownerTicket;
      if (!iterator->second.terminal) {
        SkinProfileCommitResult result;
        try {
          result = owner.pollCommit(ownerTicket);
        } catch (...) {
          // A throwing owner has not supplied a terminal result, so it cannot
          // be acknowledged or forgotten safely.
          ++iterator;
          continue;
        }
        if (result.status == SkinProfileCommitResult::Status::Pending) {
          ++iterator;
          continue;
        }
        iterator->second.terminal.emplace(std::move(result));
      }

      if (!recordProfile(client, coordinatorTicket,
                         std::move(*iterator->second.terminal))) {
        ++iterator;
        continue;
      }
      owner.acknowledgeCommit(ownerTicket);
      iterator = profiles.erase(iterator);
    }
  }

  bool hasPending(const SkinProfileId &profile) const {
    for (const auto &[ticket, transaction] : activations) {
      (void)ticket;
      if (transaction.profile == profile) {
        return true;
      }
    }
    for (const auto &[ticket, transaction] : profiles) {
      (void)ticket;
      if (transaction.profile == profile) {
        return true;
      }
    }
    return false;
  }

  SkinActivationCommitStore &store;
  ISkinProfileSettingsOwner &owner;
  std::thread::id mainThread;
  std::shared_ptr<ProfileGateRegistry> gates;
  bool stopping = false;
  bool stopped = false;
  std::map<SkinActivationClientId, ClientDeliveries> deliveries;
  std::map<std::uint64_t, ActivationTransaction> activations;
  std::map<std::uint64_t, ProfileTransaction> profiles;
  std::map<std::string, RevalidationSlot> revalidation;
};

SkinCommitCoordinator::SkinCommitCoordinator(SkinActivationCommitStore &store,
                                             ISkinProfileSettingsOwner &owner)
    : impl_(std::make_unique<Impl>(store, owner)) {}

SkinCommitCoordinator::~SkinCommitCoordinator() { shutdown(); }

SkinActivationClientId SkinCommitCoordinator::createClient() {
  if (!impl_->onMainThread() || impl_->stopping) {
    return 0;
  }
  const auto client = allocateNeverReused(nextClientId);
  if (client != 0) {
    try {
      impl_->deliveries.try_emplace(client);
    } catch (...) {
      return 0;
    }
  }
  return client;
}

SkinActivationSubmissionResult
SkinCommitCoordinator::submitActivation(SkinActivationClientId client,
                                        PreparedSkinActivation &&prepared) {
  SkinActivationSubmissionResult submission;
  if (!impl_->onMainThread()) {
    submission.diagnostics.push_back(
        diagnostic("skin.commit.wrong_thread",
                   "Skin commits must run on the main thread"));
    return submission;
  }
  if (!impl_->accepts(client, prepared.profileId)) {
    submission.diagnostics.push_back(
        diagnostic("skin.commit.submission_blocked",
                   "The client or profile is not accepting skin commits"));
    return submission;
  }
  auto &clientDelivery = impl_->deliveries.at(client);
  if (clientDelivery.activationOutstanding == kMaxDeliveryRecordsPerClient) {
    submission.diagnostics.push_back(
        diagnostic("skin.commit.client_delivery_full",
                   "The client must consume activation completions before "
                   "submitting more"));
    return submission;
  }
  ++clientDelivery.activationOutstanding;
  const SkinProfileId profile = prepared.profileId;
  auto revalidation = impl_->revalidation.find(profile.opaque);
  if (revalidation == impl_->revalidation.end()) {
    if (impl_->revalidation.size() == kMaxRevalidationProfiles) {
      --clientDelivery.activationOutstanding;
      submission.diagnostics.push_back(diagnostic(
          "skin.commit.revalidation_capacity",
          "Too many profiles have pending activation revalidation work"));
      return submission;
    }
    try {
      revalidation = impl_->revalidation.try_emplace(profile.opaque).first;
    } catch (...) {
      --clientDelivery.activationOutstanding;
      submission.diagnostics.push_back(
          diagnostic("skin.commit.admission_allocation",
                     "Activation commit ownership could not be allocated"));
      return submission;
    }
  }
  ++revalidation->second.reservations;
  const auto coordinatorTicket = allocateNeverReused(nextCoordinatorTicket);
  if (coordinatorTicket == 0) {
    impl_->releaseRevalidationReservation(profile);
    --clientDelivery.activationOutstanding;
    submission.diagnostics.push_back(diagnostic(
        "skin.commit.ticket_exhausted", "Skin commit tickets are exhausted"));
    return submission;
  }
  // Allocate coordinator ownership before the Store can accept and retain the
  // move-only prepared activation. No allocation follows durable admission.
  try {
    impl_->activations.emplace(coordinatorTicket,
                               Impl::ActivationTransaction{.client = client,
                                                           .profile = profile,
                                                           .storeTicket = 0});
  } catch (...) {
    impl_->releaseRevalidationReservation(profile);
    --clientDelivery.activationOutstanding;
    submission.diagnostics.push_back(
        diagnostic("skin.commit.admission_allocation",
                   "Activation commit ownership could not be allocated"));
    return submission;
  }

  CommitActivationResult result;
  try {
    result = impl_->store.beginPreparedActivationCommit(std::move(prepared),
                                                        impl_->owner);
  } catch (...) {
    impl_->activations.erase(coordinatorTicket);
    impl_->releaseRevalidationReservation(profile);
    --clientDelivery.activationOutstanding;
    submission.diagnostics.push_back(
        diagnostic("skin.commit.activation_begin_exception",
                   "Activation commit admission failed unexpectedly"));
    return submission;
  }
  if (result.disposition != ActivationCommitDisposition::PendingProfileSave ||
      result.ticket == 0) {
    impl_->activations.erase(coordinatorTicket);
    impl_->releaseRevalidationReservation(profile);
    --clientDelivery.activationOutstanding;
    submission.diagnostics = std::move(result.diagnostics);
    if (submission.diagnostics.empty()) {
      submission.diagnostics.push_back(diagnostic(
          "skin.commit.activation_not_admitted",
          "Activation was not admitted for durable profile persistence"));
    }
    return submission;
  }

  submission.accepted = true;
  submission.ticket = coordinatorTicket;
  impl_->activations.at(coordinatorTicket).storeTicket = result.ticket;
  return submission;
}

SkinProfileCommitSubmissionResult SkinCommitCoordinator::submitProfileSettings(
    SkinActivationClientId client, const VersionedSkinProfileSettings &base,
    SkinProfileSettings candidate) {
  SkinProfileCommitSubmissionResult submission;
  if (!impl_->onMainThread()) {
    submission.diagnostics.push_back(
        diagnostic("skin.commit.wrong_thread",
                   "Skin commits must run on the main thread"));
    return submission;
  }
  if (!impl_->accepts(client, base.profileId)) {
    submission.diagnostics.push_back(
        diagnostic("skin.commit.submission_blocked",
                   "The client or profile is not accepting skin commits"));
    return submission;
  }
  auto &clientDelivery = impl_->deliveries.at(client);
  if (clientDelivery.profileOutstanding == kMaxDeliveryRecordsPerClient) {
    submission.diagnostics.push_back(diagnostic(
        "skin.commit.client_delivery_full",
        "The client must consume profile completions before submitting more"));
    return submission;
  }
  ++clientDelivery.profileOutstanding;
  const auto coordinatorTicket = allocateNeverReused(nextCoordinatorTicket);
  if (coordinatorTicket == 0) {
    --clientDelivery.profileOutstanding;
    submission.diagnostics.push_back(diagnostic(
        "skin.commit.ticket_exhausted", "Skin commit tickets are exhausted"));
    return submission;
  }
  // Reserve storage before owner admission so every accepted durable ticket
  // is owned even if the process is under allocation pressure.
  try {
    impl_->profiles.emplace(coordinatorTicket,
                            Impl::ProfileTransaction{.client = client,
                                                     .profile = base.profileId,
                                                     .ownerTicket = 0});
  } catch (...) {
    --clientDelivery.profileOutstanding;
    submission.diagnostics.push_back(
        diagnostic("skin.commit.admission_allocation",
                   "Profile commit ownership could not be allocated"));
    return submission;
  }

  SkinProfileCommitResult result;
  try {
    result = impl_->owner.beginCommit(base.profileId, base.generation,
                                      std::move(candidate));
  } catch (...) {
    impl_->profiles.erase(coordinatorTicket);
    --clientDelivery.profileOutstanding;
    submission.diagnostics.push_back(
        diagnostic("skin.commit.profile_begin_exception",
                   "Profile commit admission failed unexpectedly"));
    return submission;
  }
  if (result.status != SkinProfileCommitResult::Status::Pending ||
      result.ticket == 0) {
    impl_->profiles.erase(coordinatorTicket);
    --clientDelivery.profileOutstanding;
    if (result.failure) {
      submission.diagnostics.push_back(*result.failure);
    }
    if (submission.diagnostics.empty()) {
      submission.diagnostics.push_back(diagnostic(
          result.status == SkinProfileCommitResult::Status::GenerationChanged
              ? "skin.commit.profile_generation_changed"
              : "skin.commit.profile_not_admitted",
          result.status == SkinProfileCommitResult::Status::GenerationChanged
              ? "The profile generation changed before commit admission"
              : "The profile commit was not admitted"));
    }
    return submission;
  }

  submission.accepted = true;
  submission.ticket = coordinatorTicket;
  impl_->profiles.at(coordinatorTicket).ownerTicket = result.ticket;
  return submission;
}

void SkinCommitCoordinator::poll() {
  if (!impl_->onMainThread() || impl_->stopped) {
    return;
  }
  impl_->pollOnce();
}

std::vector<SkinActivationCompletion>
SkinCommitCoordinator::takeCompletions(SkinActivationClientId client) {
  std::vector<SkinActivationCompletion> result;
  if (!impl_->onMainThread()) {
    return result;
  }
  auto found = impl_->deliveries.find(client);
  if (found == impl_->deliveries.end()) {
    return result;
  }
  auto completions = found->second.activations.take();
  found->second.activationOutstanding -= completions.size();
  return completions;
}

std::vector<SkinProfileCommitCompletion>
SkinCommitCoordinator::takeProfileCompletions(SkinActivationClientId client) {
  std::vector<SkinProfileCommitCompletion> result;
  if (!impl_->onMainThread()) {
    return result;
  }
  auto found = impl_->deliveries.find(client);
  if (found == impl_->deliveries.end()) {
    return result;
  }
  auto completions = found->second.profiles.take();
  found->second.profileOutstanding -= completions.size();
  return completions;
}

void SkinCommitCoordinator::detachClient(
    SkinActivationClientId client) noexcept {
  try {
    if (!impl_->onMainThread()) {
      return;
    }
    impl_->deliveries.erase(client);
  } catch (...) {
  }
}

std::vector<VersionedSkinProfileSettings>
SkinCommitCoordinator::takeRevalidationRequests() {
  std::vector<VersionedSkinProfileSettings> result;
  if (!impl_->onMainThread()) {
    return result;
  }
  std::size_t requestCount = 0;
  for (const auto &[profile, slot] : impl_->revalidation) {
    (void)profile;
    requestCount += slot.request.has_value() ? 1 : 0;
  }
  result.reserve(requestCount);
  for (auto iterator = impl_->revalidation.begin();
       iterator != impl_->revalidation.end();) {
    if (iterator->second.request) {
      result.push_back(std::move(*iterator->second.request));
      iterator->second.request.reset();
    }
    if (iterator->second.reservations == 0) {
      iterator = impl_->revalidation.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return result;
}

BeginSkinProfileMutationResult
SkinCommitCoordinator::beginProfileMutation(const SkinProfileId &profile) {
  BeginSkinProfileMutationResult result;
  if (!impl_->onMainThread()) {
    result.error = "Profile mutation barriers must begin on the main thread";
    return result;
  }
  if (impl_->stopping) {
    result.error = "Skin commit coordinator is shutting down";
    return result;
  }
  const auto token = allocateNeverReused(nextBarrierToken);
  if (token == 0 || !impl_->gates->block(profile, token)) {
    result.error = token == 0 ? "Profile mutation barrier IDs are exhausted"
                              : "Profile is already blocked for mutation";
    return result;
  }
  ProfileGateRollback rollback(impl_->gates, profile, token);

  std::weak_ptr<ProfileGateRegistry> weakGates = impl_->gates;
  SkinProfileMutationBarrier barrier(profile, [weakGates, profile, token] {
    if (auto gates = weakGates.lock()) {
      gates->resume(profile, token);
    }
  });
  barrier.state_->token = token;
  barrier.state_->registry = impl_->gates;
  rollback.release();
  std::size_t pendingAttempts = 0;
  while (impl_->hasPending(profile)) {
    impl_->pollOnce();
    boundedBackoff(pendingAttempts);
  }

  result.barrier.emplace(std::move(barrier));
  return result;
}

void SkinCommitCoordinator::finishProfileMutation(
    SkinProfileMutationBarrier &&barrier, bool mutationSucceeded,
    bool profileStillExists) noexcept {
  try {
    if (!impl_->onMainThread() || !barrier.state_) {
      return;
    }
    const auto registry = barrier.state_->registry.lock();
    if (!registry || registry.get() != impl_->gates.get()) {
      return;
    }
    if (mutationSucceeded) {
      try {
        impl_->store.removeProfileActivations(barrier.state_->profile);
      } catch (...) {
        // Destruction runs the still-armed resume callback. The Store contract
        // preserves the previous activation when removal fails.
        barrier.state_.reset();
        return;
      }
    }
    const auto profile = barrier.state_->profile;
    const auto token = barrier.state_->token;
    barrier.state_->resume = {};
    barrier.state_.reset();
    if (!mutationSucceeded || profileStillExists) {
      impl_->gates->resume(profile, token);
    }
  } catch (...) {
  }
}

void SkinCommitCoordinator::shutdown() noexcept {
  try {
    if (impl_->stopped || !impl_->onMainThread()) {
      return;
    }
    impl_->stopping = true;
    impl_->deliveries.clear();
    impl_->revalidation.clear();
    std::size_t pendingAttempts = 0;
    while (!impl_->activations.empty() || !impl_->profiles.empty()) {
      impl_->pollOnce();
      // Store and owner contracts guarantee accepted tickets eventually become
      // terminal. Backoff is bounded to avoid a hot shutdown spin without
      // imposing a timeout that would drop an unacknowledged ticket or lease.
      boundedBackoff(pendingAttempts);
    }
    impl_->stopped = true;
  } catch (...) {
    // Dependencies promise no-throw polling/acknowledgement semantics for
    // accepted work. Any non-allocation polling exception stays owned and is
    // retried by pollOnce rather than acknowledged prematurely.
  }
}

} // namespace skin
