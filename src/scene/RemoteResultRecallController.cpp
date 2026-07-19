#include "RemoteResultRecallController.h"

#include <utility>

namespace {

RemoteResultRecallStatus fail(RemoteResultRecallCallbacks &callbacks,
                              RemoteResultRecallStatus status,
                              std::string diagnostic) noexcept {
  try {
    if (callbacks.failAndReload) {
      callbacks.failAndReload(std::move(diagnostic));
    }
  } catch (...) {
  }
  return status;
}

bool validRequestIdentity(const RemoteResultRecallRequest &request) noexcept {
  try {
    const auto normalizedOrigin =
        ir::normalizeServerOrigin(request.identity.serverOrigin);
    return request.identity.providerId == ir::kTachiProviderId &&
           normalizedOrigin.has_value() &&
           *normalizedOrigin == request.identity.serverOrigin &&
           !request.identity.remoteScoreId.empty() &&
           !request.selectedStableKey.empty();
  } catch (...) {
    return false;
  }
}

} // namespace

ResultRecordRecallActionState resultRecordRecallActionState(
    const std::optional<ResultRecordSummary> &selection,
    bool resultActionAllowedInCurrentMode,
    bool modalOperationInProgress) noexcept {
  const bool visible = selection.has_value() &&
                       selection->capabilities.resultRecall &&
                       resultActionAllowedInCurrentMode;
  return {.visible = visible, .enabled = visible && !modalOperationInProgress};
}

bool remoteResultRecallSelectionMatches(
    const std::optional<ResultRecordSummary> &selection,
    const RemoteResultRecallRequest &request) noexcept {
  return selection.has_value() &&
         remoteResultRecallSelectionMatches(*selection, request);
}

bool remoteResultRecallSelectionMatches(
    const ResultRecordSummary &selection,
    const RemoteResultRecallRequest &request) noexcept {
  try {
    const auto *identity = std::get_if<IrRemoteRecordId>(&selection.identity);
    return identity != nullptr && *identity == request.identity &&
           selection.capabilities.resultRecall &&
           selection.remote.has_value() &&
           selection.remote->remoteScoreId == request.identity.remoteScoreId &&
           selection.stableKey() == request.selectedStableKey;
  } catch (...) {
    return false;
  }
}

RemoteResultRecallStatus
executeRemoteResultRecall(const RemoteResultRecallRequest &request,
                          RemoteResultRecallCallbacks &callbacks) noexcept {
  if (!validRequestIdentity(request) || !callbacks.selectionStillMatches ||
      !callbacks.loadExact || !callbacks.transition) {
    return fail(callbacks, RemoteResultRecallStatus::Invalid,
                "synchronized result identity is invalid");
  }

  try {
    if (!callbacks.selectionStillMatches(request)) {
      return fail(callbacks, RemoteResultRecallStatus::StaleSelection,
                  "selected synchronized result changed");
    }

    ir::IrRemoteScoreLookupOutcome loaded =
        callbacks.loadExact(request.identity);
    using LookupStatus = ir::IrRemoteScoreLookupOutcome::Status;
    if (loaded.status != LookupStatus::Loaded || !loaded.score.has_value()) {
      switch (loaded.status) {
      case LookupStatus::NotFound:
        return fail(callbacks, RemoteResultRecallStatus::NotFound,
                    "synchronized result is no longer available");
      case LookupStatus::Invalid:
        return fail(callbacks, RemoteResultRecallStatus::Invalid,
                    "synchronized result could not be verified");
      case LookupStatus::StorageFailure:
      case LookupStatus::Loaded:
        return fail(callbacks, RemoteResultRecallStatus::StorageFailure,
                    "synchronized result storage is unavailable");
      }
    }

    std::string diagnostic;
    if (!ir::validateIrRemoteScore(*loaded.score, diagnostic) ||
        loaded.score->remoteScoreId != request.identity.remoteScoreId) {
      return fail(callbacks, RemoteResultRecallStatus::Invalid,
                  "synchronized result identity changed");
    }
    if (!callbacks.selectionStillMatches(request)) {
      return fail(callbacks, RemoteResultRecallStatus::StaleSelection,
                  "selected synchronized result changed");
    }

    ResultRemoteOptions remote{
        .score = std::move(*loaded.score),
        .providerId = request.identity.providerId,
        .serverOrigin = request.identity.serverOrigin,
    };
    remote.rankingQuery = makeRemoteResultRankingQuery(remote.score);
    if (!callbacks.transition(std::move(remote), true)) {
      return fail(callbacks, RemoteResultRecallStatus::TransitionFailed,
                  "synchronized result could not be opened");
    }
    return RemoteResultRecallStatus::Transitioned;
  } catch (...) {
    return fail(callbacks, RemoteResultRecallStatus::TransitionFailed,
                "synchronized result could not be opened");
  }
}

bool executeRemoteResultBack(
    const std::function<void()> &returnToRetainedRecords) noexcept {
  if (!returnToRetainedRecords) {
    return false;
  }
  try {
    returnToRetainedRecords();
    return true;
  } catch (...) {
    return false;
  }
}
