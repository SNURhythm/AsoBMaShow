#include "GameplaySkinLifecycle.h"

#include "GameplaySkinTraits.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <string_view>
#include <type_traits>
#include <utility>

namespace skin {
namespace {

std::atomic_uint64_t nextGameplaySkinSessionSerial{0};

std::uint64_t allocateSessionSerial() noexcept {
  auto current = nextGameplaySkinSessionSerial.load(std::memory_order_relaxed);
  while (current != std::numeric_limits<std::uint64_t>::max()) {
    if (nextGameplaySkinSessionSerial.compare_exchange_weak(
            current, current + 1, std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      return current + 1;
    }
  }
  return 0;
}

SkinDiagnostic lifecycleDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

bool sameIdentity(const PlaySkinSessionIdentity &left,
                  const PlaySkinSessionIdentity &right) noexcept {
  return left.sessionSerial == right.sessionSerial &&
         left.profileId == right.profileId && left.entry == right.entry &&
         left.revisionDigest == right.revisionDigest &&
         left.configurationDigest == right.configurationDigest;
}

bool selectsGameplayEntry(const SkinProfileSettings &settings,
                          const SkinEntryId &entry) {
  return std::ranges::any_of(
      settings.selectedGameplayEntries,
      [&entry](const auto &selection) { return selection.second == entry; });
}

void applyWrite(EntryProfileSettings &entry,
                const PersistedSkinConfigurationWrite &write) {
  std::visit(
      [&entry](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, SetSkinOption>) {
          entry.options[value.key] = value.value;
        } else if constexpr (std::is_same_v<Value, SetSkinFilePath>) {
          entry.filePaths[value.key] = value.declaredValue;
        } else if constexpr (std::is_same_v<Value, SetSkinOffset>) {
          entry.offsets[value.key] = value.value;
        }
      },
      write);
}

#if !defined(ASOBMASHOW_GAMEPLAY_SKIN_LIFECYCLE_TESTING)
GameplaySkinLifecycleDependencies makeProductionDependencies(
    SkinStorageRoots roots, SkinPackageOperationService &operations,
    SkinDiagnosticHistory &history, SkinConfigurationWriteQueue &writes,
    ISkinProfileSettingsOwner &owner, ISkinProfileSnapshotProvider &snapshots,
    SkinCommitCoordinator &commits, SkinActivationClientId client) {
  const auto visibleRoot = roots.visiblePackages;
  return {
      .roots = std::move(roots),
      .ensureVisibleRoot =
          [visibleRoot] {
            std::error_code error;
            std::filesystem::create_directories(visibleRoot, error);
            return !error;
          },
      .snapshotProfile =
          [&owner](const SkinProfileId &profile) {
            return owner.snapshot(profile);
          },
      .acquireActivation =
          [&operations](const SkinProfileId &profile, const SkinEntryId &entry,
                        std::string_view digest) {
            return operations.acquireValidatedActivation(profile, entry,
                                                         digest);
          },
      .submitPrepareActivation =
          [&operations](VersionedSkinProfileSettings base, SkinEntryId entry,
                        SkinProfileSettings candidate) {
            auto handle = operations.submitPrepareActivation(
                std::move(base), std::move(entry), std::move(candidate));
            GameplaySkinLifecycleOperationSubmission result{.ticket =
                                                                handle.ticket};
            if (handle.ticket == 0) {
              result.diagnostics.push_back(lifecycleDiagnostic(
                  "skin.lifecycle.operation_rejected",
                  "Skin activation preparation was not admitted"));
            }
            return result;
          },
      .submitReconcileProfileActivations =
          [&operations](std::vector<SkinProfileId> profiles) {
            auto handle = operations.submitReconcileProfileActivations(
                std::move(profiles));
            GameplaySkinLifecycleOperationSubmission result{.ticket =
                                                                handle.ticket};
            if (handle.ticket == 0) {
              result.diagnostics.push_back(lifecycleDiagnostic(
                  "skin.lifecycle.reconcile_rejected",
                  "The gameplay skin activation reconciliation was not "
                  "admitted"));
            }
            return result;
          },
      .submitRescan =
          [&operations](ProfileInventorySnapshot inventory) {
            auto handle = operations.submitRescan(std::move(inventory));
            GameplaySkinLifecycleOperationSubmission result{.ticket =
                                                                handle.ticket};
            if (handle.ticket == 0) {
              result.diagnostics.push_back(lifecycleDiagnostic(
                  "skin.lifecycle.rescan_rejected",
                  "The gameplay skin rescan was not admitted"));
            }
            return result;
          },
      .pollOperation =
          [&operations](std::uint64_t ticket) {
            auto completion = operations.poll(ticket);
            if (!completion) {
              return std::optional<GameplaySkinLifecycleOperationCompletion>{};
            }
            GameplaySkinLifecycleOperationCompletion result{
                .ticket = completion->ticket};
            if (auto *prepared = std::get_if<PrepareActivationResult>(
                    &completion->payload)) {
              result.payload = std::move(*prepared);
            } else if (auto *reconciled =
                           std::get_if<ReconcileProfileActivationsResult>(
                               &completion->payload)) {
              result.payload = std::move(*reconciled);
            } else if (auto *scan = std::get_if<ScanPackagesResult>(
                           &completion->payload)) {
              result.payload = std::move(*scan);
            }
            return std::optional{std::move(result)};
          },
      .cancelOperation =
          [&operations](std::uint64_t ticket) {
            operations.cancelAndDetach(ticket);
          },
      .beginProfileInventory =
          [&snapshots] { return snapshots.beginSnapshotAllProfiles(); },
      .pollProfileInventory =
          [&snapshots](std::uint64_t ticket) {
            return snapshots.pollSnapshotAllProfiles(ticket);
          },
      .cancelProfileInventory =
          [&snapshots](std::uint64_t ticket) {
            snapshots.cancelSnapshotAllProfiles(ticket);
          },
      .submitActivation =
          [&commits, client](PreparedSkinActivation prepared) {
            return commits.submitActivation(client, std::move(prepared));
          },
      .takeActivationCompletions =
          [&commits, client] { return commits.takeCompletions(client); },
      .takeRevalidationRequests =
          [&commits] { return commits.takeRevalidationRequests(); },
      .submitProfileSettings =
          [&commits, client](const VersionedSkinProfileSettings &base,
                             SkinProfileSettings candidate) {
            return commits.submitProfileSettings(client, base,
                                                 std::move(candidate));
          },
      .takeProfileCompletions =
          [&commits, client] { return commits.takeProfileCompletions(client); },
      .drainConfigurationWrites = [&writes] { return writes.drain(); },
      .closeConfigurationWrites = [&writes] { writes.close(); },
      .catalogSnapshot = [&operations] { return operations.catalogSnapshot(); },
      .detachCommitClient = [&commits,
                             client] { commits.detachClient(client); },
      .appendHistory =
          [&history](SkinDiagnosticHistoryRecord record) {
            history.append(std::move(record));
          },
  };
}
#endif

} // namespace

struct GameplaySkinLifecycle::Impl {
  enum class PreparePurpose : std::uint8_t {
    Writer,
    Revalidation,
    Reconcile,
    Rescan,
  };

