#include "GameplaySkinSettingsController.h"

#include "../skin/GameplaySkinTraits.h"
#include "../skin/package/SkinPathPolicy.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

namespace skin {
namespace {

bool endsWithZipAsciiCaseInsensitive(std::string_view value) {
  if (value.size() < 4) {
    return false;
  }
  const auto suffix = value.substr(value.size() - 4);
  return suffix[0] == '.' && (suffix[1] == 'z' || suffix[1] == 'Z') &&
         (suffix[2] == 'i' || suffix[2] == 'I') &&
         (suffix[3] == 'p' || suffix[3] == 'P');
}

ControllerActionResult rejected(std::string message) {
  return {.message = std::move(message)};
}

ControllerActionResult accepted(std::string message, bool asynchronous = true) {
  return {.accepted = true,
          .asynchronous = asynchronous,
          .message = std::move(message)};
}

std::string firstDiagnosticMessage(const std::vector<SkinDiagnostic> &values,
                                   std::string fallback) {
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
      !gameplaySkinTraitForSkinType(catalogEntry->metadata->skinType)) {
    return std::nullopt;
  }
  return catalogEntry->metadata->skinType;
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
  if (pathKind == PlatformTemporaryPathKind::File &&
      endsWithZipAsciiCaseInsensitive(proposed)) {
    proposed.resize(proposed.size() - 4);
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
  enum class Phase : std::uint8_t {
    Idle,
    PickingArchive,
    PickingFolder,
    NameReady,
    PreparingPackage,
    LoadingInventory,
    Publishing,
    PreparingActivation,
    WaitingActivationCommit,
    WaitingProfileCommit,
    Removing,
  };

  explicit Impl(GameplaySkinSettingsControllerDependencies dependencies)
      : dependencies(std::move(dependencies)) {
    refreshProjection();
  }

  GameplaySkinSettingsControllerDependencies dependencies;
  GameplaySkinSettingsSnapshot projected;
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
  std::shared_ptr<const SkinPackageCatalogSnapshot> projectedCatalog;
  std::string projectedProfileId;
  std::uint64_t projectedProfileGeneration = 0;
  bool projectionInputsReady = false;

  void refreshCachedPresentationKey() {
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
    append("v1");
    number(projectedCatalog ? projectedCatalog->catalogGeneration : 0);
    number(projectedCatalog ? projectedCatalog->sourceGeneration : 0);
    append(projectedProfileId);
    number(projectedProfileGeneration);
    number(static_cast<unsigned>(projected.state));
    number(projected.compatibilityEnabled);
    append(projected.statusMessage);
    number(projected.canCancel);
    number(static_cast<unsigned>(projected.progress.phase));
    number(projected.progress.completedBytes);
    number(projected.progress.totalBytes);
    number(projected.progress.completedFiles);
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
    return phase == Phase::PickingArchive || phase == Phase::PickingFolder ||
           phase == Phase::NameReady || phase == Phase::PreparingPackage ||
           phase == Phase::LoadingInventory || phase == Phase::Publishing ||
           phase == Phase::PreparingActivation || phase == Phase::Removing;
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

  void refreshProjection() {
    if (closed) {
      return;
    }
    const auto profile =
        dependencies.profileOwner.snapshot(dependencies.profileId);
    const auto catalogValue = catalog();
    const bool inputsChanged = !projectionInputsReady ||
                               projectedCatalog != catalogValue ||
                               projectedProfileId != profile.profileId.opaque ||
                               projectedProfileGeneration != profile.generation;
    if (inputsChanged) {
      // Keep the full, display-ready catalog projection stable between catalog
      // or profile generations. Opening a dropdown must not clone every skin
      // title, configuration declaration, and diagnostic again.
      projected.compatibilityEnabled =
          profile.settings.gameplayCompatibilityEnabled;
      projected.selectedGameplayEntries =
          profile.settings.selectedGameplayEntries;
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
          if (const auto settings = profile.settings.entries.find(source.entry);
              settings != profile.settings.entries.end()) {
            row.settings = settings->second;
          }
          row.configurationDigest = skinConfigurationDigest(row.settings);
          projected.entries.push_back(std::move(row));
        }
      }
      projectedCatalog = catalogValue;
      projectedProfileId = profile.profileId.opaque;
      projectedProfileGeneration = profile.generation;
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

  void setBusy(std::string message) {
    errorState = false;
    projected.state = GameplaySkinSettingsState::Busy;
    projected.statusMessage = std::move(message);
    projected.canCancel = phaseCanCancel();
    refreshCachedPresentationKey();
  }

  void setError(std::string message) {
    errorState = true;
    phase = Phase::Idle;
    projected.state = GameplaySkinSettingsState::Error;
    projected.statusMessage = std::move(message);
    projected.canCancel = false;
    projected.progress = {};
    progress.reset();
    refreshCachedPresentationKey();
  }

  void setIdle(std::string message = {}) {
    errorState = false;
    phase = Phase::Idle;
    projected.statusMessage = std::move(message);
    projected.canCancel = false;
    projected.progress = {};
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
    setError("Skin package preparation could not be queued.");
  }

  bool beginInventory(std::string &error) {
    try {
      inventoryTicket =
          dependencies.profileSnapshots.beginSnapshotAllProfiles();
      if (inventoryTicket == 0) {
        error = "Profile inventory could not be started.";
        return false;
      }
      phase = Phase::LoadingInventory;
      setBusy("Loading profile inventory…");
      return true;
    } catch (const std::exception &exception) {
      error = exception.what();
      return false;
    } catch (...) {
      error = "Profile inventory could not be started.";
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
      setError("Skin package publication could not be queued.");
      return;
    }
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    phase = Phase::Publishing;
    setBusy("Publishing skin package…");
  }

  void pollHandoff() {
    if (!handoff || !handoff.ready()) {
      return;
    }
    auto result = handoff.takeResult();
    handoff.close();
    if (!result) {
      releaseDisposalReservation();
      setError("The document picker returned no result.");
      return;
    }
    if (result->cancelled()) {
      pickedSource =
          std::make_shared<PlatformDocumentHandoffResult>(std::move(*result));
      transferPickedCleanup();
      setIdle("Skin import cancelled.");
      return;
    }
    if (!result->ok()) {
      const auto message = result->message.empty()
                               ? "Skin source selection failed."
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
    projected.collisionPackage.reset();
    errorState = false;
    phase = Phase::NameReady;
    projected.state = GameplaySkinSettingsState::Ready;
    projected.statusMessage = "Confirm the skin package name.";
    projected.canCancel = true;
    refreshProjection();
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
          result->diagnostics, "A complete profile inventory is required."));
      return;
    }
    submitPublish(std::move(*result->inventory));
  }

