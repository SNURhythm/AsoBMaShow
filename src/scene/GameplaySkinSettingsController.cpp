#include "../i18n/Localization.h"
#include "GameplaySkinSettingsController.h"

#include "../ArchiveFile.h"
#include "../skin/SkinTargetTraits.h"
#include "../skin/package/SkinPathPolicy.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace skin {
namespace {

ControllerActionResult rejected(i18n::Text message) {
  return {.message = std::move(message)};
}

ControllerActionResult accepted(i18n::Text message, bool asynchronous = true) {
  return {.accepted = true,
          .asynchronous = asynchronous,
          .message = std::move(message)};
}

i18n::Text firstDiagnosticMessage(const std::vector<SkinDiagnostic> &values,
                                   i18n::Text fallback) {
  if (!values.empty() && !values.front().message.empty()) {
    return values.front().message;
  }
  return fallback;
}

bool containsConfiguration(const SkinCatalogEntrySnapshot &catalogEntry,
                           const EntryProfileSettings &settings) {
  const auto digest = skinConfigurationDigest(settings);
  return std::ranges::find(catalogEntry.validatedConfigurationDigests,
                           digest) !=
         catalogEntry.validatedConfigurationDigests.end();
}

std::optional<int>
selectableGameplaySkinType(const SkinCatalogEntrySnapshot *catalogEntry) {
  if (catalogEntry == nullptr ||
      catalogEntry->validation !=
          SkinValidationDisposition::SelectableGameplay ||
      !catalogEntry->metadata ||
      !skinTargetTraitForType(catalogEntry->metadata->skinType)) {
    return std::nullopt;
  }
  return catalogEntry->metadata->skinType;
}

SkinPackageId automaticPackageId(SkinPackageNameSuggestion &suggestion) {
  if (const auto normalized =
          normalizePackageId(suggestion.suggestedPackageName);
      normalized.package) {
    suggestion.suggestedPackageName = normalized.package->directoryName;
    suggestion.validationError.clear();
    return *normalized.package;
  }

  // A picker normally supplies a valid basename. If a third-party provider
  // does not, continue with a stable, safe package root instead of requiring
  // the user to diagnose its malformed display name. This is a persistent
  // directory/collision identity, so it must never be translated.
  const auto fallback = normalizePackageId("Imported Skin");
  suggestion.suggestedPackageName = fallback.package->directoryName;
  suggestion.validationError.clear();
  return *fallback.package;
}

} // namespace

SkinPackageNameSuggestion
suggestSkinPackageName(std::string originalSourceName,
                       PlatformTemporaryPathKind pathKind) {
  SkinPackageNameSuggestion result{.originalSourceName =
                                       std::move(originalSourceName)};
  auto normalized = normalizeSkinSourceNameNfc(result.originalSourceName);
  if (!normalized.value) {
    result.validationError = std::move(normalized.error);
    return result;
  }

  std::string proposed = std::move(*normalized.value);
  if (pathKind == PlatformTemporaryPathKind::File) {
    const auto extension = archive_file::archiveExtensionFromName(proposed);
    proposed.resize(proposed.size() - extension.size());
  }
  auto package = normalizePackageId(proposed);
  if (!package.package) {
    result.suggestedPackageName = std::move(proposed);
    result.validationError = std::move(package.error);
    return result;
  }
  result.suggestedPackageName = package.package->directoryName;
  return result;
}

struct GameplaySkinSettingsController::Impl {
  std::optional<int> activeTarget;
  enum class Phase : std::uint8_t {
    Idle,
    PickingArchive,
    PickingFolder,
    NameReady,
    PreparingPackage,
    LoadingInventory,
    LoadingRemovalInventory,
    Publishing,
    PreparingActivation,
    WaitingActivationCommit,
    WaitingProfileCommit,
    Removing,
    Rescanning,
  };

  explicit Impl(GameplaySkinSettingsControllerDependencies dependencies)
      : dependencies(std::move(dependencies)) {
    refreshProjection();
  }

  GameplaySkinSettingsControllerDependencies dependencies;
  GameplaySkinSettingsSnapshot projected;
  // Keep owned messages translatable until projection; provider diagnostics
  // remain literal Text values.
  i18n::Text statusText;
  Phase phase = Phase::Idle;
  bool closed = false;
  bool errorState = false;
  platform_document_handoff::PlatformDocumentHandoffOperation handoff;
  std::shared_ptr<PlatformDocumentHandoffResult> pickedSource;
  std::optional<SkinPreparedDisposalReservation> disposalReservation;
  std::optional<PreparedPackage> preparedPackage;
  PackageCollisionPolicy collisionPolicy = PackageCollisionPolicy::Reject;
  std::uint64_t operationTicket = 0;
  std::shared_ptr<const SkinPackageProgressMailbox> progress;
  std::uint64_t inventoryTicket = 0;
  std::optional<SkinPackageId> removalPackage;
  std::optional<ProfileInventoryCommitFence> removalFence;
  std::shared_ptr<const SkinPackageCatalogSnapshot> projectedCatalog;
  std::string projectedProfileId;
  std::uint64_t projectedProfileGeneration = 0;
  PresentationOrientation projectedOrientation = PresentationOrientation::Landscape;
  bool projectionInputsReady = false;

  void setStatus(i18n::Text message) {
    statusText = std::move(message);
    projected.statusMessage = statusText.resolve();
  }