  struct PrepareOperation {
    PreparePurpose purpose = PreparePurpose::Revalidation;
    std::uint64_t chainGeneration = 0;
  };

  struct CommitPurpose {
    PreparePurpose purpose = PreparePurpose::Revalidation;
    std::uint64_t chainGeneration = 0;
  };

  struct WriterChain {
    std::uint64_t generation = 0;
    PlaySkinSessionIdentity identity;
    VersionedSkinProfileSettings base;
    std::deque<SkinConfigurationWriteRequest> pending;
    std::uint64_t lastFrameSerial = 0;
    std::uint64_t prepareTicket = 0;
    std::uint64_t commitTicket = 0;
    std::optional<VersionedSkinProfileSettings> commitSuccessor;
  };

  struct PendingRevalidation {
    VersionedSkinProfileSettings base;
    SkinEntryId entry;
  };

  struct PendingViewportCommit {
    std::uint64_t ticket = 0;
    PlaySkinSessionIdentity identity;
    VersionedSkinProfileSettings base;
    SkinProfileSettings candidate;
    std::optional<VersionedSkinProfileSettings> successor;
  };

  struct DeferredViewport {
    PlaySkinSessionIdentity identity;
    ViewportSettings viewport;
  };

  explicit Impl(GameplaySkinLifecycleDependencies dependencies)
      : deps(std::move(dependencies)) {}

