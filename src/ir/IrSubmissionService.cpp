#include "IrSubmissionService.h"

#include "IrHttpClient.h"
#include "IrScoreReconciliation.h"
#include "../repositories/ReplayRepository.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ir {
namespace {

using SteadyTimePoint = IrSubmissionServiceOptions::SteadyTimePoint;
using StatusKey = std::pair<std::string, std::string>;

constexpr std::int64_t kSucceededRetentionMs = 7LL * 24 * 60 * 60 * 1000;
constexpr std::size_t kWorkerBatchSize = 64;
constexpr auto kReconciliationCooldown = std::chrono::seconds(60);

std::int64_t systemWallNowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t safeNow(const IrSubmissionServiceOptions &options) {
  try {
    return std::max<std::int64_t>(0, options.wallNowUnixMillis
                                         ? options.wallNowUnixMillis()
                                         : systemWallNowMillis());
  } catch (...) {
    return systemWallNowMillis();
  }
}

SteadyTimePoint monotonicNow(const IrSubmissionServiceOptions &options) {
  try {
    return options.monotonicNow ? options.monotonicNow()
                                : std::chrono::steady_clock::now();
  } catch (...) {
    return std::chrono::steady_clock::now();
  }
}

std::int64_t safeAdd(std::int64_t base, std::int64_t delay) {
  if (delay <= 0) {
    return base;
  }
  if (base > std::numeric_limits<std::int64_t>::max() - delay) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return base + delay;
}

std::int64_t backoffDelay(int consecutiveFailures) {
  constexpr std::array<std::int64_t, 5> delays{10'000, 30'000, 120'000, 600'000,
                                               3'600'000};
  const std::size_t index = static_cast<std::size_t>(std::clamp(
      consecutiveFailures - 1, 0, static_cast<int>(delays.size() - 1)));
  return delays[index];
}

std::int64_t remotePollDelay(int completedPollCount) {
  constexpr std::array<std::int64_t, 6> delays{200, 1'000, 2'000,
                                               3'000, 5'000, 10'000};
  const std::size_t index = static_cast<std::size_t>(std::clamp(
      completedPollCount, 0, static_cast<int>(delays.size() - 1)));
  return delays[index];
}

std::string lookupCredential(const IrSubmissionServiceOptions &options,
                             std::string_view profileId,
                             std::string_view providerId) {
  try {
    return options.credentialLookup
               ? options.credentialLookup(profileId, providerId)
               : std::string{};
  } catch (...) {
    return {};
  }
}

struct CredentialFingerprint {
  bool present = false;
  std::size_t size = 0;
  std::size_t hash = 0;

  bool operator==(const CredentialFingerprint &) const = default;
};

CredentialFingerprint fingerprint(std::string_view credential) {
  return {.present = !credential.empty(),
          .size = credential.size(),
          .hash = credential.empty()
                      ? 0
                      : std::hash<std::string_view>{}(credential)};
}

std::string redactCredential(std::string value, std::string_view credential) {
  if (!credential.empty()) {
    std::size_t offset = 0;
    while ((offset = value.find(credential, offset)) != std::string::npos) {
      value.replace(offset, credential.size(), "[redacted]");
      offset += 10;
    }
  }
  return sanitizeDiagnostic(value);
}

bool responseIdentityContainsCredential(const DeliveryOutcome &outcome,
                                        std::string_view credential) {
  if (credential.empty()) {
    return false;
  }
  const auto containsCredential = [&](const auto &value) {
    return value && value->find(credential) != std::string::npos;
  };
  return (outcome.remoteUserId &&
          std::to_string(*outcome.remoteUserId) == credential) ||
         containsCredential(outcome.remoteJobId) ||
         containsCredential(outcome.remoteOrigin) ||
         containsCredential(outcome.remoteScoreId) ||
         std::ranges::any_of(outcome.remoteScoreIds, [&](const auto &value) {
           return value.find(credential) != std::string::npos;
         });
}

IrAttemptStatusSnapshot snapshotFrom(const IrOutboxEntry &entry,
                                     std::uint64_t revision) {
  const IrActiveRequestKind activeRequest =
      entry.state != IrOutboxState::Uploading
          ? IrActiveRequestKind::None
          : (!entry.remoteJobId.empty() && !entry.remoteOrigin.empty()
                 ? IrActiveRequestKind::Poll
                 : IrActiveRequestKind::Submit);
  return {.revision = revision,
          .found = true,
          .rowId = entry.id,
          .state = entry.state,
          .activeRequest = activeRequest,
          .requestAttemptCount = entry.requestAttemptCount,
          .consecutiveFailureCount = entry.consecutiveFailureCount,
          .nextAttemptAtUnixMillis = entry.nextAttemptAtUnixMillis,
          .errorCode = entry.lastErrorCode,
          .diagnostic = sanitizeDiagnostic(entry.lastErrorMessage)};
}

bool providerCanSubmit(const IrDriverRegistry &drivers,
                       std::string_view providerId) {
  const auto driver = drivers.find(providerId);
  if (!driver) {
    return false;
  }
  const auto capabilities = driver->capabilities();
  return !capabilities.readOnly && capabilities.scoreSubmission;
}

bool providerCanReconcile(const IrDriverRegistry &drivers,
                          std::string_view providerId) {
  const auto driver = drivers.find(providerId);
  return driver && driver->capabilities().scoreReconciliation;
}

std::optional<std::string> requestOrigin(const IrOutboxEntry &entry,
                                         const IrProviderSettings &settings) {
  if (!entry.remoteOrigin.empty()) {
    return normalizeServerOrigin(entry.remoteOrigin);
  }
  return normalizeServerOrigin(settings.serverOrigin);
}

std::vector<std::string>
providersWithChangedRuntime(const IrActiveProfileConfig &previous,
                            const IrActiveProfileConfig &next) {
  if (previous.profileId != next.profileId) {
    return {};
  }

  std::vector<std::string> changed;
  for (const auto &[providerId, nextSettings] : next.providers) {
    if (!nextSettings.enabled) {
      continue;
    }
    const auto previousProvider = previous.providers.find(providerId);
    if (previousProvider == previous.providers.end() ||
        !previousProvider->second.enabled ||
        previousProvider->second.serverOrigin != nextSettings.serverOrigin) {
      changed.push_back(providerId);
    }
  }
  return changed;
}

} // namespace

struct IrSubmissionService::Impl {
  struct ReconciliationCommand {
    std::string providerId;
    std::string profileId;
    std::string serverOrigin;
    std::uint64_t serviceGeneration = 0;
    std::uint64_t credentialGeneration = 0;
  };

  ReplayRepository &repository;
  const IrDriverRegistry &drivers;
  IrHttpClient &http;
  IrSubmissionServiceOptions options;