  void refreshCachedPresentationKey() {
    projected.statusMessage = statusText.resolve();
    std::string key;
    const auto append = [&key](std::string_view value) {
      key.append(std::to_string(value.size()));
      key.push_back(':');
      key.append(value);
      key.push_back(';');
    };
    const auto number = [&append](auto value) {
      append(std::to_string(value));
    };
    append("v3");
    number(activeTarget.has_value());
    number(activeTarget.value_or(0));
    number(projectedCatalog ? projectedCatalog->catalogGeneration : 0);
    number(projectedCatalog ? projectedCatalog->sourceGeneration : 0);
    append(projectedProfileId);
    number(projectedProfileGeneration);
    number(static_cast<int>(projectedOrientation));
    number(static_cast<unsigned>(projected.state));
    number(projected.compatibilityEnabled);
    number(static_cast<unsigned>(projected.safetyLevel));
    number(projected.pendingSafetyLevel.has_value());
    number(projected.pendingSafetyLevel
               ? static_cast<unsigned>(*projected.pendingSafetyLevel)
               : 0U);
    append(projected.statusMessage);
    number(projected.hasPackageProgress);
    number(static_cast<unsigned>(projected.progress.phase));
    number(projected.progress.completedBytes);
    number(projected.progress.totalBytes);
    number(projected.progress.completedFiles);
    number(static_cast<unsigned>(projected.rescanProgress.phase));
    number(static_cast<unsigned>(projected.rescanProgress.packageProgress.phase));
    number(projected.rescanProgress.packageProgress.completedBytes);
    number(projected.rescanProgress.packageProgress.totalBytes);
    number(projected.rescanProgress.packageProgress.completedFiles);
    if (projected.preparedName) {
      append(projected.preparedName->originalSourceName);
      append(projected.preparedName->suggestedPackageName);
      append(projected.preparedName->validationError);
    } else {
      append({});
      append({});
      append({});
    }
    if (projected.collisionPackage) {
      append(projected.collisionPackage->directoryName);
      append(projected.collisionPackage->collisionKey);
    } else {
      append({});
      append({});
    }
    number(projected.history.size());
    number(projected.history.empty() ? 0
                                     : projected.history.back().recordSerial);
    projected.cachedPresentationKey = std::move(key);
  }

  [[nodiscard]] bool hasControllerOperation() const noexcept {
    return phase != Phase::Idle;
  }

  [[nodiscard]] bool phaseCanCancel() const noexcept {
    return phase == Phase::Rescanning;
  }

  std::shared_ptr<const SkinPackageCatalogSnapshot> catalog() const {
    if (!dependencies.catalogSnapshot) {
      return {};
    }
    return dependencies.catalogSnapshot();
  }

  const SkinCatalogEntrySnapshot *findCatalogEntry(
      const SkinEntryId &entry,
      const std::shared_ptr<const SkinPackageCatalogSnapshot> &snapshot) const {
    if (!snapshot) {
      return nullptr;
    }
    const auto found = std::ranges::find(snapshot->entries, entry,
                                         &SkinCatalogEntrySnapshot::entry);
    return found == snapshot->entries.end() ? nullptr : &*found;
  }

  int configurationTarget(int declaredType) const {
    return activeTarget && skinSourceTypeForTarget(*activeTarget) == declaredType
               ? *activeTarget : declaredType;
  }

  void refreshProjection() {
    if (closed) {
      return;
    }
    const auto profile =
        dependencies.profileOwner.snapshot(dependencies.profileId, dependencies.orientation);
    const auto catalogValue = catalog();
    const bool inputsChanged = !projectionInputsReady ||
                               projectedCatalog != catalogValue ||
                               projectedProfileId != profile.profileId.opaque ||
                               projectedProfileGeneration != profile.generation ||
                               projectedOrientation != profile.orientation;
    if (inputsChanged) {
      // Keep the full, display-ready catalog projection stable between catalog
      // or profile generations. Opening a dropdown must not clone every skin
      // title, configuration declaration, and diagnostic again.
      projected.compatibilityEnabled =
          profile.settings.gameplayCompatibilityEnabled;
      projected.safetyLevel = profile.settings.safetyLevel;
      projected.selectedSkinEntries = profile.settings.selectedSkinEntries;
      projected.follow5K1S = profile.settings.follow5K1S;
      projected.follow7K1S = profile.settings.follow7K1S;
      projected.selected7KeyEntry = profile.settings.selected7KeyEntry;
      projected.entries.clear();
      if (catalogValue) {
        projected.entries.reserve(catalogValue->entries.size());
        for (const auto &source : catalogValue->entries) {
          GameplaySkinEntryRow row{
              .entry = source.entry,
              .revisionDigest = source.revisionDigest,
              .validation = source.validation,
              .diagnostics = source.diagnostics,
          };
          if (source.metadata) {
            row.metadata = *source.metadata;
          }
          const auto &targetEntries = profile.settings.entriesForTarget(
              configurationTarget(row.metadata.skinType));
          if (const auto settings = targetEntries.find(source.entry);
              settings != targetEntries.end()) {
            row.settings = settings->second;
          }
          row.configurationDigest = skinConfigurationDigest(row.settings);
          projected.entries.push_back(std::move(row));
        }
      }
      projectedCatalog = catalogValue;
      projectedProfileId = profile.profileId.opaque;
      projectedProfileGeneration = profile.generation;
      projectedOrientation = profile.orientation;
      projectionInputsReady = true;
    }
    projected.history = dependencies.history.records();
    if (phase == Phase::Idle) {
      projected.state = errorState ? GameplaySkinSettingsState::Error
                                   : (projected.entries.empty()
                                          ? GameplaySkinSettingsState::Empty
                                          : GameplaySkinSettingsState::Ready);
    } else if (phase == Phase::NameReady) {
      projected.state = GameplaySkinSettingsState::Ready;
    } else if (phase != Phase::WaitingActivationCommit &&
               phase != Phase::WaitingProfileCommit) {
      projected.state = GameplaySkinSettingsState::Busy;
    }
    projected.canCancel = phaseCanCancel();
    refreshCachedPresentationKey();
  }

  void setBusy(i18n::Text message) {
    errorState = false;
    projected.state = GameplaySkinSettingsState::Busy;
    setStatus(std::move(message));
    projected.canCancel = phaseCanCancel();
    refreshCachedPresentationKey();
  }

  void setError(i18n::Text message) {
    errorState = true;
    phase = Phase::Idle;
    projected.state = GameplaySkinSettingsState::Error;
    setStatus(std::move(message));
    projected.canCancel = false;
    projected.hasPackageProgress = false;
    projected.progress = {};
    projected.rescanProgress = {};
    progress.reset();
    refreshCachedPresentationKey();
  }

  void setIdle(i18n::Text message = {}) {
    errorState = false;
    phase = Phase::Idle;
    setStatus(std::move(message));
    projected.canCancel = false;
    projected.hasPackageProgress = false;
    projected.progress = {};
    projected.rescanProgress = {};
    progress.reset();
    refreshProjection();
  }

