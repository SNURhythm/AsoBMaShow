#pragma once

#include "../ResultRecordSummary.h"
#include "ResultScene.h"

#include <functional>
#include <optional>
#include <string>

struct ResultRecordRecallActionState {
  bool visible = false;
  bool enabled = false;

  bool operator==(const ResultRecordRecallActionState &) const = default;
};

[[nodiscard]] ResultRecordRecallActionState resultRecordRecallActionState(
    const std::optional<ResultRecordSummary> &selection,
    bool resultActionAllowedInCurrentMode,
    bool modalOperationInProgress) noexcept;

struct RemoteResultRecallRequest {
  IrRemoteRecordId identity;
  std::string selectedStableKey;
};

enum class RemoteResultRecallStatus {
  Transitioned,
  StaleSelection,
  NotFound,
  Invalid,
  StorageFailure,
  TransitionFailed,
};

struct RemoteResultRecallCallbacks {
  std::function<bool(const RemoteResultRecallRequest &)> selectionStillMatches;
  std::function<ir::IrRemoteScoreLookupOutcome(const IrRemoteRecordId &)>
      loadExact;
  std::function<bool(ResultRemoteOptions, bool retainCurrentScene)> transition;
  std::function<void(std::string)> failAndReload;
};

[[nodiscard]] bool remoteResultRecallSelectionMatches(
    const std::optional<ResultRecordSummary> &selection,
    const RemoteResultRecallRequest &request) noexcept;

[[nodiscard]] bool remoteResultRecallSelectionMatches(
    const ResultRecordSummary &selection,
    const RemoteResultRecallRequest &request) noexcept;

// Executes the exact origin-scoped lookup and retained transition used by the
// Records modal. All failure outcomes route through failAndReload so a live
// Records modal can refresh in place.
[[nodiscard]] RemoteResultRecallStatus
executeRemoteResultRecall(const RemoteResultRecallRequest &request,
                          RemoteResultRecallCallbacks &callbacks) noexcept;

// Runs the ResultScene Back transition to the retained MainMenu instance.
// Returning false means the navigation callback was absent or failed.
[[nodiscard]] bool executeRemoteResultBack(
    const std::function<void()> &returnToRetainedRecords) noexcept;
