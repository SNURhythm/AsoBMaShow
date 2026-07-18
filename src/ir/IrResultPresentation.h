#pragma once

#include "IrDriver.h"
#include "IrProfileSettings.h"
#include "IrSubmission.h"
#include "IrSubmissionService.h"
#include "../ResultPersistenceCoordinator.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ir {

enum class IrResultState {
  Hidden,
  NotSubmitted,
  Queued,
  Submitting,
  Waiting,
  Submitted,
  AuthenticationRequired,
  Failed,
  Unsupported,
};

struct IrResultPresentationInput {
  std::string providerId;
  std::string providerDisplayName;
  IrDriverCapabilities capabilities;
  IrProviderSettings settings;
  result_persistence::SaveOutcome saveOutcome;
  std::shared_ptr<const IrSubmission> submission;
  IrAttemptStatusSnapshot snapshot;
};

struct IrResultPresentation {
  std::string providerId;
  std::string providerDisplayName;
  IrResultState state = IrResultState::Hidden;
  bool visible = false;
  bool canSubmit = false;
  bool canRetry = false;
  bool blocksResultActions = false;
  bool persistenceDecisionRequired = false;
  std::int64_t rowId = 0;
  std::uint64_t snapshotRevision = 0;
  std::string statusText;
  std::string detailText;
};

[[nodiscard]] IrResultPresentation
makeIrResultPresentation(IrResultPresentationInput input);

} // namespace ir