  void transferPreparedDisposal(RejectedPreparedDisposal disposal) noexcept {
    if (!disposalReservation || !*disposalReservation) {
      std::terminate();
    }
    auto returned =
        std::move(*disposalReservation).transfer(std::move(disposal));
    disposalReservation.reset();
    if (returned) {
      std::terminate();
    }
  }

  void disposeLocalPrepared() noexcept {
    if (!preparedPackage) {
      return;
    }
    transferPreparedDisposal(
        {.prepared = std::move(*preparedPackage), .cleanup = {}});
    preparedPackage.reset();
  }

  void releaseDisposalReservation() noexcept { disposalReservation.reset(); }

  static SkinDeferredCleanup
  sourceCleanup(const std::shared_ptr<PlatformDocumentHandoffResult> &source) {
    return SkinDeferredCleanup([source] {
      (void)platform_document_handoff::CleanupTemporaryPath(*source);
    });
  }

  void transferPickedCleanup() noexcept {
    if (!pickedSource || !pickedSource->temporaryOwnership) {
      pickedSource.reset();
      releaseDisposalReservation();
      return;
    }
    if (!disposalReservation || !*disposalReservation) {
      std::terminate();
    }
    auto returned =
        std::move(*disposalReservation).transfer(sourceCleanup(pickedSource));
    disposalReservation.reset();
    if (returned) {
      std::terminate();
    }
    pickedSource.reset();
  }

  void rejectPrepareSubmission(SkinPackageOperationHandle handle) noexcept {
    if (!handle.rejectedCleanup || !disposalReservation ||
        !*disposalReservation) {
      std::terminate();
    }
    auto returned = std::move(*disposalReservation)
                        .transfer(std::move(*handle.rejectedCleanup));
    disposalReservation.reset();
    pickedSource.reset();
    if (returned) {
      std::terminate();
    }
    operationTicket = 0;
    setError(i18n::message("settings.skins.skin_package_preparation_failed_queued.message"));
  }

  bool beginInventory(i18n::Text &error) {
    try {
      inventoryTicket =
          dependencies.profileSnapshots.beginSnapshotAllProfiles();
      if (inventoryTicket == 0) {
        error = i18n::message("settings.skins.profile_inventory_failed_started.message");
        return false;
      }
      phase = Phase::LoadingInventory;
      projected.hasPackageProgress = false;
      setBusy(i18n::message("settings.skins.loading_profile_inventory.progress"));
      return true;
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    } catch (...) {
      error = i18n::message("settings.skins.profile_inventory_failed_started.message");
      return false;
    }
  }

  bool beginRemovalInventory(SkinPackageId package, i18n::Text &error) {
    try {
      inventoryTicket = dependencies.profileSnapshots.beginSnapshotAllProfiles();
      if (inventoryTicket == 0) {
        error = i18n::message("settings.skins.profile_inventory_failed_started.message");
        return false;
      }
      removalPackage.emplace(std::move(package));
      phase = Phase::LoadingRemovalInventory;
      projected.hasPackageProgress = false;
      setBusy(i18n::message("settings.skins.checking_skin_selections.progress"));
      return true;
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    } catch (...) {
      error = i18n::message("settings.skins.profile_inventory_failed_started.message");
      return false;
    }
  }

  void submitPublish(ProfileInventorySnapshot inventory) {
    auto handle = dependencies.operations.submitPublish(
        std::move(*preparedPackage), collisionPolicy, std::move(inventory));
    preparedPackage.reset();
    if (handle.ticket == 0) {
      if (!handle.rejectedPrepared) {
        std::terminate();
      }
      transferPreparedDisposal(std::move(*handle.rejectedPrepared));
      handle.rejectedPrepared.reset();
      setError(i18n::message("settings.skins.skin_package_publication_failed_queued.message"));
      return;
    }
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    projected.hasPackageProgress = true;
    phase = Phase::Publishing;
    setBusy(i18n::message("settings.skins.publishing_skin_package.progress"));
  }

  void pollHandoff() {
    if (!handoff || !handoff.ready()) {
      return;
    }
    auto result = handoff.takeResult();
    handoff.close();
    if (!result) {
      releaseDisposalReservation();
      setError(i18n::message("settings.skins.document_picker_returned_no_result.message"));
      return;
    }
    if (result->cancelled()) {
      pickedSource =
          std::make_shared<PlatformDocumentHandoffResult>(std::move(*result));
      transferPickedCleanup();
      setIdle(i18n::message("settings.skins.skin_import_cancelled.message"));
      return;
    }
    if (!result->ok()) {
      const auto message = result->message.empty()
                               ? i18n::message("settings.skins.skin_source_selection_failed.message")
                               : result->message;
      pickedSource =
          std::make_shared<PlatformDocumentHandoffResult>(std::move(*result));
      transferPickedCleanup();
      setError(message);
      return;
    }
    pickedSource =
        std::make_shared<PlatformDocumentHandoffResult>(std::move(*result));
    projected.preparedName = suggestSkinPackageName(
        pickedSource->originalSourceName, pickedSource->temporaryPathKind);
    const auto package = automaticPackageId(*projected.preparedName);
    const auto catalogValue = catalog();
    if (catalogValue) {
      const auto collision = std::ranges::find_if(
          catalogValue->packages, [&](const SkinPackageId &candidate) {
            return candidate.collisionKey == package.collisionKey;
          });
      if (collision != catalogValue->packages.end()) {
        projected.collisionPackage = *collision;
        errorState = false;
        phase = Phase::NameReady;
        projected.state = GameplaySkinSettingsState::Ready;
        setStatus(
            i18n::message("settings.skins.package_name_already_installed_confirm_replacement.message"));
        refreshProjection();
        return;
      }
    }
    (void)submitPreparedSource(package, PackageCollisionPolicy::Reject);
  }