  mutable std::mutex mutex;
  std::condition_variable_any condition;
  std::jthread worker;
  bool started = false;
  bool stopped = false;
  bool profilePaused = true;
  bool applicationActive = true;
  bool workerBusy = false;
  bool configurationDirty = false;
  std::uint64_t generation = 0;
  std::uint64_t credentialGeneration = 0;
  std::uint64_t wakeRevision = 0;
  std::uint64_t statusRevision = 0;
  std::uint64_t reconciliationRevision = 0;
  IrActiveProfileConfig profile;
  std::stop_source requestStop;
  std::optional<ReconciliationCommand> pendingReconciliation;
  std::optional<ReconciliationCommand> activeReconciliation;
  std::map<StatusKey, IrAttemptStatusSnapshot> statusSnapshots;
  std::map<std::int64_t, StatusKey> rowKeys;
  std::map<std::string, IrOutboxCounts, std::less<>> countSnapshots;
  std::map<std::string, CredentialFingerprint, std::less<>> credentials;
  std::map<std::string, IrReconciliationStatusSnapshot, std::less<>>
      reconciliationSnapshots;

  Impl(ReplayRepository &repositoryValue, const IrDriverRegistry &driversValue,
       IrHttpClient &httpValue, IrSubmissionServiceOptions optionsValue)
      : repository(repositoryValue), drivers(driversValue), http(httpValue),
        options(std::move(optionsValue)) {}

  void publishReconciliationPhaseLocked(std::string_view providerId,
                                        IrReconciliationPhase phase,
                                        std::string diagnostic = {}) {
    auto &snapshot = reconciliationSnapshots[std::string(providerId)];
    if (phase == IrReconciliationPhase::Queued) {
      snapshot = {};
    }
    snapshot.revision = ++reconciliationRevision;
    snapshot.phase = phase;
    snapshot.diagnostic = sanitizeDiagnostic(diagnostic);
  }

  void publishReconciliationSuccessLocked(
      std::string_view providerId,
      const IrRemoteSnapshotApplyOutcome &outcome,
      SteadyTimePoint nextAllowedAt) {
    auto &snapshot = reconciliationSnapshots[std::string(providerId)];
    snapshot.revision = ++reconciliationRevision;
    snapshot.phase = IrReconciliationPhase::Succeeded;
    snapshot.remoteScores = outcome.remoteScoreCount;
    snapshot.remoteScoresAdded = outcome.remoteScoresAdded;
    snapshot.remoteScoresRemoved = outcome.remoteScoresRemoved;
    snapshot.receiptsUpserted = outcome.receiptsUpserted;
    snapshot.receiptsDeleted = outcome.receiptsDeleted;
    snapshot.outboxRowsSettled = outcome.outboxRowsSettled;
    snapshot.ambiguousReceiptsPreserved =
        outcome.ambiguousReceiptsPreserved;
    snapshot.nextAllowedAt = nextAllowedAt;
    snapshot.diagnostic.clear();
  }

  void publishReconciliationFailureLocked(std::string_view providerId,
                                          std::string diagnostic,
                                          SteadyTimePoint nextAllowedAt) {
    auto &snapshot = reconciliationSnapshots[std::string(providerId)];
    snapshot.revision = ++reconciliationRevision;
    snapshot.phase = IrReconciliationPhase::Failed;
    snapshot.nextAllowedAt = nextAllowedAt;
    snapshot.diagnostic = sanitizeDiagnostic(diagnostic);
  }

  void signal() {
    {
      std::lock_guard lock(mutex);
      ++wakeRevision;
    }
    condition.notify_all();
    if (options.wake) {
      try {
        options.wake();
      } catch (...) {
      }
    }
  }

  void publishLocked(const IrOutboxEntry &entry) {
    const StatusKey key{entry.providerId, entry.attemptId};
    if (!statusSnapshots.contains(key) &&
        statusSnapshots.size() >= kMaximumAttemptStatusSnapshots) {
      const auto oldest = statusSnapshots.begin();
      rowKeys.erase(oldest->second.rowId);
      statusSnapshots.erase(oldest);
    }
    ++statusRevision;
    statusSnapshots[key] = snapshotFrom(entry, statusRevision);
    rowKeys[entry.id] = key;
  }

  void eraseLocked(const StatusKey &key, std::int64_t rowId) {
    statusSnapshots.erase(key);
    rowKeys.erase(rowId);
    ++statusRevision;
  }

  void refreshCount(std::string_view providerId,
                    std::uint64_t expectedGeneration) {
    IrOutboxCounts loaded = repository.CountIrOutbox(providerId);
    std::lock_guard lock(mutex);
    if (generation == expectedGeneration) {
      countSnapshots[std::string(providerId)] = std::move(loaded);
    }
  }

  void refreshEntry(const StatusKey &key, std::uint64_t expectedGeneration) {
    IrOutboxReadOutcome loaded = repository.LoadIrOutbox(key.first, key.second);
    std::lock_guard lock(mutex);
    if (generation != expectedGeneration) {
      return;
    }
    if (loaded.status == IrOutboxReadStatus::Found && loaded.entry) {
      publishLocked(*loaded.entry);
    } else if (loaded.status == IrOutboxReadStatus::NotFound) {
      const auto found = statusSnapshots.find(key);
      if (found != statusSnapshots.end()) {
        eraseLocked(key, found->second.rowId);
      }
    }
  }

  void loadProfileSnapshots(const IrActiveProfileConfig &config,
                            std::uint64_t expectedGeneration,
                            bool refreshCredentials = true) {
    IrOutboxBatchOutcome entries =
        repository.ListIrOutbox(kMaximumAttemptStatusSnapshots);
    std::map<std::string, IrOutboxCounts, std::less<>> counts;
    std::map<std::string, CredentialFingerprint, std::less<>> fingerprints;
    for (const auto &[providerId, settings] : config.providers) {
      (void)settings;
      counts.emplace(providerId, repository.CountIrOutbox(providerId));
      if (refreshCredentials) {
        const std::string credential =
            lookupCredential(options, config.profileId, providerId);
        fingerprints.emplace(providerId, fingerprint(credential));
      }
    }
    std::lock_guard lock(mutex);
    if (generation != expectedGeneration) {
      return;
    }
    statusSnapshots.clear();
    rowKeys.clear();
    countSnapshots = std::move(counts);
    if (refreshCredentials) {
      credentials = std::move(fingerprints);
    }
    ++statusRevision;
    if (entries.status == IrOutboxBatchStatus::Loaded) {
      for (auto iterator = entries.entries.rbegin();
           iterator != entries.entries.rend(); ++iterator) {
        publishLocked(*iterator);
      }
    }
  }

  void prepareProfile(IrActiveProfileConfig config) {
    const std::int64_t now = safeNow(options);
    repository.RecoverStaleIrOutbox(now);
    repository.PurgeSucceededIrOutbox(
        now > kSucceededRetentionMs ? now - kSucceededRetentionMs : 0);
    std::uint64_t currentGeneration = 0;
    {
      std::lock_guard lock(mutex);
      profile = std::move(config);
      ++generation;
      currentGeneration = generation;
      profilePaused = false;
      configurationDirty = false;
      pendingReconciliation.reset();
      activeReconciliation.reset();
      reconciliationSnapshots.clear();
      ++reconciliationRevision;
    }
    loadProfileSnapshots(profile, currentGeneration);
    signal();
  }