  std::optional<VersionedSkinProfileSettings> captureAdmittedSuccessor(
      const VersionedSkinProfileSettings &base,
      const SkinProfileSettings &candidate) const {
    if (!deps.snapshotProfile ||
        base.generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    try {
      // Failed admissions reserve and never reuse owner generations, so the
      // direct successor is the synchronously published snapshot rather than
      // necessarily base.generation + 1.
      auto successor = deps.snapshotProfile(base.profileId);
      if (successor.profileId != base.profileId ||
          successor.generation <= base.generation ||
          successor.settings != candidate) {
        return std::nullopt;
      }
      return successor;
    } catch (...) {
      return std::nullopt;
    }
  }

  bool ownerStillMatches(
      const VersionedSkinProfileSettings &expected) const noexcept {
    if (!deps.snapshotProfile) {
      return false;
    }
    try {
      return deps.snapshotProfile(expected.profileId) == expected;
    } catch (...) {
      return false;
    }
  }

  void append(SkinDiagnostic diagnostic, SkinDiagnosticPhase phase,
              const std::optional<PlaySkinSessionIdentity> &identity = {},
              std::optional<std::uint64_t> frame = {}) noexcept {
    if (!deps.appendHistory) {
      return;
    }
    try {
      SkinDiagnosticHistoryRecord record{.phase = phase,
                                         .diagnostic = std::move(diagnostic),
                                         .frameSerial = frame};
      if (identity) {
        record.entry = identity->entry;
        record.revisionDigest = identity->revisionDigest;
        record.configurationDigest = identity->configurationDigest;
      }
      deps.appendHistory(std::move(record));
    } catch (...) {
    }
  }

  void appendAll(
      std::vector<SkinDiagnostic> diagnostics, SkinDiagnosticPhase phase,
      const std::optional<PlaySkinSessionIdentity> &identity = {}) noexcept {
    for (auto &diagnostic : diagnostics) {
      append(std::move(diagnostic), phase, identity);
    }
  }

  void invalidateWriterChain(std::string code, std::string message) noexcept {
    if (!writer) {
      return;
    }
    const auto identity = writer->identity;
    if (writer->prepareTicket != 0 && deps.cancelOperation) {
      deps.cancelOperation(writer->prepareTicket);
      prepareOperations.erase(writer->prepareTicket);
    }
    writer.reset();
    append(lifecycleDiagnostic(std::move(code), std::move(message)),
           SkinDiagnosticPhase::Session, identity);
  }

  void discardWriterChain() noexcept {
    if (!writer) {
      return;
    }
    if (writer->prepareTicket != 0 && deps.cancelOperation) {
      deps.cancelOperation(writer->prepareTicket);
      prepareOperations.erase(writer->prepareTicket);
    }
    writer.reset();
  }

  bool activationWorkInFlight() const noexcept {
    if (!commitPurposes.empty()) {
      return true;
    }
    return std::ranges::any_of(prepareOperations, [](const auto &item) {
      return item.second.purpose != PreparePurpose::Rescan;
    });
  }

  bool matchesWriterIdentity(const SkinConfigurationWriteRequest &request,
                             const WriterChain &chain) const noexcept {
    return request.sessionSerial == chain.identity.sessionSerial &&
           request.profileId == chain.identity.profileId &&
           request.entry == chain.identity.entry &&
           request.expectedRevisionDigest == chain.identity.revisionDigest &&
           request.expectedConfigurationDigest ==
               chain.identity.configurationDigest;
  }

  void drainWriterIngress() {
    if (!deps.drainConfigurationWrites) {
      return;
    }
    auto requests = deps.drainConfigurationWrites();
    for (auto &request : requests) {
      if (!writer || !matchesWriterIdentity(request, *writer) ||
          request.frameSerial == 0 ||
          request.frameSerial <= writer->lastFrameSerial) {
        append(lifecycleDiagnostic(
                   "skin.lifecycle.writer_identity_stale",
                   "A gameplay skin writer batch no longer matches the "
                   "active chart identity or authored frame order"),
               SkinDiagnosticPhase::Session,
               writer ? std::optional{writer->identity} : currentIdentity,
               request.frameSerial == 0 ? std::optional<std::uint64_t>{}
                                        : std::optional{request.frameSerial});
        continue;
      }
      if (writer->pending.size() == maxPendingSessionWrites) {
        append(lifecycleDiagnostic(
                   "skin.lifecycle.writer_fifo_full",
                   "The bounded gameplay skin writer FIFO is full"),
               SkinDiagnosticPhase::Session, writer->identity,
               request.frameSerial);
        continue;
      }
      writer->lastFrameSerial = request.frameSerial;
      writer->pending.push_back(std::move(request));
    }
  }

  bool writerBaseStillCurrent() const {
    if (!writer || !deps.snapshotProfile) {
      return false;
    }
    try {
      return deps.snapshotProfile(writer->identity.profileId) == writer->base;
    } catch (...) {
      return false;
    }
  }

  void startWriterPrepare() {
    if (!writer || writer->pending.empty() || writer->prepareTicket != 0 ||
        writer->commitTicket != 0 || viewportCommit || activationWorkInFlight()) {
      return;
    }
    if (!writerBaseStillCurrent()) {
      invalidateWriterChain(
          "skin.lifecycle.writer_external_change",
          "The desired skin configuration changed outside this session");
      return;
    }
    auto candidate = writer->base.settings;
    const auto found = candidate.entries.find(writer->identity.entry);
    if (found == candidate.entries.end() ||
        !selectsGameplayEntry(candidate, writer->identity.entry)) {
      invalidateWriterChain(
          "skin.lifecycle.writer_entry_changed",
          "The selected gameplay skin entry changed before writer commit");
      return;
    }
    for (const auto &write : writer->pending.front().orderedWrites) {
      applyWrite(found->second, write);
    }
    candidate.sanitize();
    if (!selectsGameplayEntry(candidate, writer->identity.entry) ||
        !candidate.entries.contains(writer->identity.entry)) {
      invalidateWriterChain(
          "skin.lifecycle.writer_candidate_invalid",
          "The gameplay skin writer candidate failed sanitization");
      return;
    }
    auto submission = deps.submitPrepareActivation(
        writer->base, writer->identity.entry, std::move(candidate));
    appendAll(std::move(submission.diagnostics), SkinDiagnosticPhase::Session,
              writer->identity);
    if (submission.ticket == 0) {
      invalidateWriterChain(
          "skin.lifecycle.writer_prepare_rejected",
          "The gameplay skin writer candidate was not admitted");
      return;
    }
    writer->pending.pop_front();
    writer->prepareTicket = submission.ticket;
    prepareOperations.emplace(
        submission.ticket,
        PrepareOperation{.purpose = PreparePurpose::Writer,
                         .chainGeneration = writer->generation});
  }

  void startRevalidation() {
    if (pendingRevalidations.empty() || viewportCommit ||
        activationWorkInFlight()) {
      return;
    }
    // Preserve the writer chain's strict one-transaction progression.
    if (writer && (writer->prepareTicket != 0 || writer->commitTicket != 0 ||
                   !writer->pending.empty())) {
      return;
    }
    PendingRevalidation work = std::move(pendingRevalidations.front());
    pendingRevalidations.pop_front();
    SkinProfileSettings candidate = work.base.settings;
    auto submission = deps.submitPrepareActivation(
        std::move(work.base), std::move(work.entry), std::move(candidate));
    appendAll(std::move(submission.diagnostics),
              SkinDiagnosticPhase::Validation);
    if (submission.ticket != 0) {
      prepareOperations.emplace(
          submission.ticket,
          PrepareOperation{.purpose = PreparePurpose::Revalidation});
    }
  }

  void
  consumePrepareCompletion(GameplaySkinLifecycleOperationCompletion completion,
                           PrepareOperation operation) {
    if (operation.purpose == PreparePurpose::Reconcile) {
      auto *reconciled = std::get_if<ReconcileProfileActivationsResult>(
          &completion.payload);
      if (reconciled != nullptr) {
        appendAll(std::move(reconciled->diagnostics),
                  SkinDiagnosticPhase::Scan);
      }
      if (reconciled == nullptr || !reconciled->completed ||
          !pendingRescanInventory) {
        pendingRescanInventory.reset();
        append(lifecycleDiagnostic(
                   "skin.lifecycle.reconcile_failed",
                   "Gameplay skin activation reconciliation did not complete"),
               SkinDiagnosticPhase::Scan);
        return;
      }
      if (!deps.submitRescan) {
        pendingRescanInventory.reset();
        append(lifecycleDiagnostic(
                   "skin.lifecycle.rescan_rejected",
                   "The gameplay skin rescan service is unavailable"),
               SkinDiagnosticPhase::Scan);
        return;
      }
      auto submission =
          deps.submitRescan(std::move(*pendingRescanInventory));
      pendingRescanInventory.reset();
      appendAll(std::move(submission.diagnostics), SkinDiagnosticPhase::Scan);
      if (submission.ticket == 0) {
        append(lifecycleDiagnostic(
                   "skin.lifecycle.rescan_rejected",
                   "The gameplay skin rescan was not admitted"),
               SkinDiagnosticPhase::Scan);
        return;
      }
      prepareOperations.emplace(
          submission.ticket,
          PrepareOperation{.purpose = PreparePurpose::Rescan});
      return;
    }

    if (operation.purpose == PreparePurpose::Rescan) {
      auto *scan = std::get_if<ScanPackagesResult>(&completion.payload);
      if (scan != nullptr) {
        appendAll(std::move(scan->diagnostics), SkinDiagnosticPhase::Scan);
      }
      const bool succeeded = scan != nullptr && !scan->cancelled &&
                             !scan->retryableInventoryRace &&
                             scan->sourceGeneration != 0;
      if (!succeeded) {
        append(lifecycleDiagnostic(
                   "skin.lifecycle.rescan_failed",
                   "The gameplay skin rescan did not complete successfully"),
               SkinDiagnosticPhase::Scan);
        return;
      }
      acquisitionReady = true;
      if (activeProfile && deps.snapshotProfile) {
        try {
          const auto base = deps.snapshotProfile(*activeProfile);
          for (const auto &[skinType, entry] :
               base.settings.selectedGameplayEntries) {
            (void)skinType;
            pendingRevalidations.push_back(
                {.base = base, .entry = entry});
          }
        } catch (...) {
        }
      }
      return;
    }

    auto *prepared = std::get_if<PrepareActivationResult>(&completion.payload);
    if (prepared == nullptr || prepared->cancelled || !prepared->prepared) {
      if (prepared != nullptr) {
        appendAll(std::move(prepared->diagnostics),
                  SkinDiagnosticPhase::Validation,
                  operation.purpose == PreparePurpose::Writer && writer
                      ? std::optional{writer->identity}
                      : std::nullopt);
      }
      if (operation.purpose == PreparePurpose::Writer && writer &&
          writer->generation == operation.chainGeneration) {
        writer->prepareTicket = 0;
        invalidateWriterChain(
            "skin.lifecycle.writer_validation_failed",
            "The gameplay skin writer candidate did not validate");
      }
      return;
    }
    std::optional<SkinProfileSettings> writerCandidate;
    if (operation.purpose == PreparePurpose::Writer) {
      if (!writer || writer->generation != operation.chainGeneration) {
        return;
      }
      const auto &successor = *prepared->prepared;
      const auto successorEntry =
          successor.candidateProfileSettings.entries.find(writer->identity.entry);
      const bool identityChanged =
          successor.profileId != writer->identity.profileId ||
          successor.expectedProfileGeneration != writer->base.generation ||
          successor.activation.entry != writer->identity.entry ||
          successor.activation.revision.revision().lowercaseSha256 !=
              writer->identity.revisionDigest ||
          !selectsGameplayEntry(successor.candidateProfileSettings,
                                writer->identity.entry) ||
          successorEntry == successor.candidateProfileSettings.entries.end() ||
          successorEntry->second != successor.activation.reconciledSettings ||
          skinConfigurationDigest(successor.activation.reconciledSettings) !=
              successor.activation.configurationDigest;
      if (identityChanged) {
        writer->prepareTicket = 0;
        invalidateWriterChain(
            "skin.lifecycle.writer_prepared_identity_stale",
            "The prepared gameplay skin writer successor changed profile, "
            "entry, revision, or configuration identity");
        return;
      }
      writerCandidate = successor.candidateProfileSettings;
    }
    auto submission = deps.submitActivation(std::move(*prepared->prepared));
    appendAll(std::move(submission.diagnostics),
              SkinDiagnosticPhase::Validation,
              operation.purpose == PreparePurpose::Writer && writer
                  ? std::optional{writer->identity}
                  : std::nullopt);
    if (!submission.accepted || submission.ticket == 0) {
      if (operation.purpose == PreparePurpose::Writer && writer &&
          writer->generation == operation.chainGeneration) {
        writer->prepareTicket = 0;
        invalidateWriterChain(
            "skin.lifecycle.writer_commit_rejected",
            "The validated gameplay skin writer candidate was not admitted");
      }
      return;
    }
    commitPurposes.emplace(
        submission.ticket,
        CommitPurpose{.purpose = operation.purpose,
                      .chainGeneration = operation.chainGeneration});
    if (operation.purpose == PreparePurpose::Writer && writer &&
        writer->generation == operation.chainGeneration) {
      writer->prepareTicket = 0;
      writer->commitTicket = submission.ticket;
      writer->commitSuccessor =
          writerCandidate
              ? captureAdmittedSuccessor(writer->base, *writerCandidate)
              : std::nullopt;
    }
  }

  void consumeOperations() {
    for (auto iterator = prepareOperations.begin();
         iterator != prepareOperations.end();) {
      const auto ticket = iterator->first;
      auto completion = deps.pollOperation(ticket);
      if (!completion) {
        ++iterator;
        continue;
      }
      const auto operation = iterator->second;
      iterator = prepareOperations.erase(iterator);
      consumePrepareCompletion(std::move(*completion), operation);
    }
  }

  void consumeActivationCompletions() {
    if (!deps.takeActivationCompletions) {
      return;
    }
    for (auto &completion : deps.takeActivationCompletions()) {
      const auto purpose = commitPurposes.find(completion.ticket);
      if (purpose == commitPurposes.end()) {
        continue;
      }
      const auto operation = purpose->second;
      commitPurposes.erase(purpose);
      appendAll(std::move(completion.result.diagnostics),
                SkinDiagnosticPhase::Validation,
                operation.purpose == PreparePurpose::Writer && writer
                    ? std::optional{writer->identity}
                    : std::nullopt);
      if (operation.purpose != PreparePurpose::Writer || !writer ||
          writer->generation != operation.chainGeneration) {
        continue;
      }
      writer->commitTicket = 0;
      if (completion.result.disposition ==
              ActivationCommitDisposition::ActivatedRequested &&
          completion.result.profileSnapshot && completion.result.activation) {
        const auto successorEntry =
            completion.result.profileSnapshot->settings.entries.find(
                writer->identity.entry);
        const bool exactSuccessor =
            writer->commitSuccessor &&
            *completion.result.profileSnapshot == *writer->commitSuccessor &&
            ownerStillMatches(*writer->commitSuccessor) &&
            successorEntry !=
                completion.result.profileSnapshot->settings.entries.end() &&
            completion.result.activation->entry == writer->identity.entry &&
            completion.result.activation->revision.revision().lowercaseSha256 ==
                writer->identity.revisionDigest &&
            completion.result.activation->reconciledSettings ==
                successorEntry->second &&
            completion.result.activation->configurationDigest ==
                skinConfigurationDigest(successorEntry->second);
        if (exactSuccessor) {
          writer->base = std::move(*completion.result.profileSnapshot);
          writer->commitSuccessor.reset();
          continue;
        }
      }
      invalidateWriterChain(
          "skin.lifecycle.writer_commit_failed",
          "The gameplay skin writer commit did not activate its successor");
    }
  }

  std::optional<VersionedSkinProfileSettings>
  validateCurrentIdentity(const PlaySkinSessionIdentity &identity) {
    if (!currentIdentity || !sameIdentity(*currentIdentity, identity) ||
        !activeProfile || *activeProfile != identity.profileId ||
        !deps.snapshotProfile || !deps.acquireActivation) {
      return std::nullopt;
    }
    try {
      auto snapshot = deps.snapshotProfile(identity.profileId);
      if (!selectsGameplayEntry(snapshot.settings, identity.entry)) {
        return std::nullopt;
      }
      const auto entry = snapshot.settings.entries.find(identity.entry);
      if (entry == snapshot.settings.entries.end()) {
        return std::nullopt;
      }
      auto expectedConfigurationDigest = identity.configurationDigest;
      if (writer && sameIdentity(writer->identity, identity)) {
        const bool matchesWriterBase = snapshot == writer->base;
        const bool matchesPendingWriter =
            writer->commitTicket != 0 && writer->commitSuccessor &&
            snapshot == *writer->commitSuccessor;
        const bool matchesPendingViewport =
            viewportCommit &&
            sameIdentity(viewportCommit->identity, identity) &&
            viewportCommit->successor &&
            snapshot == *viewportCommit->successor;
        if (!matchesWriterBase && !matchesPendingWriter &&
            !matchesPendingViewport) {
          return std::nullopt;
        }
        if (matchesPendingWriter) {
          const auto committedEntry =
              writer->base.settings.entries.find(identity.entry);
          if (committedEntry == writer->base.settings.entries.end()) {
            return std::nullopt;
          }
          expectedConfigurationDigest =
              skinConfigurationDigest(committedEntry->second);
        } else {
          expectedConfigurationDigest = skinConfigurationDigest(entry->second);
        }
      } else if (skinConfigurationDigest(entry->second) !=
                 expectedConfigurationDigest) {
        return std::nullopt;
      }
      auto activation = deps.acquireActivation(
          identity.profileId, identity.entry, expectedConfigurationDigest);
      if (!activation.activation ||
          activation.activation->entry != identity.entry ||
          activation.activation->configurationDigest !=
              expectedConfigurationDigest ||
          activation.activation->revision.revision().lowercaseSha256 !=
              identity.revisionDigest) {
        return std::nullopt;
      }
      return snapshot;
    } catch (...) {
      return std::nullopt;
    }
  }

  GameplayViewportPersistenceResult
  submitViewport(const PlaySkinSessionIdentity &identity,
                 ViewportSettings viewport) {
    auto base = validateCurrentIdentity(identity);
    if (!base) {
      auto diagnostic = lifecycleDiagnostic(
          "skin.lifecycle.viewport_identity_stale",
          "Reset Layout no longer matches the active chart skin identity");
      append(diagnostic, SkinDiagnosticPhase::Session, identity);
      return {.disposition = GameplayViewportPersistenceDisposition::Rejected,
              .diagnostic = std::move(diagnostic)};
    }
    const auto found = base->settings.entries.find(identity.entry);
    if (found == base->settings.entries.end()) {
      return {.disposition = GameplayViewportPersistenceDisposition::Rejected};
    }
    auto candidate = base->settings;
    candidate.entries.at(identity.entry).viewport = viewport;
    candidate.sanitize();
    auto submission = deps.submitProfileSettings(*base, candidate);
    appendAll(std::move(submission.diagnostics), SkinDiagnosticPhase::Session,
              identity);
    if (!submission.accepted || submission.ticket == 0) {
      auto diagnostic = lifecycleDiagnostic(
          "skin.lifecycle.viewport_commit_rejected",
          "The viewport-only profile update was not admitted");
      append(diagnostic, SkinDiagnosticPhase::Session, identity);
      return {.disposition = GameplayViewportPersistenceDisposition::Rejected,
              .diagnostic = std::move(diagnostic)};
    }
    auto successor = captureAdmittedSuccessor(*base, candidate);
    viewportCommit = PendingViewportCommit{.ticket = submission.ticket,
                                           .identity = identity,
                                           .base = std::move(*base),
                                           .candidate = std::move(candidate),
                                           .successor = std::move(successor)};
    return {.disposition = GameplayViewportPersistenceDisposition::Queued};
  }

  void retryDeferredViewport() {
    if (!deferredViewport || viewportCommit || activationWorkInFlight() ||
        (writer && !writer->pending.empty())) {
      return;
    }
    auto deferred = std::move(*deferredViewport);
    deferredViewport.reset();
    (void)submitViewport(deferred.identity, deferred.viewport);
  }

  void consumeProfileCompletions() {
    if (!deps.takeProfileCompletions) {
      return;
    }
    for (auto &completion : deps.takeProfileCompletions()) {
      if (!viewportCommit || completion.ticket != viewportCommit->ticket) {
        continue;
      }
      auto pending = std::move(*viewportCommit);
      viewportCommit.reset();
      const auto &identity = pending.identity;
      if (completion.result.status !=
          SkinProfileCommitResult::Status::Persisted) {
        if (completion.result.failure) {
          append(std::move(*completion.result.failure),
                 SkinDiagnosticPhase::Session, identity);
        }
        deferredViewport.reset();
        continue;
      }
      if (!completion.result.snapshot || !pending.successor ||
          *completion.result.snapshot != *pending.successor ||
          !ownerStillMatches(*pending.successor)) {
        if (writer && sameIdentity(writer->identity, identity)) {
          invalidateWriterChain(
              "skin.lifecycle.viewport_successor_mismatch",
              "The viewport profile commit did not return its exact successor");
        }
        deferredViewport.reset();
        continue;
      }
      for (auto &revalidation : pendingRevalidations) {
        if (revalidation.base == pending.base) {
          revalidation.base = *completion.result.snapshot;
        }
      }
      if (writer && sameIdentity(writer->identity, identity)) {
        if (writer->base != pending.base) {
          invalidateWriterChain(
              "skin.lifecycle.viewport_base_changed",
              "The gameplay skin writer base changed during viewport persistence");
          deferredViewport.reset();
          continue;
        }
        writer->base = *completion.result.snapshot;
      }
      if (deferredViewport) {
        retryDeferredViewport();
      }
    }
  }

  void pollInventoryAndRescan() {
    if (inventoryTicket != 0) {
      auto result = deps.pollProfileInventory(inventoryTicket);
      if (result) {
        inventoryTicket = 0;
        appendAll(std::move(result->diagnostics), SkinDiagnosticPhase::Scan);
        if (result->complete && !result->cancelled && result->inventory) {
          pendingRescanInventory = std::move(*result->inventory);
          std::vector<SkinProfileId> profiles;
          profiles.reserve(pendingRescanInventory->profiles.size());
          for (const auto &snapshot : pendingRescanInventory->profiles) {
            profiles.push_back(snapshot.profileId);
          }
          GameplaySkinLifecycleOperationSubmission submission;
          if (deps.submitReconcileProfileActivations) {
            submission = deps.submitReconcileProfileActivations(
                std::move(profiles));
          }
          appendAll(std::move(submission.diagnostics),
                    SkinDiagnosticPhase::Scan);
          if (submission.ticket == 0) {
            pendingRescanInventory.reset();
            append(lifecycleDiagnostic(
                       "skin.lifecycle.reconcile_rejected",
                       "The gameplay skin activation reconciliation was not "
                       "admitted"),
                   SkinDiagnosticPhase::Scan);
          } else {
            prepareOperations.emplace(
                submission.ticket,
                PrepareOperation{.purpose = PreparePurpose::Reconcile});
          }
        } else {
          append(lifecycleDiagnostic(
                     "skin.lifecycle.inventory_failed",
                     "A complete all-profile gameplay skin inventory was not "
                     "obtained"),
                 SkinDiagnosticPhase::Scan);
        }
      }
    }
    const bool rescanOperationInFlight = std::ranges::any_of(
        prepareOperations, [](const auto &item) {
          return item.second.purpose == PreparePurpose::Reconcile ||
                 item.second.purpose == PreparePurpose::Rescan;
        });
    if (rescanRequested && inventoryTicket == 0 &&
        !pendingRescanInventory && !rescanOperationInFlight &&
        deps.beginProfileInventory) {
      rescanRequested = false;
      inventoryTicket = deps.beginProfileInventory();
      if (inventoryTicket == 0) {
        append(lifecycleDiagnostic(
                   "skin.lifecycle.inventory_rejected",
                   "The all-profile inventory was not admitted for rescan"),
               SkinDiagnosticPhase::Scan);
      }
    }
  }

  GameplaySkinLifecycleDependencies deps;
  std::optional<SkinProfileId> activeProfile;
  std::optional<PlaySkinSessionIdentity> currentIdentity;
  std::optional<WriterChain> writer;
  std::deque<PendingRevalidation> pendingRevalidations;
  std::map<std::uint64_t, PrepareOperation> prepareOperations;
  std::map<std::uint64_t, CommitPurpose> commitPurposes;
  std::optional<PendingViewportCommit> viewportCommit;
  std::optional<DeferredViewport> deferredViewport;
  std::optional<ProfileInventorySnapshot> pendingRescanInventory;
  std::uint64_t inventoryTicket = 0;
  std::uint64_t nextChainGeneration = 0;
  bool initialized = false;
  bool acquisitionReady = false;
  bool rescanRequested = false;
  bool stopped = false;
};

#if !defined(ASOBMASHOW_GAMEPLAY_SKIN_LIFECYCLE_TESTING)
GameplaySkinLifecycle::GameplaySkinLifecycle(
    SkinStorageRoots roots, SkinPackageOperationService &operations,
    SkinDiagnosticHistory &history, SkinConfigurationWriteQueue &writes,
    ISkinProfileSettingsOwner &owner, ISkinProfileSnapshotProvider &snapshots,
    SkinCommitCoordinator &commits, SkinActivationClientId lifecycleClientId)
    : GameplaySkinLifecycle(makeProductionDependencies(
          std::move(roots), operations, history, writes, owner, snapshots,
          commits, lifecycleClientId)) {}
#endif

GameplaySkinLifecycle::GameplaySkinLifecycle(
    GameplaySkinLifecycleDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies))) {}