  ControllerActionResult submitPreparedSource(const SkinPackageId &package,
                                              PackageCollisionPolicy policy) {
    if (!pickedSource || !disposalReservation) {
      return rejected(i18n::message("settings.skins.no_selected_skin_source_ready_import.message"));
    }
    SkinPackageOperationHandle handle;
    const SkinSafetyPolicy safetyPolicy(
        dependencies.profileOwner.snapshot(dependencies.profileId, dependencies.orientation)
            .settings.safetyLevel);
    if (pickedSource->temporaryPathKind == PlatformTemporaryPathKind::File) {
      handle = dependencies.operations.submitPrepareArchive(
          pickedSource->localPath, package, sourceCleanup(pickedSource),
          safetyPolicy);
    } else if (pickedSource->temporaryPathKind ==
               PlatformTemporaryPathKind::Directory) {
      handle = dependencies.operations.submitPrepareFolder(
          pickedSource->localPath, package, sourceCleanup(pickedSource),
          safetyPolicy);
    } else {
      return rejected(i18n::message("settings.skins.selected_source_has_no_supported_path_kind.message"));
    }
    if (handle.ticket == 0) {
      rejectPrepareSubmission(std::move(handle));
      return rejected(statusText);
    }
    pickedSource.reset();
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    projected.hasPackageProgress = true;
    collisionPolicy = policy;
    phase = Phase::PreparingPackage;
    projected.preparedName.reset();
    projected.collisionPackage.reset();
    setBusy(i18n::message("settings.skins.preparing_skin_package.progress"));
    return accepted(i18n::message("settings.skins.skin_package_preparation_started.message"));
  }

  void pollInventory() {
    auto result =
        dependencies.profileSnapshots.pollSnapshotAllProfiles(inventoryTicket);
    if (!result) {
      return;
    }
    dependencies.profileSnapshots.cancelSnapshotAllProfiles(inventoryTicket);
    inventoryTicket = 0;
    if (result->cancelled || !result->complete || !result->inventory) {
      disposeLocalPrepared();
      setError(firstDiagnosticMessage(
          result->diagnostics, i18n::message("settings.skins.complete_profile_inventory_required.message")));
      return;
    }
    submitPublish(std::move(*result->inventory));
  }

  void pollRemovalInventory() {
    auto result =
        dependencies.profileSnapshots.pollSnapshotAllProfiles(inventoryTicket);
    if (!result) {
      return;
    }
    dependencies.profileSnapshots.cancelSnapshotAllProfiles(inventoryTicket);
    inventoryTicket = 0;
    if (result->cancelled || !result->complete || !result->inventory ||
        !removalPackage) {
      removalPackage.reset();
      setError(firstDiagnosticMessage(
          result->diagnostics, i18n::message("settings.skins.complete_profile_inventory_required.message")));
      return;
    }

    const auto selectedByAnyProfile = std::ranges::any_of(
        result->inventory->profiles, [&](const auto &profile) {
          return std::ranges::any_of(
              profile.settings.selectedSkinEntries,
              [&](const auto &selection) {
                return selection.second.package.collisionKey ==
                       removalPackage->collisionKey;
              });
        });
    if (selectedByAnyProfile) {
      removalPackage.reset();
      setError(i18n::message("settings.skins.remove_skin_from_every_profile_before_deleting.message"));
      return;
    }

    std::optional<ProfileInventoryCommitFence> fence;
    try {
      fence = dependencies.profileSnapshots.tryAcquireInventoryCommitFence(
          *result->inventory);
    } catch (...) {
    }
    if (!fence) {
      removalPackage.reset();
      setError(i18n::message("settings.skins.skin_profile_selections_changed_before_package_removal.message"));
      return;
    }

    auto handle = dependencies.operations.submitRemove(*removalPackage);
    if (handle.ticket == 0) {
      removalPackage.reset();
      setError(i18n::message("settings.skins.skin_package_removal_failed_queued.message"));
      return;
    }
    removalFence.emplace(std::move(*fence));
    removalPackage.reset();
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    phase = Phase::Removing;
    setBusy(i18n::message("settings.skins.removing_skin_package.progress"));
  }

  void pollPreparePackage(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PreparePackageResult>(&completion.payload);
    if (!result || !result->prepared) {
      releaseDisposalReservation();
      setError(result ? firstDiagnosticMessage(result->diagnostics,
                                               result->cancelled
                                                   ? i18n::message("settings.skins.skin_import_cancelled.message")
                                                   : i18n::message("settings.skins.skin_package_invalid.message"))
                      : i18n::message("settings.skins.unexpected_package_preparation_result.message"));
      return;
    }
    preparedPackage.emplace(std::move(*result->prepared));
    i18n::Text error;
    if (!beginInventory(error)) {
      disposeLocalPrepared();
      setError(std::move(error));
    }
  }

  void pollPublish(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PublishPackageResult>(&completion.payload);
    if (!result) {
      releaseDisposalReservation();
      setError(i18n::message("settings.skins.unexpected_package_publication_result.message"));
      return;
    }
    if (result->published) {
      releaseDisposalReservation();
      projected.preparedName.reset();
      projected.collisionPackage.reset();
      setIdle(i18n::message("settings.skins.skin_package_installed.message"));
      return;
    }
    if (result->retryableInventoryRace && result->retryPrepared) {
      preparedPackage.emplace(std::move(*result->retryPrepared));
      i18n::Text error;
      if (!beginInventory(error)) {
        disposeLocalPrepared();
        setError(std::move(error));
      }
      return;
    }
    if (result->retryPrepared) {
      preparedPackage.emplace(std::move(*result->retryPrepared));
      disposeLocalPrepared();
    } else {
      releaseDisposalReservation();
    }
    setError(firstDiagnosticMessage(result->diagnostics,
                                    i18n::message("settings.skins.skin_package_publication_failed.message")));
  }

  void pollPrepareActivation(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PrepareActivationResult>(&completion.payload);
    if (!result || !result->prepared) {
      setError(result
                   ? firstDiagnosticMessage(
                         result->diagnostics,
                         result->cancelled ? i18n::message("settings.skins.skin_activation_cancelled.message")
                                           : i18n::message("settings.skins.skin_configuration_invalid.message"))
                   : i18n::message("settings.skins.unexpected_activation_preparation_result.message"));
      return;
    }
    auto submitted = dependencies.commits.submitActivation(
        dependencies.clientId, std::move(*result->prepared));
    if (!submitted.accepted) {
      setError(firstDiagnosticMessage(
          submitted.diagnostics, i18n::message("settings.skins.skin_activation_failed_committed.message")));
      return;
    }
    phase = Phase::WaitingActivationCommit;
    projected.state = GameplaySkinSettingsState::Busy;
    setStatus(i18n::message("settings.skins.saving_selected_skin.progress"));
    projected.canCancel = false;
  }

