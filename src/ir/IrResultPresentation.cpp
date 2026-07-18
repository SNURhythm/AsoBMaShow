#include "IrResultPresentation.h"

#include <utility>

namespace ir {

IrResultPresentation makeIrResultPresentation(IrResultPresentationInput input) {
  IrResultPresentation result{
      .providerId = std::move(input.providerId),
      .providerDisplayName = std::move(input.providerDisplayName),
      .snapshotRevision = input.snapshot.revision,
  };
  if (!input.saveOutcome.saved() || !input.submission ||
      !input.settings.enabled) {
    return result;
  }

  result.visible = true;
  if (input.capabilities.readOnly || !input.capabilities.scoreSubmission) {
    result.state = IrResultState::Unsupported;
    result.statusText = "Submission unsupported";
    result.detailText =
        result.providerDisplayName + " is configured for score reading only.";
    return result;
  }

  if (!input.snapshot.found) {
    result.state = IrResultState::NotSubmitted;
    result.showSubmit = true;
    const bool eligible = !input.draftOutcome.has_value() ||
                          input.draftOutcome->status == BuildDraftStatus::Built;
    result.canSubmit = eligible;
    result.statusText = eligible ? "Not submitted" : "Not eligible";
    result.detailText =
        eligible
            ? "Submit this saved result to " + result.providerDisplayName +
                  "."
            : (input.draftOutcome->diagnostic.empty()
                   ? "This saved result is not eligible for submission."
                   : input.draftOutcome->diagnostic);
    return result;
  }

  result.rowId = input.snapshot.rowId;
  switch (input.snapshot.state) {
  case IrOutboxState::Pending:
    result.state = IrResultState::Queued;
    result.canRetry = true;
    result.statusText = "Queued";
    result.detailText = "Waiting for the next submission attempt.";
    break;
  case IrOutboxState::Uploading:
    result.state = IrResultState::Submitting;
    result.statusText = "Submitting";
    result.detailText =
        "Sending this score to " + result.providerDisplayName + ".";
    break;
  case IrOutboxState::AwaitingRemoteResult:
    result.state = IrResultState::Waiting;
    result.canRetry = true;
    result.statusText = "Waiting for " + result.providerDisplayName;
    result.detailText = "The import was queued remotely and is being polled.";
    break;
  case IrOutboxState::BlockedConfiguration:
    result.state = IrResultState::AuthenticationRequired;
    if (input.snapshot.errorCode == "legacy_ruleset_proof_missing") {
      result.canRetry = false;
      result.statusText = "Submission blocked";
      result.detailText = input.snapshot.diagnostic.empty()
                              ? "The queued score has no valid ruleset proof."
                              : input.snapshot.diagnostic;
    } else {
      result.canRetry = true;
      result.statusText = "Authentication required";
      result.detailText =
          "Add or replace the API key in Settings > IR, then retry.";
    }
    break;
  case IrOutboxState::FailedPermanent:
    result.state = IrResultState::Failed;
    result.canRetry = true;
    result.statusText = "Submission failed";
    result.detailText = input.snapshot.diagnostic.empty()
                            ? "The score was not accepted. You can retry it."
                            : input.snapshot.diagnostic;
    break;
  case IrOutboxState::Succeeded:
    result.state = IrResultState::Submitted;
    result.statusText = "Submitted";
    result.detailText =
        "This score was accepted by " + result.providerDisplayName + ".";
    break;
  }
  return result;
}

} // namespace ir