  void waitForSignal(std::unique_lock<std::mutex> &lock,
                     std::stop_token stopToken,
                     std::optional<SteadyTimePoint> deadline) {
    const std::uint64_t observed = wakeRevision;
    if (options.waitUntil) {
      lock.unlock();
      try {
        options.waitUntil(stopToken, deadline);
      } catch (...) {
      }
      lock.lock();
      return;
    }
    const auto predicate = [&] {
      return stopToken.stop_requested() || stopped || wakeRevision != observed;
    };
    if (deadline) {
      condition.wait_until(lock, *deadline, predicate);
    } else {
      condition.wait(lock, predicate);
    }
  }

  void processCredentialChanges(std::uint64_t expectedGeneration,
                                const IrActiveProfileConfig &config) {
    bool dirty = false;
    {
      std::lock_guard lock(mutex);
      if (generation != expectedGeneration) {
        return;
      }
      dirty = std::exchange(configurationDirty, false);
    }
    if (!dirty) {
      return;
    }

    const std::int64_t now = safeNow(options);
    bool changedAny = false;
    for (const auto &[providerId, settings] : config.providers) {
      (void)settings;
      const std::string credential =
          lookupCredential(options, config.profileId, providerId);
      const CredentialFingerprint next = fingerprint(credential);
      bool changed = false;
      {
        std::lock_guard lock(mutex);
        if (generation != expectedGeneration) {
          return;
        }
        const auto found = credentials.find(providerId);
        changed = found == credentials.end() || found->second != next;
        credentials[providerId] = next;
      }
      if (!changed) {
        continue;
      }
      changedAny = true;
      if (next.present) {
        repository.UnblockIrOutbox(providerId, now);
      }
      if (options.credentialChanged) {
        try {
          options.credentialChanged(config.profileId, providerId);
        } catch (...) {
        }
      }
    }
    if (changedAny) {
      loadProfileSnapshots(config, expectedGeneration);
    }
  }

  [[nodiscard]] bool
  requestMayStartLocked(std::uint64_t expectedGeneration) const noexcept {
    return generation == expectedGeneration && !stopped && !profilePaused &&
           applicationActive;
  }

  [[nodiscard]] bool reconciliationIsCurrentLocked(
      const ReconciliationCommand &command) const {
    if (!requestMayStartLocked(command.serviceGeneration) ||
        credentialGeneration != command.credentialGeneration ||
        profile.profileId != command.profileId) {
      return false;
    }
    const auto provider = profile.providers.find(command.providerId);
    if (provider == profile.providers.end() || !provider->second.enabled) {
      return false;
    }
    const auto origin = normalizeServerOrigin(provider->second.serverOrigin);
    return origin && *origin == command.serverOrigin;
  }

  void failReconciliation(const ReconciliationCommand &command,
                          std::string diagnostic) {
    const SteadyTimePoint nextAllowedAt =
        monotonicNow(options) + kReconciliationCooldown;
    std::lock_guard lock(mutex);
    if (activeReconciliation &&
        activeReconciliation->providerId == command.providerId) {
      activeReconciliation.reset();
    }
    if (generation == command.serviceGeneration) {
      publishReconciliationFailureLocked(command.providerId,
                                         std::move(diagnostic), nextAllowedAt);
    }
  }

  void runReconciliation(const ReconciliationCommand &command) {
    const std::string credential =
        lookupCredential(options, command.profileId, command.providerId);
    std::stop_token requestToken;
    {
      std::lock_guard lock(mutex);
      if (reconciliationIsCurrentLocked(command) && !credential.empty()) {
        requestStop = std::stop_source{};
        requestToken = requestStop.get_token();
        publishReconciliationPhaseLocked(command.providerId,
                                         IrReconciliationPhase::Fetching7K);
      }
    }
    if (!requestToken.stop_possible()) {
      failReconciliation(command,
                         credential.empty()
                             ? "IR API key is required"
                             : "IR reconciliation was cancelled");
      return;
    }

    const IrProviderRuntimeConfig runtime{
        .profileId = command.profileId,
        .serverOrigin = command.serverOrigin,
        .apiKey = credential,
    };
    IrUserScoreSnapshotOutcome fetched = drivers.fetchUserScoreSnapshot(
        command.providerId, runtime, http, requestToken,
        [this, command](std::string_view game, int, int) {
          std::lock_guard lock(mutex);
          if (!activeReconciliation ||
              activeReconciliation->providerId != command.providerId ||
              !reconciliationIsCurrentLocked(command)) {
            return;
          }
          std::optional<IrReconciliationPhase> phase;
          if (game == "bms-7k") {
            phase = IrReconciliationPhase::Fetching7K;
          } else if (game == "bms-14k") {
            phase = IrReconciliationPhase::Fetching14K;
          }
          if (!phase) {
            return;
          }
          auto &snapshot = reconciliationSnapshots[command.providerId];
          if (snapshot.phase != *phase) {
            publishReconciliationPhaseLocked(command.providerId, *phase);
          }
        });
    fetched.code = redactCredential(std::move(fetched.code), credential);
    fetched.diagnostic =
        redactCredential(std::move(fetched.diagnostic), credential);

    {
      std::lock_guard lock(mutex);
      if (!reconciliationIsCurrentLocked(command) ||
          requestToken.stop_requested()) {
        fetched.status = IrUserScoreSnapshotStatus::Cancelled;
        fetched.snapshot.reset();
        fetched.diagnostic = "IR reconciliation was cancelled";
      }
    }
    if (fetched.status != IrUserScoreSnapshotStatus::Succeeded ||
        !fetched.snapshot) {
      failReconciliation(
          command,
          fetched.diagnostic.empty() ? "IR reconciliation failed"
                                     : std::move(fetched.diagnostic));
      return;
    }

    {
      std::lock_guard lock(mutex);
      if (!reconciliationIsCurrentLocked(command)) {
        fetched.snapshot.reset();
      } else {
        publishReconciliationPhaseLocked(command.providerId,
                                         IrReconciliationPhase::Applying);
      }
    }
    if (!fetched.snapshot) {
      failReconciliation(command, "IR reconciliation was cancelled");
      return;
    }

    const std::int64_t synchronizedAt = safeNow(options);
    IrReconciliationReadOutcome candidates =
        repository.LoadIrReconciliationCandidates(command.providerId,
                                                   command.serverOrigin);
    if (candidates.status != IrReconciliationReadOutcome::Status::Loaded) {
      failReconciliation(
          command, candidates.diagnostic.empty()
                       ? "Could not load IR reconciliation candidates"
                       : std::move(candidates.diagnostic));
      return;
    }
    IrScoreReconciliationPlan plan = planScoreReconciliation(
        command.providerId, command.serverOrigin, candidates.candidates,
        fetched.snapshot->scores, synchronizedAt);
    if (plan.status != IrScoreReconciliationPlan::Status::Planned) {
      failReconciliation(command,
                         plan.diagnostic.empty()
                             ? "Could not plan IR reconciliation"
                             : std::move(plan.diagnostic));
      return;
    }

    IrRemoteSnapshotMutation mutation{
        .providerId = command.providerId,
        .serverOrigin = command.serverOrigin,
        .synchronizedAtUnixMillis = synchronizedAt,
        .scores = std::move(fetched.snapshot->scores),
        .upsertedReceipts = std::move(plan.upsertedReceipts),
        .deletedReceiptIds = std::move(plan.deletedReceiptIds),
        .settledOutboxRowIds = std::move(plan.settledOutboxRowIds),
        .purgedSucceededOutboxRowIds =
            std::move(plan.purgedSucceededOutboxRowIds),
    };
    IrRemoteSnapshotApplyOutcome applied;
    bool applyAttempted = false;
    {
      std::lock_guard lock(mutex);
      if (reconciliationIsCurrentLocked(command)) {
        applyAttempted = true;
        applied = repository.ApplyIrRemoteSnapshot(mutation);
      }
    }
    if (!applyAttempted) {
      failReconciliation(command, "IR reconciliation was cancelled");
      return;
    }
    if (applied.status != IrRemoteSnapshotApplyOutcome::Status::Applied) {
      failReconciliation(command,
                         applied.diagnostic.empty()
                             ? "Could not apply IR reconciliation"
                             : std::move(applied.diagnostic));
      return;
    }

    if (options.remoteSnapshotApplied) {
      std::string projectionDiagnostic;
      bool projected = false;
      try {
        projected = options.remoteSnapshotApplied(
            command.profileId, command.providerId, command.serverOrigin,
            applied.syncGeneration, mutation.scores, projectionDiagnostic);
      } catch (...) {
        projectionDiagnostic =
            "Synchronized IR scores could not be imported";
      }
      if (!projected) {
        failReconciliation(
            command,
            projectionDiagnostic.empty()
                ? "Synchronized IR scores could not be imported"
                : sanitizeDiagnostic(projectionDiagnostic));
        return;
      }
    }

    IrActiveProfileConfig refreshedProfile;
    {
      std::lock_guard lock(mutex);
      if (generation == command.serviceGeneration) {
        refreshedProfile = profile;
      }
    }
    if (!refreshedProfile.profileId.empty()) {
      loadProfileSnapshots(refreshedProfile, command.serviceGeneration, false);
    }

    const SteadyTimePoint nextAllowedAt =
        monotonicNow(options) + kReconciliationCooldown;
    std::lock_guard lock(mutex);
    if (activeReconciliation &&
        activeReconciliation->providerId == command.providerId) {
      activeReconciliation.reset();
    }
    if (generation == command.serviceGeneration) {
      publishReconciliationSuccessLocked(command.providerId, applied,
                                         nextAllowedAt);
    }
  }