  void pollPreparePackage(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PreparePackageResult>(&completion.payload);
    if (!result || !result->prepared) {
      releaseDisposalReservation();
      setError(result ? firstDiagnosticMessage(result->diagnostics,
                                               result->cancelled
                                                   ? "Skin import cancelled."
                                                   : "Skin package is invalid.")
                      : "Unexpected package preparation result.");
      return;
    }
    preparedPackage.emplace(std::move(*result->prepared));
    std::string error;
    if (!beginInventory(error)) {
      disposeLocalPrepared();
      setError(std::move(error));
    }
  }

  void pollPublish(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PublishPackageResult>(&completion.payload);
    if (!result) {
      releaseDisposalReservation();
      setError("Unexpected package publication result.");
      return;
    }
    if (result->published) {
      releaseDisposalReservation();
      projected.preparedName.reset();
      projected.collisionPackage.reset();
      setIdle("Skin package installed.");
      return;
    }
    if (result->retryableInventoryRace && result->retryPrepared) {
      preparedPackage.emplace(std::move(*result->retryPrepared));
      std::string error;
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
                                    "Skin package publication failed."));
  }

  void pollPrepareActivation(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<PrepareActivationResult>(&completion.payload);
    if (!result || !result->prepared) {
      setError(result
                   ? firstDiagnosticMessage(
                         result->diagnostics,
                         result->cancelled ? "Skin activation cancelled."
                                           : "Skin configuration is invalid.")
                   : "Unexpected activation preparation result.");
      return;
    }
    auto submitted = dependencies.commits.submitActivation(
        dependencies.clientId, std::move(*result->prepared));
    if (!submitted.accepted) {
      setError(firstDiagnosticMessage(
          submitted.diagnostics, "Skin activation could not be committed."));
      return;
    }
    phase = Phase::WaitingActivationCommit;
    projected.state = GameplaySkinSettingsState::Busy;
    projected.statusMessage = "Saving selected skin…";
    projected.canCancel = false;
  }