  void pollRemove(SkinPackageOperationCompletion completion) {
    removalFence.reset();
    removalPackage.reset();
    auto *result = std::get_if<RemovePackageResult>(&completion.payload);
    if (result && result->removed) {
      setIdle(i18n::message("settings.skins.skin_package_removed.message"));
      return;
    }
    setError(result ? firstDiagnosticMessage(result->diagnostics,
                                             i18n::message("settings.skins.skin_package_removal_failed.message"))
                    : i18n::message("settings.skins.unexpected_package_removal_result.message"));
  }

  void pollPackageOperation() {
    if (progress) {
      projected.progress = progress->snapshot();
    }
    if (operationTicket == 0) {
      return;
    }
    auto completion = dependencies.operations.poll(operationTicket);
    if (!completion) {
      return;
    }
    operationTicket = 0;
    progress.reset();
    if (phase == Phase::PreparingPackage) {
      pollPreparePackage(std::move(*completion));
    } else if (phase == Phase::Publishing) {
      pollPublish(std::move(*completion));
    } else if (phase == Phase::PreparingActivation) {
      pollPrepareActivation(std::move(*completion));
    } else if (phase == Phase::Removing) {
      pollRemove(std::move(*completion));
    }
  }

  void pollCommitCompletions() {
    auto activations =
        dependencies.commits.takeCompletions(dependencies.clientId);
    if (!activations.empty() && phase == Phase::WaitingActivationCommit) {
      const auto disposition = activations.back().result.disposition;
      if (disposition == ActivationCommitDisposition::ActivatedRequested) {
        setIdle(i18n::message("settings.skins.selected_skin_saved.message"));
      } else {
        setError(
            firstDiagnosticMessage(activations.back().result.diagnostics,
                                   i18n::message("settings.skins.selected_skin_failed_activated.message")));
      }
    }
    auto profiles =
        dependencies.commits.takeProfileCompletions(dependencies.clientId);
    if (!profiles.empty() && phase == Phase::WaitingProfileCommit) {
      if (profiles.back().result.status ==
          SkinProfileCommitResult::Status::Persisted) {
        setIdle(i18n::message("settings.skins.gameplay_skin_settings_saved.message"));
      } else {
        setError(i18n::message("settings.skins.gameplay_skin_settings_failed_saved.message"));
      }
    }
  }

  void pollRescan() {
    if (!dependencies.rescanProgress) {
      setError(i18n::message("settings.skins.gameplay_skin_rescan_service_unavailable.message"));
      return;
    }
    try {
      projected.rescanProgress = dependencies.rescanProgress();
    } catch (...) {
      setError(i18n::message("settings.skins.gameplay_skin_rescan_failed_observed.message"));
      return;
    }
    switch (projected.rescanProgress.phase) {
    case SkinRescanProgressPhase::Idle:
      return;
    case SkinRescanProgressPhase::LoadingProfileInventory:
      setStatus(i18n::message("settings.skins.loading_skin_profile_inventory.progress"));
      return;
    case SkinRescanProgressPhase::ReconcilingActivations:
      setStatus(i18n::message("settings.skins.reconciling_skin_activations.progress"));
      return;
    case SkinRescanProgressPhase::ScanningVisiblePackages:
      setStatus(i18n::message("settings.skins.scanning_skin_packages.progress"));
      return;
    case SkinRescanProgressPhase::Succeeded:
      setIdle(i18n::message("settings.skins.skin_scan_complete.message"));
      return;
    case SkinRescanProgressPhase::Failed:
      setError(i18n::message("settings.skins.skin_scan_did_not_complete_check_diagnostics.message"));
      return;
    }
  }

  void poll() {
    if (closed) {
      return;
    }
    if (phase == Phase::PickingArchive || phase == Phase::PickingFolder) {
      pollHandoff();
    } else if (phase == Phase::LoadingInventory) {
      pollInventory();
    } else if (phase == Phase::LoadingRemovalInventory) {
      pollRemovalInventory();
    } else if (phase == Phase::PreparingPackage || phase == Phase::Publishing ||
               phase == Phase::PreparingActivation ||
               phase == Phase::Removing) {
      pollPackageOperation();
    } else if (phase == Phase::Rescanning) {
      pollRescan();
    }
    pollCommitCompletions();
    refreshProjection();
  }

  ControllerActionResult beginImport(bool archive) {
    if (closed) {
      return rejected(i18n::message("settings.skins.gameplay_skin_settings_closed.message"));
    }
    if (hasControllerOperation()) {
      return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
    }
    auto reservation = dependencies.operations.reservePreparedDisposal();
    if (!reservation) {
      return rejected(i18n::message("settings.skins.skin_staging_disposal_currently_unavailable.message"));
    }
    disposalReservation.emplace(std::move(*reservation));
    projected.preparedName.reset();
    projected.collisionPackage.reset();
    projected.hasPackageProgress = false;
    try {
      if (archive) {
        handoff =
            dependencies.beginArchiveHandoff
                ? dependencies.beginArchiveHandoff()
                : platform_document_handoff::PlatformDocumentHandoffOperation{};
        phase = Phase::PickingArchive;
      } else {
        handoff =
            dependencies.beginFolderHandoff
                ? dependencies.beginFolderHandoff(
                      {.maxBytes = SkinPackagePolicy::maxExpandedBytes,
                       .maxFiles = SkinPackagePolicy::maxFiles,
                       .maxRegularFileBytes =
                           SkinPackagePolicy::maxRegularFileBytes,
                       .maxDepth = SkinPackagePolicy::maxPathComponents,
                       .maxPathBytes = SkinPackagePolicy::maxPathBytes})
                : platform_document_handoff::PlatformDocumentHandoffOperation{};
        phase = Phase::PickingFolder;
      }
    } catch (const std::exception &exception) {
      releaseDisposalReservation();
      setError(exception.what());
      return rejected(statusText);
    } catch (...) {
      releaseDisposalReservation();
      setError(i18n::message("settings.skins.document_picker_failed_started.message"));
      return rejected(statusText);
    }
    if (!handoff) {
      releaseDisposalReservation();
      setError(i18n::message("settings.skins.document_picker_unavailable.message"));
      return rejected(statusText);
    }
    setBusy(archive
                ? i18n::message("settings.skins.selecting_skin_archive.progress")
                : i18n::message("settings.skins.selecting_skin_folder.progress"));
    return accepted(i18n::message("settings.skins.skin_source_selection_started.message"));
  }