  std::optional<std::int64_t>
  nextEligibleFutureTime(std::uint64_t expectedGeneration,
                         const IrActiveProfileConfig &config,
                         std::int64_t now) {
    std::optional<std::int64_t> result;
    for (const auto &[providerId, settings] : config.providers) {
      if (!settings.enabled || !providerCanSubmit(drivers, providerId)) {
        continue;
      }
      const auto next = repository.NextIrOutboxAttemptAfter(providerId, now);
      if (next && (!result || *next < *result)) {
        result = next;
      }
    }
    {
      std::lock_guard lock(mutex);
      if (generation != expectedGeneration) {
        return std::nullopt;
      }
    }
    return result;
  }

  IrOutboxDeliveryUpdate deliveryUpdate(const IrOutboxEntry &claimed,
                                        IrOutboxState originalState,
                                        const DeliveryOutcome &outcome,
                                        std::int64_t now) {
    IrOutboxDeliveryUpdate update{
        .rowId = claimed.id,
        .consecutiveFailureCount = 0,
        .lastErrorCode = outcome.code,
        .lastErrorMessage = sanitizeDiagnostic(outcome.diagnostic),
        .updatedAtUnixMillis = now,
    };
    const auto preserveRemote = [&] {
      if (originalState == IrOutboxState::AwaitingRemoteResult) {
        update.remotePollCount = claimed.remotePollCount;
        update.remoteJobId = claimed.remoteJobId;
        update.remoteOrigin = claimed.remoteOrigin;
      }
    };
    switch (outcome.status) {
    case DeliveryStatus::Succeeded:
      update.nextState = IrOutboxState::Succeeded;
      update.completedAtUnixMillis = now;
      break;
    case DeliveryStatus::Deferred:
      update.nextState = IrOutboxState::AwaitingRemoteResult;
      update.remotePollCount = 0;
      update.nextAttemptAtUnixMillis = safeAdd(now, remotePollDelay(0));
      update.remoteJobId = outcome.remoteJobId;
      update.remoteOrigin = outcome.remoteOrigin;
      break;
    case DeliveryStatus::Ongoing:
      update.nextState = IrOutboxState::AwaitingRemoteResult;
      preserveRemote();
      if (update.remotePollCount < std::numeric_limits<int>::max()) {
        ++update.remotePollCount;
      }
      update.nextAttemptAtUnixMillis =
          safeAdd(now, remotePollDelay(update.remotePollCount));
      break;
    case DeliveryStatus::TransientFailure: {
      update.nextState = originalState;
      preserveRemote();
      update.consecutiveFailureCount = claimed.consecutiveFailureCount + 1;
      std::int64_t delay = backoffDelay(update.consecutiveFailureCount);
      if (outcome.retryAfterDelay) {
        const auto retryAfter = outcome.retryAfterDelay->count();
        if (retryAfter > delay) {
          delay = retryAfter;
        }
      }
      update.nextAttemptAtUnixMillis = safeAdd(now, delay);
      break;
    }
    case DeliveryStatus::BlockedConfiguration:
      update.nextState = IrOutboxState::BlockedConfiguration;
      preserveRemote();
      break;
    case DeliveryStatus::PermanentFailure:
    case DeliveryStatus::Unsupported:
      update.nextState = IrOutboxState::FailedPermanent;
      break;
    case DeliveryStatus::Cancelled:
      update.nextState = originalState;
      update.consecutiveFailureCount = claimed.consecutiveFailureCount;
      update.nextAttemptAtUnixMillis = now;
      update.lastErrorCode.clear();
      update.lastErrorMessage.clear();
      preserveRemote();
      break;
    }
    return update;
  }