  void pollRemove(SkinPackageOperationCompletion completion) {
    auto *result = std::get_if<RemovePackageResult>(&completion.payload);
    if (result && result->removed) {
      setIdle("Skin package removed.");
      return;
    }
    setError(result ? firstDiagnosticMessage(result->diagnostics,
                                             "Skin package removal failed.")
                    : "Unexpected package removal result.");
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
        setIdle("Selected skin saved.");
      } else {
        setError(
            firstDiagnosticMessage(activations.back().result.diagnostics,
                                   "Selected skin could not be activated."));
      }
    }
    auto profiles =
        dependencies.commits.takeProfileCompletions(dependencies.clientId);
    if (!profiles.empty() && phase == Phase::WaitingProfileCommit) {
      if (profiles.back().result.status ==
          SkinProfileCommitResult::Status::Persisted) {
        setIdle("Gameplay skin settings saved.");
      } else {
        setError("Gameplay skin settings could not be saved.");
      }
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
    } else if (phase == Phase::PreparingPackage || phase == Phase::Publishing ||
               phase == Phase::PreparingActivation ||
               phase == Phase::Removing) {
      pollPackageOperation();
    }
    pollCommitCompletions();
    refreshProjection();
  }

  ControllerActionResult beginImport(bool archive) {
    if (closed) {
      return rejected("Gameplay skin settings are closed.");
    }
    if (hasControllerOperation()) {
      return rejected("Another gameplay skin operation is active.");
    }
    auto reservation = dependencies.operations.reservePreparedDisposal();
    if (!reservation) {
      return rejected("Skin staging disposal is currently unavailable.");
    }
    disposalReservation.emplace(std::move(*reservation));
    projected.preparedName.reset();
    projected.collisionPackage.reset();
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
                       .maxDepth = SkinPackagePolicy::maxPathComponents,
                       .maxPathBytes = SkinPackagePolicy::maxPathBytes})
                : platform_document_handoff::PlatformDocumentHandoffOperation{};
        phase = Phase::PickingFolder;
      }
    } catch (const std::exception &exception) {
      releaseDisposalReservation();
      setError(exception.what());
      return rejected(projected.statusMessage);
    } catch (...) {
      releaseDisposalReservation();
      setError("The document picker could not be started.");
      return rejected(projected.statusMessage);
    }
    if (!handoff) {
      releaseDisposalReservation();
      setError("The document picker is unavailable.");
      return rejected(projected.statusMessage);
    }
    setBusy(archive ? "Selecting a skin archive…" : "Selecting a skin folder…");
    return accepted("Skin source selection started.");
  }

  ControllerActionResult setSuggestedPackageName(std::string packageName) {
    if (closed || phase != Phase::NameReady || !projected.preparedName) {
      return rejected("No selected skin source is awaiting a package name.");
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
                           ? "Package name updated."
                           : projected.preparedName->validationError};
  }

  ControllerActionResult
  confirmPreparedImport(PackageCollisionPolicy requestedPolicy) {
    if (closed || phase != Phase::NameReady || !pickedSource ||
        !projected.preparedName || !disposalReservation) {
      return rejected("No selected skin source is ready to import.");
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
        return rejected("A package with this name is already installed.");
      }
    }

    collisionPolicy = requestedPolicy;
    SkinPackageOperationHandle handle;
    if (pickedSource->temporaryPathKind == PlatformTemporaryPathKind::File) {
      handle = dependencies.operations.submitPrepareArchive(
          pickedSource->localPath, *normalized.package,
          sourceCleanup(pickedSource));
    } else if (pickedSource->temporaryPathKind ==
               PlatformTemporaryPathKind::Directory) {
      handle = dependencies.operations.submitPrepareFolder(
          pickedSource->localPath, *normalized.package,
          sourceCleanup(pickedSource));
    } else {
      return rejected("The selected source has no supported path kind.");
    }
    if (handle.ticket == 0) {
      rejectPrepareSubmission(std::move(handle));
      return rejected(projected.statusMessage);
    }
    pickedSource.reset();
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    phase = Phase::PreparingPackage;
    projected.collisionPackage.reset();
    setBusy("Preparing skin package…");
    return accepted("Skin package preparation started.");
  }

  ControllerActionResult prepareActivation(SkinEntryId entry,
                                           SkinProfileSettings candidate,
                                           std::string message) {
    if (closed || hasControllerOperation()) {
      return rejected("Another gameplay skin operation is active.");
    }
    auto base = dependencies.profileOwner.snapshot(dependencies.profileId);
    candidate.sanitize();
    auto handle = dependencies.operations.submitPrepareActivation(
        std::move(base), std::move(entry), std::move(candidate));
    if (handle.ticket == 0) {
      return rejected("Skin activation preparation could not be queued.");
    }
    operationTicket = handle.ticket;
    progress = std::move(handle.progress);
    phase = Phase::PreparingActivation;
    setBusy(std::move(message));
    return accepted("Skin activation preparation started.");
  }

  ControllerActionResult submitProfileOnly(SkinProfileSettings candidate) {
    if (closed || hasControllerOperation()) {
      return rejected("Another gameplay skin operation is active.");
    }
    const auto base =
        dependencies.profileOwner.snapshot(dependencies.profileId);
    candidate.sanitize();
    auto submission = dependencies.commits.submitProfileSettings(
        dependencies.clientId, base, std::move(candidate));
    if (!submission.accepted) {
      return {.message = firstDiagnosticMessage(
                  submission.diagnostics,
                  "Gameplay skin settings could not be queued."),
              .diagnostics = std::move(submission.diagnostics)};
    }
    phase = Phase::WaitingProfileCommit;
    projected.state = GameplaySkinSettingsState::Busy;
    projected.statusMessage = "Saving gameplay skin settings…";
    projected.canCancel = false;
    return accepted("Gameplay skin settings save started.");
  }

  void cancelOperation() noexcept {
    if (closed) {
      return;
    }
    if (phase == Phase::WaitingActivationCommit ||
        phase == Phase::WaitingProfileCommit) {
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
      if (preparedPackage) {
        disposeLocalPrepared();
      } else {
        transferPickedCleanup();
      }
      projected.preparedName.reset();
      projected.collisionPackage.reset();
      progress.reset();
      phase = Phase::Idle;
      errorState = false;
      projected.statusMessage = "Operation cancelled.";
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
      if (preparedPackage) {
        disposeLocalPrepared();
      } else {
        transferPickedCleanup();
      }
      projected.preparedName.reset();
      projected.collisionPackage.reset();
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

void GameplaySkinSettingsController::poll() { impl_->poll(); }

void GameplaySkinSettingsController::profileChanged(
    SkinProfileId profileId, SkinActivationClientId clientId) {
  if (impl_->closed) {
    return;
  }
  impl_->cancelOperation();
  impl_->dependencies.commits.detachClient(impl_->dependencies.clientId);
  // Accepted coordinator transactions are durable after detachment, but their
  // delivery-only wait state belongs to the old profile binding.
  impl_->phase = Impl::Phase::Idle;
  impl_->errorState = false;
  impl_->progress.reset();
  impl_->projected.progress = {};
  impl_->projected.canCancel = false;
  impl_->dependencies.profileId = std::move(profileId);
  impl_->dependencies.clientId = clientId;
  impl_->projected.statusMessage.clear();
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
      !impl_->dependencies.requestRescan) {
    return rejected("A gameplay skin rescan is not currently available.");
  }
  impl_->dependencies.requestRescan();
  return accepted("Gameplay skin rescan requested.");
}

ControllerActionResult
GameplaySkinSettingsController::requestRevalidation(const SkinEntryId &entry) {
  if (impl_->closed || impl_->hasControllerOperation() ||
      !impl_->dependencies.requestRevalidation) {
    return rejected("Skin revalidation is not currently available.");
  }
  impl_->dependencies.requestRevalidation(entry);
  return accepted("Skin revalidation requested.");
}

ControllerActionResult
GameplaySkinSettingsController::select(const SkinEntryId &entry) {
  const auto catalogValue = impl_->catalog();
  const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
  const auto skinType = selectableGameplaySkinType(catalogEntry);
  if (!skinType) {
    return rejected("Only a validated gameplay skin can be selected.");
  }
  return selectGameplayTrait(*skinType, entry);
}

ControllerActionResult
GameplaySkinSettingsController::selectGameplayTrait(int skinType,
                                                    const SkinEntryId &entry) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  if (!gameplaySkinTraitForSkinType(skinType)) {
    return rejected("The requested gameplay skin trait is unavailable.");
  }
  const auto catalogValue = impl_->catalog();
  const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
  const auto entrySkinType = selectableGameplaySkinType(catalogEntry);
  if (!entrySkinType || *entrySkinType != skinType) {
    return rejected("The selected skin does not support this gameplay trait.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  candidate.selectedGameplayEntries.insert_or_assign(skinType, entry);
  candidate.entries.try_emplace(entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  "Validating selected skin…");
}

ControllerActionResult
GameplaySkinSettingsController::clearGameplayTrait(int skinType) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  if (!gameplaySkinTraitForSkinType(skinType)) {
    return rejected("The requested gameplay skin trait is unavailable.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  candidate.selectedGameplayEntries.erase(skinType);
  // An empty new-format map must not be repopulated from a legacy alias.
  candidate.selected7KeyEntry.reset();
  candidate.gameplayCompatibilityEnabled = false;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::setCompatibilityEnabled(bool enabled) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  if (enabled) {
    if (candidate.selectedGameplayEntries.empty()) {
      return rejected("Select a validated gameplay skin first.");
    }
    const auto catalogValue = impl_->catalog();
    for (const auto &[skinType, entry] : candidate.selectedGameplayEntries) {
      const auto *catalogEntry = impl_->findCatalogEntry(entry, catalogValue);
      const auto entrySkinType = selectableGameplaySkinType(catalogEntry);
      const auto settings = candidate.entries.find(entry);
      const EntryProfileSettings defaults;
      const auto &configured =
          settings == candidate.entries.end() ? defaults : settings->second;
      if (!entrySkinType || *entrySkinType != skinType ||
          !containsConfiguration(*catalogEntry, configured)) {
        return rejected("A selected skin configuration is not validated.");
      }
    }
  } else {
    candidate.selectedGameplayEntries.clear();
    candidate.selected7KeyEntry.reset();
  }
  candidate.gameplayCompatibilityEnabled = enabled;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::setOption(const SkinEntryId &entry,
                                          std::string name, int value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected("Only a validated gameplay skin can be configured.");
  }
  candidate.entries[entry].options[std::move(name)] = value;
  candidate.selectedGameplayEntries.insert_or_assign(*skinType, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  "Validating skin option…");
}

ControllerActionResult GameplaySkinSettingsController::setFileChoice(
    const SkinEntryId &entry, std::string name, std::string value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected("Only a validated gameplay skin can be configured.");
  }
  candidate.entries[entry].filePaths[std::move(name)] = std::move(value);
  candidate.selectedGameplayEntries.insert_or_assign(*skinType, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  "Validating skin file choice…");
}

ControllerActionResult GameplaySkinSettingsController::setOffset(
    const SkinEntryId &entry, std::string name, ConfigOffset value) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  const auto skinType = selectableGameplaySkinType(
      impl_->findCatalogEntry(entry, impl_->catalog()));
  if (!skinType) {
    return rejected("Only a validated gameplay skin can be configured.");
  }
  candidate.entries[entry].offsets[std::move(name)] = value;
  candidate.selectedGameplayEntries.insert_or_assign(*skinType, entry);
  return impl_->prepareActivation(entry, std::move(candidate),
                                  "Validating skin offset…");
}

ControllerActionResult
GameplaySkinSettingsController::setViewport(const SkinEntryId &entry,
                                            ViewportSettings viewport) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto candidate =
      impl_->dependencies.profileOwner.snapshot(impl_->dependencies.profileId)
          .settings;
  candidate.entries[entry].viewport = viewport;
  return impl_->submitProfileOnly(std::move(candidate));
}

ControllerActionResult
GameplaySkinSettingsController::requestRemoval(const SkinPackageId &package) {
  if (impl_->closed || impl_->hasControllerOperation()) {
    return rejected("Another gameplay skin operation is active.");
  }
  auto handle = impl_->dependencies.operations.submitRemove(package);
  if (handle.ticket == 0) {
    return rejected("Skin package removal could not be queued.");
  }
  impl_->operationTicket = handle.ticket;
  impl_->progress = std::move(handle.progress);
  impl_->phase = Impl::Phase::Removing;
  impl_->setBusy("Removing skin package…");
  return accepted("Skin package removal started.");
}

ControllerActionResult
GameplaySkinSettingsController::resetLayout(const SkinEntryId &entry) {
  return setViewport(entry, ViewportSettings{.mode = ViewportMode::Fit});
}

void GameplaySkinSettingsController::cancelOperation() noexcept {
  impl_->cancelOperation();
}

void GameplaySkinSettingsController::close() noexcept { impl_->close(); }

} // namespace skin