  ControllerActionResult setSuggestedPackageName(std::string packageName) {
    if (closed || phase != Phase::NameReady || !projected.preparedName) {
      return rejected(i18n::message("settings.skins.no_source_awaiting_package_name.message"));
    }
    auto normalized = normalizePackageId(packageName);
    projected.preparedName->suggestedPackageName = std::move(packageName);
    projected.preparedName->validationError =
        normalized.package ? std::string{} : std::move(normalized.error);
    if (normalized.package) {
      projected.preparedName->suggestedPackageName =
          normalized.package->directoryName;
    }
    projected.collisionPackage.reset();
    refreshCachedPresentationKey();
    return {.accepted = normalized.package.has_value(),
            .message = normalized.package
                           ? i18n::message("settings.skins.package_name_updated.message")
                           : i18n::Text(projected.preparedName->validationError)};
  }

  ControllerActionResult
  confirmPreparedImport(PackageCollisionPolicy requestedPolicy) {
    if (closed || phase != Phase::NameReady || !pickedSource ||
        !projected.preparedName || !disposalReservation) {
      return rejected(i18n::message("settings.skins.no_selected_skin_source_ready_import.message"));
    }
    auto normalized =
        normalizePackageId(projected.preparedName->suggestedPackageName);
    if (!normalized.package) {
      projected.preparedName->validationError = normalized.error;
      return rejected(normalized.error);
    }
    projected.preparedName->suggestedPackageName =
        normalized.package->directoryName;
    projected.preparedName->validationError.clear();

    const auto catalogValue = catalog();
    if (catalogValue) {
      const auto collision = std::ranges::find_if(
          catalogValue->packages, [&](const SkinPackageId &package) {
            return package.collisionKey == normalized.package->collisionKey;
          });
      if (collision != catalogValue->packages.end() &&
          requestedPolicy == PackageCollisionPolicy::Reject) {
        projected.collisionPackage = *collision;
        refreshCachedPresentationKey();
        return rejected(i18n::message("settings.skins.package_name_already_installed.message"));
      }
    }

    return submitPreparedSource(*normalized.package, requestedPolicy);
  }

  ControllerActionResult prepareActivation(SkinEntryId entry,
                                           SkinProfileSettings candidate,
                                           i18n::Text message,
                                           std::optional<int> target = std::nullopt) {
    if (closed || hasControllerOperation()) {
      return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
    }
    auto base = dependencies.profileOwner.snapshot(dependencies.profileId, dependencies.orientation);
    candidate.sanitize();
    auto handle = dependencies.operations.submitPrepareActivation(
        std::move(base), std::move(entry), std::move(candidate), target);
    if (handle.ticket == 0) {
      return rejected(i18n::message("settings.skins.activation_preparation_failed_queued.message"));
    }
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    projected.hasPackageProgress = true;
    phase = Phase::PreparingActivation;
    setBusy(std::move(message));
    return accepted(i18n::message("settings.skins.activation_preparation_started.message"));
  }

  ControllerActionResult submitProfileOnly(SkinProfileSettings candidate) {
    if (closed || hasControllerOperation()) {
      return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
    }
    const auto base =
        dependencies.profileOwner.snapshot(dependencies.profileId, dependencies.orientation);
    candidate.sanitize();
    auto submission = dependencies.commits.submitProfileSettings(
        dependencies.clientId, base, std::move(candidate));
    if (!submission.accepted) {
      return {.message = firstDiagnosticMessage(
                  submission.diagnostics,
                  i18n::message("settings.skins.settings_failed_queued.message")).resolve(),
              .diagnostics = std::move(submission.diagnostics)};
    }
    phase = Phase::WaitingProfileCommit;
    projected.state = GameplaySkinSettingsState::Busy;
    setStatus(i18n::message("settings.skins.saving_settings.progress"));
    projected.canCancel = false;
    return accepted(i18n::message("settings.skins.settings_save_started.message"));
  }

  void abortUncommittedOperation() noexcept {
    if (closed) {
      return;
    }
    if (phase == Phase::WaitingActivationCommit ||
        phase == Phase::WaitingProfileCommit || phase == Phase::Rescanning) {
      return;
    }
    try {
      if (handoff) {
        handoff.abandon();
      }
      if (operationTicket != 0) {
        dependencies.operations.cancelAndDetach(operationTicket);
        operationTicket = 0;
      }
      if (inventoryTicket != 0) {
        dependencies.profileSnapshots.cancelSnapshotAllProfiles(
            inventoryTicket);
        inventoryTicket = 0;
      }
      removalFence.reset();
      removalPackage.reset();
      if (preparedPackage) {
        disposeLocalPrepared();
      } else {
        transferPickedCleanup();
      }
      projected.preparedName.reset();
      projected.collisionPackage.reset();
      projected.hasPackageProgress = false;
      progress.reset();
      phase = Phase::Idle;
      errorState = false;
      setStatus(i18n::message("settings.skins.operation_abandoned.message"));
      projected.canCancel = false;
      refreshProjection();
    } catch (...) {
      std::terminate();
    }
  }