  std::optional<std::int64_t> runOne(std::uint64_t expectedGeneration,
                                     const IrActiveProfileConfig &config) {
    processCredentialChanges(expectedGeneration, config);
    const std::int64_t now = safeNow(options);
    IrOutboxBatchOutcome due{.status = IrOutboxBatchStatus::Loaded};
    for (const auto &[providerId, settings] : config.providers) {
      if (!settings.enabled || !providerCanSubmit(drivers, providerId)) {
        continue;
      }
      const auto providerDue =
          repository.ListDueIrOutbox(providerId, now, kWorkerBatchSize);
      if (providerDue.status != IrOutboxBatchStatus::Loaded) {
        return safeAdd(now, 1'000);
      }
      due.entries.insert(due.entries.end(), providerDue.entries.begin(),
                         providerDue.entries.end());
    }
    std::ranges::sort(due.entries, [](const IrOutboxEntry &left,
                                     const IrOutboxEntry &right) {
      const std::int64_t leftTime = left.nextAttemptAtUnixMillis.value_or(0);
      const std::int64_t rightTime = right.nextAttemptAtUnixMillis.value_or(0);
      return leftTime == rightTime ? left.id < right.id : leftTime < rightTime;
    });
    if (due.entries.size() > kWorkerBatchSize) {
      due.entries.resize(kWorkerBatchSize);
    }

    for (const IrOutboxEntry &firstDue : due.entries) {
      const auto provider = config.providers.find(firstDue.providerId);
      if (provider == config.providers.end() || !provider->second.enabled ||
          !providerCanSubmit(drivers, firstDue.providerId)) {
        continue;
      }

      const bool polling =
          firstDue.state == IrOutboxState::AwaitingRemoteResult;
      std::vector<IrOutboxEntry> providerDue;
      providerDue.reserve(due.entries.size());
      for (const auto &candidate : due.entries) {
        if (candidate.providerId == firstDue.providerId &&
            (candidate.state == IrOutboxState::AwaitingRemoteResult) ==
                polling) {
          providerDue.push_back(candidate);
        }
      }
      const auto plan = drivers.planBatch(firstDue.providerId, providerDue);
      if (plan.status != IrOutboxBatchPlanStatus::Planned) {
        if (!plan.rejectedRowId) {
          return safeAdd(now, 1'000);
        }
        const auto rejectedDue = std::ranges::find(
            providerDue, *plan.rejectedRowId, &IrOutboxEntry::id);
        if (rejectedDue == providerDue.end()) {
          return safeAdd(now, 1'000);
        }
        {
          std::lock_guard lock(mutex);
          if (!requestMayStartLocked(expectedGeneration)) {
            return std::nullopt;
          }
        }
        const IrOutboxClaimRequest request{
            .rowId = rejectedDue->id, .expectedState = rejectedDue->state};
        auto rejected =
            repository.ClaimIrOutboxBatch(std::span(&request, 1), now);
        if (rejected.status != IrOutboxClaimStatus::Claimed ||
            rejected.entries.size() != 1) {
          return safeAdd(now, 1'000);
        }
        {
          std::lock_guard lock(mutex);
          if (generation != expectedGeneration) {
            return std::nullopt;
          }
          publishLocked(rejected.entries.front());
        }
        const DeliveryOutcome planningFailure{
            .status = DeliveryStatus::PermanentFailure,
            .code = plan.status == IrOutboxBatchPlanStatus::Unsupported
                        ? "unsupported_batch_plan"
                        : "invalid_batch_plan",
            .diagnostic =
                "IR provider rejected an identified row during batch planning",
        };
        const auto update =
            deliveryUpdate(rejected.entries.front(), rejectedDue->state,
                           planningFailure, now);
        const auto applied = repository.ApplyIrOutboxDelivery(update);
        refreshEntry({rejectedDue->providerId, rejectedDue->attemptId},
                     expectedGeneration);
        refreshCount(firstDue.providerId, expectedGeneration);
        return applied.status == IrOutboxMutationStatus::Updated
                   ? now
                   : safeAdd(now, 1'000);
      }

      std::vector<IrOutboxEntry> planned;
      planned.reserve(plan.rowIds.size());
      for (const std::int64_t rowId : plan.rowIds) {
        const auto found = std::ranges::find(
            providerDue, rowId, &IrOutboxEntry::id);
        if (found == providerDue.end()) {
          planned.clear();
          break;
        }
        planned.push_back(*found);
      }
      if (planned.empty()) {
        return safeAdd(now, 1'000);
      }
      if (std::ranges::any_of(planned, [&](const IrOutboxEntry &entry) {
            return (entry.state == IrOutboxState::AwaitingRemoteResult) !=
                   polling;
          })) {
        return safeAdd(now, 1'000);
      }

      const std::string credential =
          lookupCredential(options, config.profileId, firstDue.providerId);
      {
        std::lock_guard lock(mutex);
        if (!requestMayStartLocked(expectedGeneration)) {
          return std::nullopt;
        }
      }
      if (credential.empty()) {
        {
          std::lock_guard lock(mutex);
          if (generation != expectedGeneration) {
            return std::nullopt;
          }
        }
        for (const auto &entry : planned) {
          repository.BlockIrOutboxConfiguration(
              entry.id, entry.state, "missing_api_key",
              "IR API key is required", now);
        }
        for (const auto &entry : planned) {
          refreshEntry({entry.providerId, entry.attemptId},
                       expectedGeneration);
        }
        refreshCount(firstDue.providerId, expectedGeneration);
        return now;
      }

      std::vector<IrOutboxClaimRequest> claimRequests;
      claimRequests.reserve(planned.size());
      for (const auto &entry : planned) {
        claimRequests.push_back(
            {.rowId = entry.id, .expectedState = entry.state});
      }
      auto claim = repository.ClaimIrOutboxBatch(claimRequests, now);
      if (claim.status != IrOutboxClaimStatus::Claimed ||
          claim.entries.size() != planned.size()) {
        continue;
      }
      std::stop_token requestToken;
      {
        std::lock_guard lock(mutex);
        if (!requestMayStartLocked(expectedGeneration)) {
          return std::nullopt;
        }
        for (const auto &entry : claim.entries) {
          publishLocked(entry);
        }
        requestStop = std::stop_source{};
        requestToken = requestStop.get_token();
      }
      refreshCount(firstDue.providerId, expectedGeneration);

      const IrProviderRuntimeConfig runtime{
          .profileId = config.profileId,
          .serverOrigin = provider->second.serverOrigin,
          .apiKey = credential,
      };
      DeliveryOutcome outcome;
      try {
        outcome = polling
                      ? drivers.pollBatch(firstDue.providerId, claim.entries,
                                          runtime, http, requestToken)
                      : drivers.submitBatch(
                            firstDue.providerId, claim.entries,
                            claim.consumedUserIntent, runtime, http,
                            requestToken);
      } catch (...) {
        outcome = {.status = DeliveryStatus::TransientFailure,
                   .code = "worker_exception",
                   .diagnostic = "IR delivery failed unexpectedly"};
      }
      outcome.code = redactCredential(std::move(outcome.code), credential);
      outcome.diagnostic =
          redactCredential(std::move(outcome.diagnostic), credential);
      if (responseIdentityContainsCredential(outcome, credential)) {
        outcome = {
            .status = DeliveryStatus::PermanentFailure,
            .code = "malformed_response",
            .diagnostic = "IR delivery returned invalid receipt identity",
        };
      }

      const std::int64_t completedAt = safeNow(options);
      {
        std::lock_guard lock(mutex);
        if (generation != expectedGeneration) {
          return std::nullopt;
        }
      }

      std::string singularRemoteScoreId;
      if (outcome.status == DeliveryStatus::Succeeded) {
        bool scoreIdentityValid = true;
        if (planned.size() == 1) {
          if (outcome.remoteScoreIds.size() > 1 ||
              (outcome.remoteScoreId && !outcome.remoteScoreIds.empty() &&
               *outcome.remoteScoreId != outcome.remoteScoreIds.front())) {
            scoreIdentityValid = false;
          } else if (outcome.remoteScoreIds.size() == 1) {
            singularRemoteScoreId = outcome.remoteScoreIds.front();
          } else if (outcome.remoteScoreId) {
            singularRemoteScoreId = *outcome.remoteScoreId;
          }
        }
        if (!scoreIdentityValid) {
          outcome = {
              .status = DeliveryStatus::PermanentFailure,
              .code = "malformed_response",
              .diagnostic = "IR delivery returned invalid receipt identity",
          };
        }
      }

      std::vector<IrOutboxDeliveryUpdate> updates;
      std::vector<std::optional<std::string>> successfulOrigins(planned.size());
      updates.reserve(planned.size());
      for (std::size_t index = 0; index < planned.size(); ++index) {
        if (outcome.status == DeliveryStatus::Succeeded) {
          successfulOrigins[index] =
              requestOrigin(planned[index], provider->second);
          std::string receiptDiagnostic;
          const IrSuccessfulReceiptDraft receipt{
              .serverOrigin = successfulOrigins[index].value_or(std::string{}),
              .remoteUserId = outcome.remoteUserId,
              .remoteScoreId = singularRemoteScoreId,
              .source = IrReceiptConfirmationSource::Submission,
              .observedInSnapshot = false,
              .confirmedAtUnixMillis = completedAt,
          };
          if (!successfulOrigins[index] ||
              !validateIrSuccessfulReceiptDraft(receipt, receiptDiagnostic)) {
            outcome = {
                .status = DeliveryStatus::PermanentFailure,
                .code = "malformed_response",
                .diagnostic = "IR delivery returned invalid receipt identity",
            };
            std::ranges::fill(successfulOrigins, std::nullopt);
            break;
          }
        }
      }
      for (std::size_t index = 0; index < planned.size(); ++index) {
        auto update = deliveryUpdate(claim.entries[index], planned[index].state,
                                     outcome, completedAt);
        if (outcome.status == DeliveryStatus::Succeeded) {
          update.successfulReceipt = IrSuccessfulReceiptDraft{
              .serverOrigin = *successfulOrigins[index],
              .remoteUserId = outcome.remoteUserId,
              .remoteScoreId = singularRemoteScoreId,
              .source = IrReceiptConfirmationSource::Submission,
              .observedInSnapshot = false,
              .confirmedAtUnixMillis = completedAt,
          };
        }
        updates.push_back(std::move(update));
      }
      const auto applied = repository.ApplyIrOutboxDeliveries(updates);
      if (applied.status != IrOutboxMutationStatus::Updated) {
        const DeliveryOutcome recovery{
            .status = DeliveryStatus::TransientFailure,
            .code = "storage_apply_failed",
            .diagnostic =
                "IR delivery state could not be stored; retry scheduled",
        };
        std::vector<IrOutboxDeliveryUpdate> recoveryUpdates;
        recoveryUpdates.reserve(planned.size());
        for (std::size_t index = 0; index < planned.size(); ++index) {
          recoveryUpdates.push_back(deliveryUpdate(
              claim.entries[index], planned[index].state, recovery,
              completedAt));
        }
        const auto recovered =
            repository.ApplyIrOutboxDeliveries(recoveryUpdates);
        // If this scoped mutation also fails, lifecycle recovery owns these
        // claims. Active delivery must not reset unrelated Uploading rows.
        (void)recovered;
      }
      for (const auto &entry : planned) {
        refreshEntry({entry.providerId, entry.attemptId}, expectedGeneration);
      }
      refreshCount(firstDue.providerId, expectedGeneration);
      if (applied.status == IrOutboxMutationStatus::Updated &&
          outcome.status == DeliveryStatus::Succeeded &&
          options.submissionSucceeded) {
        for (std::size_t index = 0; index < planned.size(); ++index) {
          try {
            options.submissionSucceeded(
                config.profileId, planned[index].providerId,
                *successfulOrigins[index], planned[index].chartMd5,
                planned[index].chartSha256);
          } catch (...) {
          }
        }
      }
      return applied.status == IrOutboxMutationStatus::Updated
                 ? completedAt
                 : safeAdd(completedAt, 1'000);
    }
    return nextEligibleFutureTime(expectedGeneration, config, now);
  }

  void workerMain(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
      IrActiveProfileConfig config;
      std::uint64_t currentGeneration = 0;
      std::optional<ReconciliationCommand> reconciliation;
      {
        std::unique_lock lock(mutex);
        while (!stopToken.stop_requested() &&
               (stopped || profilePaused || !applicationActive)) {
          waitForSignal(lock, stopToken, std::nullopt);
        }
        if (stopToken.stop_requested() || stopped) {
          break;
        }
        workerBusy = true;
        config = profile;
        currentGeneration = generation;
        if (pendingReconciliation) {
          reconciliation = std::move(pendingReconciliation);
          pendingReconciliation.reset();
          activeReconciliation = reconciliation;
        }
      }

      std::optional<std::int64_t> nextWallTime;
      try {
        if (reconciliation) {
          runReconciliation(*reconciliation);
          nextWallTime = safeNow(options);
        } else {
          nextWallTime = runOne(currentGeneration, config);
        }
      } catch (...) {
        if (reconciliation) {
          failReconciliation(*reconciliation,
                             "IR reconciliation failed unexpectedly");
          nextWallTime = safeNow(options);
        } else {
          repository.RecoverStaleIrOutbox(safeNow(options));
        }
      }

      std::unique_lock lock(mutex);
      workerBusy = false;
      condition.notify_all();
      if (stopToken.stop_requested() || stopped) {
        break;
      }
      std::optional<SteadyTimePoint> deadline;
      if (nextWallTime) {
        const std::int64_t now = safeNow(options);
        if (*nextWallTime <= now) {
          continue;
        }
        const std::int64_t delay =
            std::max<std::int64_t>(0, *nextWallTime - now);
        deadline = monotonicNow(options) + std::chrono::milliseconds(delay);
      }
      waitForSignal(lock, stopToken, deadline);
    }
    std::lock_guard lock(mutex);
    workerBusy = false;
    condition.notify_all();
  }
};

IrSubmissionService::IrSubmissionService(ReplayRepository &repository,
                                         const IrDriverRegistry &drivers,
                                         IrHttpClient &http,
                                         IrSubmissionServiceOptions options)
    : impl_(std::make_unique<Impl>(repository, drivers, http,
                                   std::move(options))) {}

IrSubmissionService::~IrSubmissionService() { stop(); }

void IrSubmissionService::start(IrActiveProfileConfig config) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->started) {
      return;
    }
    impl_->started = true;
    impl_->stopped = false;
  }
  impl_->prepareProfile(std::move(config));
  impl_->worker =
      std::jthread([implementation = impl_.get()](std::stop_token stopToken) {
        implementation->workerMain(stopToken);
      });
  impl_->signal();
}

void IrSubmissionService::pauseAndCancel() {
  std::unique_lock lock(impl_->mutex);
  if (!impl_->started || impl_->stopped) {
    return;
  }
  std::optional<std::string> cancelledProvider;
  const std::uint64_t pausedGeneration = impl_->generation;
  if (impl_->pendingReconciliation) {
    cancelledProvider = impl_->pendingReconciliation->providerId;
    impl_->pendingReconciliation.reset();
  }
  impl_->profilePaused = true;
  impl_->requestStop.request_stop();
  ++impl_->wakeRevision;
  impl_->condition.notify_all();
  lock.unlock();
  if (cancelledProvider) {
    const SteadyTimePoint nextAllowedAt =
        monotonicNow(impl_->options) + kReconciliationCooldown;
    lock.lock();
    if (impl_->generation == pausedGeneration) {
      impl_->publishReconciliationFailureLocked(
          *cancelledProvider, "IR reconciliation was cancelled",
          nextAllowedAt);
    }
    lock.unlock();
  }
  if (impl_->options.wake) {
    try {
      impl_->options.wake();
    } catch (...) {
    }
  }
  lock.lock();
  impl_->condition.wait(lock, [&] { return !impl_->workerBusy; });
  ++impl_->generation;
  lock.unlock();
  impl_->repository.RecoverStaleIrOutbox(safeNow(impl_->options));
}

void IrSubmissionService::activateProfile(IrActiveProfileConfig config) {
  IrActiveProfileConfig previous;
  {
    std::lock_guard lock(impl_->mutex);
    previous = impl_->profile;
  }
  const auto changedProviders = providersWithChangedRuntime(previous, config);
  pauseAndCancel();
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped) {
      return;
    }
  }
  const std::int64_t now = safeNow(impl_->options);
  for (const auto &providerId : changedProviders) {
    if (!lookupCredential(impl_->options, config.profileId, providerId)
             .empty()) {
      impl_->repository.UnblockIrOutbox(providerId, now);
    }
  }
  impl_->prepareProfile(std::move(config));
}

void IrSubmissionService::setApplicationActive(bool active) {
  if (!active) {
    {
      std::lock_guard lock(impl_->mutex);
      impl_->applicationActive = false;
      impl_->requestStop.request_stop();
    }
    impl_->signal();
    return;
  }

  IrActiveProfileConfig config;
  std::uint64_t currentGeneration = 0;
  {
    std::unique_lock lock(impl_->mutex);
    if (impl_->applicationActive) {
      lock.unlock();
      impl_->signal();
      return;
    }
    if (!impl_->started || impl_->stopped) {
      impl_->applicationActive = true;
      lock.unlock();
      impl_->signal();
      return;
    }
    impl_->requestStop.request_stop();
    lock.unlock();
    impl_->signal();
    lock.lock();
    impl_->condition.wait(lock, [&] {
      return !impl_->workerBusy || impl_->stopped;
    });
    if (impl_->stopped) {
      return;
    }
    config = impl_->profile;
    currentGeneration = impl_->generation;
  }

  impl_->repository.RecoverStaleIrOutbox(safeNow(impl_->options));
  impl_->loadProfileSnapshots(config, currentGeneration, false);
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopped) {
      impl_->applicationActive = true;
    }
  }
  impl_->signal();
}

void IrSubmissionService::notifyConfigurationChanged() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->configurationDirty = true;
    ++impl_->credentialGeneration;
    impl_->requestStop.request_stop();
  }
  impl_->signal();
}