GameplaySkinLifecycle::~GameplaySkinLifecycle() { shutdown(); }

void GameplaySkinLifecycle::startAfterProfileInitialization(
    SkinProfileId profile) {
  if (impl_->stopped || impl_->initialized) {
    return;
  }
  if (!impl_->deps.ensureVisibleRoot || !impl_->deps.ensureVisibleRoot()) {
    impl_->append(lifecycleDiagnostic(
                      "skin.lifecycle.visible_root_failed",
                      "The gameplay skin Documents directory is unavailable"),
                  SkinDiagnosticPhase::Scan);
    return;
  }
  impl_->initialized = true;
  // The package catalog is recovered before this lifecycle starts. A scan is
  // user initiated, so recovery is sufficient to serve the next chart.
  impl_->acquisitionReady = true;
  impl_->activeProfile = std::move(profile);
}

void GameplaySkinLifecycle::profileChanged(SkinProfileId profile) {
  if (impl_->stopped) {
    return;
  }
  impl_->activeProfile = std::move(profile);
  impl_->pendingRevalidations.clear();
  if (impl_->writer &&
      (!impl_->writer->pending.empty() || impl_->writer->prepareTicket != 0 ||
       impl_->writer->commitTicket != 0)) {
    impl_->invalidateWriterChain(
        "skin.lifecycle.profile_changed",
        "The active profile changed; pending writer batches were discarded");
  } else {
    impl_->discardWriterChain();
  }
  if (impl_->deps.cancelOperation) {
    for (auto iterator = impl_->prepareOperations.begin();
         iterator != impl_->prepareOperations.end();) {
      if (iterator->second.purpose != Impl::PreparePurpose::Revalidation) {
        ++iterator;
        continue;
      }
      impl_->deps.cancelOperation(iterator->first);
      iterator = impl_->prepareOperations.erase(iterator);
    }
  }
  if (!impl_->deps.snapshotProfile) {
    return;
  }
  try {
    const auto snapshot = impl_->deps.snapshotProfile(*impl_->activeProfile);
    for (const auto &[skinType, entry] :
         snapshot.settings.selectedGameplayEntries) {
      (void)skinType;
      impl_->pendingRevalidations.push_back(
          {.base = snapshot, .entry = entry});
    }
  } catch (...) {
  }
}