  void close() noexcept {
    if (closed) {
      return;
    }
    try {
      if (handoff) {
        handoff.abandon();
      }
      if (operationTicket != 0) {
        dependencies.operations.cancelAndDetach(operationTicket);
        operationTicket = 0;
      }
      if (inventoryTicket != 0) {
        dependencies.profileSnapshots.cancelSnapshotAllProfiles(
            inventoryTicket);
        inventoryTicket = 0;
      }
      removalFence.reset();
      removalPackage.reset();
      if (preparedPackage) {
        disposeLocalPrepared();
      } else {
        transferPickedCleanup();
      }
      projected.preparedName.reset();
      projected.collisionPackage.reset();
      projected.hasPackageProgress = false;
      progress.reset();
      dependencies.commits.detachClient(dependencies.clientId);
      closed = true;
      phase = Phase::Idle;
      errorState = false;
      projected.canCancel = false;
    } catch (...) {
      std::terminate();
    }
  }
};

GameplaySkinSettingsController::GameplaySkinSettingsController(
    GameplaySkinSettingsControllerDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies))) {}

GameplaySkinSettingsController::~GameplaySkinSettingsController() { close(); }

const GameplaySkinSettingsSnapshot &
GameplaySkinSettingsController::snapshot() const noexcept {
  return impl_->projected;
}

void GameplaySkinSettingsController::setActiveTarget(int skinType) {
  if (!skinTargetTraitForType(skinType) || impl_->activeTarget == skinType) return;
  impl_->activeTarget = skinType;
  impl_->projectionInputsReady = false;
  impl_->refreshProjection();
}

void GameplaySkinSettingsController::poll() { impl_->poll(); }

void GameplaySkinSettingsController::profileChanged(
    SkinProfileId profileId, SkinActivationClientId clientId, PresentationOrientation orientation) {
  if (impl_->closed) {
    return;
  }
  impl_->abortUncommittedOperation();
  impl_->dependencies.commits.detachClient(impl_->dependencies.clientId);
  // Accepted coordinator transactions are durable after detachment, but their
  // delivery-only wait state belongs to the old profile binding.
  impl_->phase = Impl::Phase::Idle;
  impl_->errorState = false;
  impl_->progress.reset();
  impl_->projected.hasPackageProgress = false;
  impl_->projected.progress = {};
  impl_->projected.canCancel = false;
  impl_->projected.pendingSafetyLevel.reset();
  impl_->dependencies.profileId = std::move(profileId);
  impl_->dependencies.orientation = orientation;
  impl_->dependencies.clientId = clientId;
  impl_->setStatus({});
  impl_->refreshProjection();
}

ControllerActionResult GameplaySkinSettingsController::beginArchiveImport() {
  return impl_->beginImport(true);
}

ControllerActionResult GameplaySkinSettingsController::beginFolderImport() {
  return impl_->beginImport(false);
}

ControllerActionResult GameplaySkinSettingsController::setSuggestedPackageName(
    std::string packageName) {
  return impl_->setSuggestedPackageName(std::move(packageName));
}

ControllerActionResult GameplaySkinSettingsController::confirmPreparedImport(
    PackageCollisionPolicy collisionPolicy) {
  return impl_->confirmPreparedImport(collisionPolicy);
}

ControllerActionResult GameplaySkinSettingsController::requestRescan() {
  if (impl_->closed || impl_->hasControllerOperation() ||
      !impl_->dependencies.requestRescan || !impl_->dependencies.cancelRescan ||
      !impl_->dependencies.rescanProgress) {
    return rejected(i18n::message("settings.skins.rescan_unavailable.message"));
  }
  impl_->dependencies.requestRescan();
  impl_->phase = Impl::Phase::Rescanning;
  impl_->projected.hasPackageProgress = false;
  impl_->projected.progress = {};
  try {
    impl_->projected.rescanProgress = impl_->dependencies.rescanProgress();
  } catch (...) {
    impl_->setError(i18n::message("settings.skins.gameplay_skin_rescan_failed_observed.message"));
    return rejected(impl_->statusText);
  }
  impl_->setBusy(i18n::message("settings.skins.preparing_scan.progress"));
  return accepted(i18n::message("settings.skins.rescan_requested.message"));
}

ControllerActionResult
GameplaySkinSettingsController::requestRevalidation(const SkinEntryId &entry) {
  if (impl_->closed || impl_->hasControllerOperation() ||
      !impl_->dependencies.requestRevalidation) {
    return rejected(i18n::message("settings.skins.revalidation_unavailable.message"));
  }
  impl_->dependencies.requestRevalidation(entry);
  return accepted(i18n::message("settings.skins.revalidation_requested.message"));
}

ControllerActionResult
GameplaySkinSettingsController::select(const SkinEntryId &entry) {
  const auto catalogValue = impl_->catalog();
  const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
  const auto skinType = selectableGameplaySkinType(catalogEntry);
  if (!skinType) {
    return rejected(i18n::message("settings.skins.validated_skin_required_selection.message"));
  }
  return selectGameplayTrait(*skinType, entry);
}