void IrSubmissionService::notifyOutboxChanged() { impl_->signal(); }

void IrSubmissionService::stop() {
  std::jthread worker;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->started || impl_->stopped) {
      return;
    }
    impl_->stopped = true;
    impl_->profilePaused = true;
    impl_->requestStop.request_stop();
    ++impl_->wakeRevision;
    worker = std::move(impl_->worker);
  }
  worker.request_stop();
  impl_->condition.notify_all();
  if (impl_->options.wake) {
    try {
      impl_->options.wake();
    } catch (...) {
    }
  }
  if (worker.joinable()) {
    worker.join();
  }
  impl_->repository.RecoverStaleIrOutbox(safeNow(impl_->options));
  std::lock_guard lock(impl_->mutex);
  ++impl_->generation;
}

IrOutboxInsertOutcome
IrSubmissionService::enqueueManual(const IrOutboxDraft &draft) {
  const auto batch =
      enqueueManualBatch(std::span<const IrOutboxDraft>(&draft, 1));
  if (batch.items.empty()) {
    return {.status = IrOutboxInsertStatus::StorageFailure,
            .diagnostic = batch.diagnostic};
  }
  const IrManualBatchItemOutcome &item = batch.items.front();
  switch (item.status) {
  case IrManualBatchItemStatus::Inserted:
    return {.status = IrOutboxInsertStatus::Inserted,
            .entry = item.entry,
            .diagnostic = item.diagnostic};
  case IrManualBatchItemStatus::RetryQueued:
  case IrManualBatchItemStatus::AlreadyQueued:
  case IrManualBatchItemStatus::AlreadySubmitted:
    return {.status = IrOutboxInsertStatus::AlreadyExists,
            .entry = item.entry,
            .diagnostic = item.diagnostic};
  case IrManualBatchItemStatus::Failed:
    std::string validation;
    if (!validateIrOutboxDraft(draft, validation) ||
        batch.diagnostic == "IR provider is unavailable for manual submit") {
      return {.status = IrOutboxInsertStatus::Invalid,
              .diagnostic = item.diagnostic.empty() ? batch.diagnostic
                                                    : item.diagnostic};
    }
    return {.status = batch.storageAvailable
                        ? IrOutboxInsertStatus::IntegrityConflict
                        : IrOutboxInsertStatus::StorageFailure,
            .diagnostic = item.diagnostic.empty() ? batch.diagnostic
                                                  : item.diagnostic};
  }
  return {.status = IrOutboxInsertStatus::StorageFailure,
          .diagnostic = "IR manual enqueue returned an unknown result"};
}