void GameplaySkinLifecycle::requestRescan(SkinRescanReason) {
  if (!impl_->stopped && impl_->initialized) {
    impl_->rescanRequested = true;
  }
}

void GameplaySkinLifecycle::requestRevalidation(const SkinEntryId &entry) {
  if (impl_->stopped || !impl_->initialized || !impl_->activeProfile ||
      !impl_->deps.snapshotProfile) {
    return;
  }
  try {
    impl_->pendingRevalidations.push_back(
        {.base = impl_->deps.snapshotProfile(*impl_->activeProfile),
         .entry = entry});
  } catch (...) {
  }
}

GameplayViewportPersistenceResult GameplaySkinLifecycle::requestViewportReset(
    const PlaySkinSessionIdentity &identity, ViewportSettings viewport) {
  if (impl_->stopped) {
    return {.disposition = GameplayViewportPersistenceDisposition::Rejected};
  }
  if (impl_->viewportCommit) {
    if (!impl_->validateCurrentIdentity(identity) ||
        !sameIdentity(impl_->viewportCommit->identity, identity)) {
      return {.disposition = GameplayViewportPersistenceDisposition::Rejected};
    }
    impl_->deferredViewport =
        Impl::DeferredViewport{.identity = identity, .viewport = viewport};
    return {.disposition = GameplayViewportPersistenceDisposition::Deferred};
  }
  if (impl_->activationWorkInFlight() ||
      (impl_->writer && !impl_->writer->pending.empty())) {
    if (!impl_->validateCurrentIdentity(identity)) {
      return {.disposition = GameplayViewportPersistenceDisposition::Rejected};
    }
    impl_->deferredViewport =
        Impl::DeferredViewport{.identity = identity, .viewport = viewport};
    return {.disposition = GameplayViewportPersistenceDisposition::Deferred};
  }
  return impl_->submitViewport(identity, viewport);
}