ControllerActionResult
GameplaySkinSettingsController::selectGameplayTrait(int skinType,
                                                    const SkinEntryId &entry) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  if (!skinTargetTraitForType(skinType)) {
    return rejected(i18n::message("settings.skins.trait_unavailable.message"));
  }
  const auto catalogValue = impl_->catalog();
  const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
  const auto entrySkinType = selectableGameplaySkinType(catalogEntry);
  if (!entrySkinType || *entrySkinType != skinSourceTypeForTarget(skinType)) {
    return rejected(i18n::message("settings.skins.skin_trait_unsupported.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  if (skinType == -5) candidate.follow5K1S = false;
  if (skinType == -7) candidate.follow7K1S = false;
  candidate.selectedSkinEntries.insert_or_assign(skinType, entry);
  candidate.entriesForTarget(skinType).try_emplace(entry);
  auto selectedEntry = entry;
  setActiveTarget(skinType);
  return impl_->prepareActivation(std::move(selectedEntry), std::move(candidate),
                                  i18n::message("settings.skins.validating_selected_skin.progress"), skinType);
}

ControllerActionResult
GameplaySkinSettingsController::clearGameplayTrait(int skinType) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  if (!skinTargetTraitForType(skinType)) {
    return rejected(i18n::message("settings.skins.trait_unavailable.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  if (skinType == -5) candidate.follow5K1S = false;
  if (skinType == -7) candidate.follow7K1S = false;
  candidate.selectedSkinEntries.erase(skinType);
  candidate.selectedGameplayEntries.erase(skinType);
  // An empty new-format map must not be repopulated from a legacy alias.
  candidate.selected7KeyEntry.reset();
  candidate.gameplayCompatibilityEnabled = false;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::followGameplayTrait(int skinType) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  if (skinType != -5 && skinType != -7) {
    return rejected(i18n::message("settings.skins.trait_unavailable.message"));
  }
  auto candidate = impl_->dependencies.profileOwner.snapshot(
      impl_->dependencies.profileId, impl_->dependencies.orientation).settings;
  (skinType == -5 ? candidate.follow5K1S : candidate.follow7K1S) = true;
  candidate.selectedSkinEntries.erase(skinType);
  candidate.selectedGameplayEntries.erase(skinType);
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::setCompatibilityEnabled(bool enabled) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  if (enabled) {
    if (candidate.selectedSkinEntries.empty()) {
      return rejected(i18n::message("settings.skins.select_validated_skin_first.message"));
    }
    const auto catalogValue = impl_->catalog();
    for (const auto &[skinType, entry] : candidate.selectedSkinEntries) {
      const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
      const auto entrySkinType = selectableGameplaySkinType(catalogEntry);
      const auto &targetEntries = candidate.entriesForTarget(skinType);
      const auto settings = targetEntries.find(entry);
      const EntryProfileSettings defaults;
      const auto &configured =
          settings == targetEntries.end() ? defaults : settings->second;
      if (!entrySkinType || *entrySkinType != skinSourceTypeForTarget(skinType) ||
          !containsConfiguration(*catalogEntry, configured)) {
        return rejected(i18n::message("settings.skins.selected_configuration_not_validated.message"));
      }
    }
  } else {
    candidate.selectedSkinEntries.clear();
    candidate.selectedGameplayEntries.clear();
    candidate.selected7KeyEntry.reset();
  }
  candidate.gameplayCompatibilityEnabled = enabled;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::setSafetyLevel(SkinSafetyLevel level) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  if (level == SkinSafetyLevel::Unrestricted &&
      impl_->projected.safetyLevel != SkinSafetyLevel::Unrestricted) {
    impl_->projected.pendingSafetyLevel = level;
    impl_->refreshCachedPresentationKey();
    return accepted(i18n::message("settings.skins.unrestricted_confirmation_required.message"), false);
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  candidate.safetyLevel = level;
  impl_->projected.pendingSafetyLevel.reset();
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::confirmSafetyLevelChange() {
  if (impl_->closed || impl_->hasControllerOperation() ||
      !impl_->projected.pendingSafetyLevel) {
    return rejected(i18n::message("settings.skins.no_safety_confirmation_pending.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  candidate.safetyLevel = *impl_->projected.pendingSafetyLevel;
  const auto result = impl_->submitProfileOnly(std::move(candidate));
  if (result.accepted) {
    impl_->projected.pendingSafetyLevel.reset();
    impl_->refreshCachedPresentationKey();
  }
  return result;
}

void GameplaySkinSettingsController::cancelSafetyLevelChange() noexcept {
  if (impl_->closed) {
    return;
  }
  impl_->projected.pendingSafetyLevel.reset();
  impl_->refreshCachedPresentationKey();
}

ControllerActionResult
GameplaySkinSettingsController::setOption(const SkinEntryId &entry,
                                          std::string name, int value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected(i18n::message("settings.skins.validated_skin_required_configuration.message"));
  }
  const int target = impl_->configurationTarget(*skinType);
  candidate.entriesForTarget(target)[entry].options[std::move(name)] = value;
  candidate.selectedSkinEntries.insert_or_assign(target, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  i18n::message("settings.skins.validating_option.progress"), target);
}

ControllerActionResult GameplaySkinSettingsController::setFileChoice(
    const SkinEntryId &entry, std::string name, std::string value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected(i18n::message("settings.skins.validated_skin_required_configuration.message"));
  }
  const int target = impl_->configurationTarget(*skinType);
  candidate.entriesForTarget(target)[entry].filePaths[std::move(name)] = std::move(value);
  candidate.selectedSkinEntries.insert_or_assign(target, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  i18n::message("settings.skins.validating_file_choice.progress"), target);
}

ControllerActionResult GameplaySkinSettingsController::setOffset(
    const SkinEntryId &entry, std::string name, ConfigOffset value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected(i18n::message("settings.skins.validated_skin_required_configuration.message"));
  }
  const int target = impl_->configurationTarget(*skinType);
  candidate.entriesForTarget(target)[entry].offsets[std::move(name)] = value;
  candidate.selectedSkinEntries.insert_or_assign(target, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  i18n::message("settings.skins.validating_offset.progress"), target);
}

ControllerActionResult
GameplaySkinSettingsController::setViewport(const SkinEntryId &entry,
                                            ViewportSettings viewport) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId, impl_->dependencies.orientation)
          .settings;
  const auto declared = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  const int target = impl_->configurationTarget(declared.value_or(0));
  candidate.entriesForTarget(target)[entry].viewport = viewport;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::requestRemoval(const SkinPackageId &package) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected(i18n::message("settings.skins.another_gameplay_skin_operation_active.message"));
  }
  i18n::Text error;
  if (!impl_->beginRemovalInventory(package, error)) {
    impl_->setError(std::move(error));
    return rejected(impl_->statusText);
  }
  return accepted(i18n::message("settings.skins.checking_selections_before_removal.message"));
}

ControllerActionResult
GameplaySkinSettingsController::resetLayout(const SkinEntryId &entry) {
  return setViewport(entry, ViewportSettings{.mode = ViewportMode::Fit});
}

void GameplaySkinSettingsController::cancelRescan() noexcept {
  if (impl_->closed || impl_->phase != Impl::Phase::Rescanning ||
      !impl_->dependencies.cancelRescan) {
    return;
  }
  try {
    impl_->dependencies.cancelRescan();
    impl_->setIdle(i18n::message("settings.skins.scan_cancelled.message"));
  } catch (...) {
    std::terminate();
  }
}

void GameplaySkinSettingsController::close() noexcept { impl_->close(); }

} // namespace skin