IrManualBatchEnqueueOutcome IrSubmissionService::enqueueManualBatch(
    std::span<const IrOutboxDraft> drafts) {
  if (drafts.empty()) {
    return {.diagnostic = "IR manual batch is empty"};
  }
  const std::string providerId = drafts.front().providerId;
  std::uint64_t currentGeneration = 0;
  {
    std::lock_guard lock(impl_->mutex);
    const auto provider = impl_->profile.providers.find(providerId);
    if (!impl_->started || impl_->stopped || impl_->profilePaused ||
        provider == impl_->profile.providers.end() ||
        !provider->second.enabled ||
        !std::ranges::all_of(drafts, [&](const IrOutboxDraft &draft) {
          return draft.providerId == providerId;
        }) ||
        !providerCanSubmit(impl_->drivers, providerId)) {
      IrManualBatchEnqueueOutcome unavailable{
          .diagnostic = "IR provider is unavailable for manual submit"};
      unavailable.items.reserve(drafts.size());
      for (const auto &draft : drafts) {
        unavailable.items.push_back({.attemptId = draft.attemptId,
                                     .diagnostic = unavailable.diagnostic});
      }
      return unavailable;
    }
    currentGeneration = impl_->generation;
  }
  auto result = impl_->repository.EnqueueReadyIrOutboxDrafts(
      drafts, true, safeNow(impl_->options));
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->generation == currentGeneration) {
      for (const auto &item : result.items) {
        if (item.entry) {
          impl_->publishLocked(*item.entry);
        }
      }
    }
  }
  impl_->refreshCount(providerId, currentGeneration);
  impl_->signal();
  return result;
}