void GameplaySkinLifecycle::poll() {
  if (impl_->stopped || !impl_->initialized) {
    return;
  }
  impl_->consumeActivationCompletions();
  impl_->consumeProfileCompletions();
  if (impl_->deps.takeRevalidationRequests) {
    for (auto &snapshot : impl_->deps.takeRevalidationRequests()) {
      for (const auto &[skinType, entry] :
           snapshot.settings.selectedGameplayEntries) {
        (void)skinType;
        impl_->pendingRevalidations.push_back(
            {.base = snapshot, .entry = entry});
      }
    }
  }
  impl_->consumeOperations();
  impl_->drainWriterIngress();
  impl_->pollInventoryAndRescan();
  impl_->retryDeferredViewport();
  impl_->startWriterPrepare();
  impl_->startRevalidation();
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
GameplaySkinLifecycle::catalogSnapshot() const noexcept {
  if (!impl_ || !impl_->deps.catalogSnapshot) {
    return {};
  }
  try {
    return impl_->deps.catalogSnapshot();
  } catch (...) {
    return {};
  }
}

GameplaySkinAcquisition
GameplaySkinLifecycle::acquireForNextChart(int keyMode) {
  if (impl_->stopped || !impl_->initialized || !impl_->acquisitionReady ||
      !impl_->activeProfile ||
      !impl_->deps.snapshotProfile || !impl_->deps.acquireActivation) {
    return {.disposition = GameplaySkinAcquisitionDisposition::Failed,
            .failure = GameplaySkinAcquisitionFailure{
                .diagnostic = lifecycleDiagnostic(
                    "skin.lifecycle.acquisition_unavailable",
                    "The selected gameplay skin could not be acquired")}};
  }
  // Every call is a chart boundary. A failed/disabled acquisition must not
  // leave the preceding chart's identity eligible for writers or Reset Layout.
  impl_->discardWriterChain();
  impl_->currentIdentity.reset();
  std::optional<SkinEntryId> requestedEntry;
  std::string requestedConfigurationDigest;
  try {
    auto base = impl_->deps.snapshotProfile(*impl_->activeProfile);
    base.settings.sanitize();
    const auto trait = gameplaySkinTraitForKeyMode(keyMode);
    if (!trait) {
      return {};
    }
    const auto selectedTrait =
        base.settings.selectedGameplayEntries.find(trait->skinType);
    if (selectedTrait == base.settings.selectedGameplayEntries.end()) {
      return {};
    }
    requestedEntry = selectedTrait->second;
    const auto selected = base.settings.entries.find(*requestedEntry);
    if (selected == base.settings.entries.end()) {
      return {.disposition = GameplaySkinAcquisitionDisposition::Failed,
              .failure = GameplaySkinAcquisitionFailure{
                  .entry = std::move(requestedEntry),
                  .diagnostic = lifecycleDiagnostic(
                      "skin.lifecycle.selected_entry_missing",
                      "The selected gameplay skin configuration is missing")}};
    }
    requestedConfigurationDigest = skinConfigurationDigest(selected->second);
    auto acquired = impl_->deps.acquireActivation(
        base.profileId, *requestedEntry, requestedConfigurationDigest);
    std::optional<SkinDiagnostic> acquisitionFailure;
    for (const auto &diagnostic : acquired.diagnostics) {
      if (!acquisitionFailure &&
          diagnostic.severity == DiagnosticSeverity::Error) {
        acquisitionFailure = diagnostic;
      }
    }
    impl_->appendAll(std::move(acquired.diagnostics),
                     SkinDiagnosticPhase::Validation);
    if (!acquired.activation || acquired.activation->entry != *requestedEntry ||
        acquired.activation->configurationDigest !=
            requestedConfigurationDigest) {
      return {.disposition = GameplaySkinAcquisitionDisposition::Failed,
              .failure = GameplaySkinAcquisitionFailure{
                  .entry = std::move(requestedEntry),
                  .configurationDigest =
                      std::move(requestedConfigurationDigest),
                  .diagnostic = acquisitionFailure.value_or(
                      lifecycleDiagnostic(
                          "skin.lifecycle.activation_unavailable",
                          "The selected gameplay skin is not ready for this "
                          "configuration"))}};
    }
    const auto sessionSerial = allocateSessionSerial();
    if (sessionSerial == 0) {
      auto diagnostic = lifecycleDiagnostic(
          "skin.lifecycle.session_serial_exhausted",
          "Gameplay skin session serials are exhausted");
      impl_->append(diagnostic, SkinDiagnosticPhase::Session);
      return {.disposition = GameplaySkinAcquisitionDisposition::Failed,
              .failure = GameplaySkinAcquisitionFailure{
                  .entry = std::move(requestedEntry),
                  .revisionDigest =
                      acquired.activation->revision.revision().lowercaseSha256,
                  .configurationDigest =
                      std::move(requestedConfigurationDigest),
                  .diagnostic = std::move(diagnostic)}};
    }
    PlaySkinSessionIdentity identity{
        .sessionSerial = sessionSerial,
        .profileId = base.profileId,
        .entry = *requestedEntry,
        .revisionDigest =
            acquired.activation->revision.revision().lowercaseSha256,
        .configurationDigest = requestedConfigurationDigest};
    const auto chainGeneration = ++impl_->nextChainGeneration;
    impl_->writer.emplace(Impl::WriterChain{
        .generation = chainGeneration, .identity = identity, .base = base});
    impl_->currentIdentity = identity;
    return {.disposition = GameplaySkinAcquisitionDisposition::Ready,
            .request = GameplaySkinActivationRequest{
                .sessionSerial = sessionSerial,
                .profileId = base.profileId,
                .activation = std::move(*acquired.activation),
                .viewport = selected->second.viewport}};
  } catch (...) {
    auto diagnostic = lifecycleDiagnostic(
        "skin.lifecycle.acquire_failed",
        "The next-chart gameplay skin activation could not be acquired");
    impl_->append(diagnostic, SkinDiagnosticPhase::Validation);
    return {.disposition = GameplaySkinAcquisitionDisposition::Failed,
            .failure = GameplaySkinAcquisitionFailure{
                .entry = std::move(requestedEntry),
                .configurationDigest =
                    std::move(requestedConfigurationDigest),
                .diagnostic = std::move(diagnostic)}};
  }
}

void GameplaySkinLifecycle::recordPresentationFailure(
    const PresentationFailure &failure) {
  PlaySkinSessionIdentity identity;
  if (impl_->currentIdentity) {
    identity = *impl_->currentIdentity;
  }
  identity.entry = failure.entry;
  identity.revisionDigest = failure.revisionDigest;
  identity.configurationDigest = failure.configurationDigest;
  impl_->append(failure.diagnostic, SkinDiagnosticPhase::FrameFallback,
                identity, failure.frameSerial);
}

void GameplaySkinLifecycle::shutdown() noexcept {
  if (!impl_ || impl_->stopped) {
    return;
  }
  impl_->stopped = true;
  if (impl_->inventoryTicket != 0 && impl_->deps.cancelProfileInventory) {
    try {
      impl_->deps.cancelProfileInventory(impl_->inventoryTicket);
    } catch (...) {
    }
  }
  if (impl_->deps.cancelOperation) {
    for (const auto &[ticket, operation] : impl_->prepareOperations) {
      (void)operation;
      try {
        impl_->deps.cancelOperation(ticket);
      } catch (...) {
      }
    }
  }
  impl_->prepareOperations.clear();
  impl_->pendingRescanInventory.reset();
  impl_->pendingRevalidations.clear();
  if (impl_->writer) {
    impl_->writer->pending.clear();
  }
  if (impl_->deps.closeConfigurationWrites) {
    try {
      impl_->deps.closeConfigurationWrites();
    } catch (...) {
    }
  }
  if (impl_->deps.drainConfigurationWrites) {
    try {
      (void)impl_->deps.drainConfigurationWrites();
    } catch (...) {
    }
  }
  if (impl_->deps.detachCommitClient) {
    try {
      impl_->deps.detachCommitClient();
    } catch (...) {
    }
  }
}

} // namespace skin