IrOutboxMutationOutcome IrSubmissionService::retry(std::int64_t rowId) {
  std::optional<StatusKey> key;
  std::uint64_t currentGeneration = 0;
  {
    std::lock_guard lock(impl_->mutex);
    currentGeneration = impl_->generation;
    const auto found = impl_->rowKeys.find(rowId);
    if (found != impl_->rowKeys.end()) {
      key = found->second;
    }
  }
  auto result = impl_->repository.RetryIrOutbox(rowId, safeNow(impl_->options));
  if (key) {
    impl_->refreshEntry(*key, currentGeneration);
    impl_->refreshCount(key->first, currentGeneration);
  }
  impl_->signal();
  return result;
}

IrOutboxMutationOutcome
IrSubmissionService::retryAll(std::string_view providerId) {
  std::uint64_t currentGeneration = 0;
  IrActiveProfileConfig profile;
  {
    std::lock_guard lock(impl_->mutex);
    currentGeneration = impl_->generation;
    profile = impl_->profile;
  }
  auto result =
      impl_->repository.RetryAllIrOutbox(providerId, safeNow(impl_->options));
  impl_->loadProfileSnapshots(profile, currentGeneration);
  impl_->signal();
  return result;
}

IrOutboxMutationOutcome IrSubmissionService::discard(std::int64_t rowId) {
  std::optional<StatusKey> key;
  std::uint64_t currentGeneration = 0;
  {
    std::lock_guard lock(impl_->mutex);
    currentGeneration = impl_->generation;
    const auto found = impl_->rowKeys.find(rowId);
    if (found != impl_->rowKeys.end()) {
      key = found->second;
    }
  }
  auto result = impl_->repository.DiscardIrOutbox(rowId);
  if (result.status == IrOutboxMutationStatus::Updated && key) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->generation == currentGeneration) {
      impl_->eraseLocked(*key, rowId);
    }
  }
  if (key) {
    impl_->refreshCount(key->first, currentGeneration);
  }
  impl_->signal();
  return result;
}

IrOutboxCounts IrSubmissionService::counts(std::string_view providerId) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->countSnapshots.find(providerId);
  return found == impl_->countSnapshots.end() ? IrOutboxCounts{}
                                              : found->second;
}

std::vector<IrAttemptStatusSnapshot>
IrSubmissionService::statuses(std::string_view providerId) const {
  std::vector<IrAttemptStatusSnapshot> result;
  std::lock_guard lock(impl_->mutex);
  for (const auto &[key, snapshot] : impl_->statusSnapshots) {
    if (key.first == providerId) {
      result.push_back(snapshot);
    }
  }
  std::ranges::sort(result, [](const auto &left, const auto &right) {
    return left.rowId > right.rowId;
  });
  return result;
}

IrAttemptStatusSnapshot
IrSubmissionService::status(std::string_view providerId,
                            std::string_view attemptId) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->statusSnapshots.find(
      StatusKey{std::string(providerId), std::string(attemptId)});
  if (found == impl_->statusSnapshots.end()) {
    return {.revision = impl_->statusRevision};
  }
  return found->second;
}

IrReconciliationRequestStatus
IrSubmissionService::requestUserScoreReconciliation(
    std::string_view providerId) {
  if (!providerCanReconcile(impl_->drivers, providerId)) {
    return IrReconciliationRequestStatus::Unsupported;
  }

  std::unique_lock lock(impl_->mutex);
  const auto configured = [&]() -> std::optional<std::string> {
    const auto provider = impl_->profile.providers.find(providerId);
    if (provider == impl_->profile.providers.end() ||
        !provider->second.enabled) {
      return std::nullopt;
    }
    const auto origin = normalizeServerOrigin(provider->second.serverOrigin);
    const auto credential = impl_->credentials.find(providerId);
    if (!origin || credential == impl_->credentials.end() ||
        !credential->second.present) {
      return std::nullopt;
    }
    return origin;
  };
  const auto serviceActive = [&] {
    return impl_->started && !impl_->stopped && !impl_->profilePaused &&
           impl_->applicationActive;
  };
  if (!serviceActive()) {
    return IrReconciliationRequestStatus::ServiceInactive;
  }
  if (impl_->pendingReconciliation || impl_->activeReconciliation) {
    return IrReconciliationRequestStatus::AlreadyRunning;
  }
  auto origin = configured();
  if (!origin) {
    return IrReconciliationRequestStatus::ConfigurationRequired;
  }

  auto status = impl_->reconciliationSnapshots.find(providerId);
  if (status != impl_->reconciliationSnapshots.end() &&
      status->second.nextAllowedAt) {
    lock.unlock();
    const SteadyTimePoint now = monotonicNow(impl_->options);
    lock.lock();
    if (!serviceActive()) {
      return IrReconciliationRequestStatus::ServiceInactive;
    }
    if (impl_->pendingReconciliation || impl_->activeReconciliation) {
      return IrReconciliationRequestStatus::AlreadyRunning;
    }
    origin = configured();
    if (!origin) {
      return IrReconciliationRequestStatus::ConfigurationRequired;
    }
    status = impl_->reconciliationSnapshots.find(providerId);
    if (status != impl_->reconciliationSnapshots.end() &&
        status->second.nextAllowedAt && now < *status->second.nextAllowedAt) {
      if (status->second.phase != IrReconciliationPhase::Cooldown) {
        status->second.revision = ++impl_->reconciliationRevision;
        status->second.phase = IrReconciliationPhase::Cooldown;
      }
      return IrReconciliationRequestStatus::Cooldown;
    }
  }

  impl_->pendingReconciliation = IrSubmissionService::Impl::ReconciliationCommand{
      .providerId = std::string(providerId),
      .profileId = impl_->profile.profileId,
      .serverOrigin = *origin,
      .serviceGeneration = impl_->generation,
      .credentialGeneration = impl_->credentialGeneration,
  };
  impl_->publishReconciliationPhaseLocked(providerId,
                                          IrReconciliationPhase::Queued);
  lock.unlock();
  impl_->signal();
  return IrReconciliationRequestStatus::Accepted;
}

IrReconciliationStatusSnapshot IrSubmissionService::reconciliationStatus(
    std::string_view providerId) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->reconciliationSnapshots.find(providerId);
  if (found == impl_->reconciliationSnapshots.end()) {
    return {.revision = impl_->reconciliationRevision};
  }
  return found->second;
}

} // namespace ir
