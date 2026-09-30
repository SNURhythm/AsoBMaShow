#include "../i18n/Localization.h"
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
    result.statusText = i18n::tr("ir.result.submission_unsupported.label");
    result.detailText =
        i18n::format("ir.result.submission.read_only_notice",
                         {{"provider", result.providerDisplayName}});
    return result;
  }

  if (!input.snapshot.found) {
    result.state = IrResultState::NotSubmitted;
    result.showSubmit = true;
    const bool eligible = !input.draftOutcome.has_value() ||
                          input.draftOutcome->status == BuildDraftStatus::Built;
    result.canSubmit = eligible;
    result.statusText = eligible ? i18n::tr("ir.result.not_submitted.label") : i18n::tr("ir.result.not_eligible.label");
    result.detailText =
        eligible
            ? i18n::format("ir.result.submission.confirmation",
                         {{"provider", result.providerDisplayName}})
            : (input.draftOutcome->diagnostic.empty()
                   ? i18n::tr("ir.result.saved_result_not_eligible_submission.message")
                   : input.draftOutcome->diagnostic);
    return result;
  }

  result.rowId = input.snapshot.rowId;
  switch (input.snapshot.state) {
  case IrOutboxState::Pending:
    result.state = IrResultState::Queued;
    result.canRetry = true;
    result.statusText = i18n::tr("ir.result.queued.label");
    result.detailText = i18n::tr("ir.result.waiting_next_submission_attempt.message");
    break;
  case IrOutboxState::Uploading:
    if (input.snapshot.activeRequest == IrActiveRequestKind::Poll) {
      result.state = IrResultState::Polling;
      result.statusText = i18n::format("ir.result.polling.status",
                         {{"provider", result.providerDisplayName}});
      result.detailText = i18n::format("ir.result.polling.description",
                         {{"provider", result.providerDisplayName}});
    } else {
      result.state = IrResultState::Submitting;
      result.statusText = i18n::tr("ir.result.submitting.label");
      result.detailText =
          i18n::format("ir.result.submission.progress",
                         {{"provider", result.providerDisplayName}});
    }
    break;
  case IrOutboxState::AwaitingRemoteResult:
    result.state = IrResultState::Waiting;
    result.canRetry = true;
    result.statusText = i18n::format("ir.result.remote_wait.status",
                         {{"provider", result.providerDisplayName}});
    result.detailText = i18n::tr("ir.result.import_queued_remotely_being_polled.message");
    break;
  case IrOutboxState::BlockedConfiguration:
    result.state = IrResultState::AuthenticationRequired;
    if (input.snapshot.errorCode == "legacy_ruleset_proof_missing") {
      result.canRetry = false;
      result.statusText = i18n::tr("ir.result.submission_blocked.label");
      result.detailText = input.snapshot.diagnostic.empty()
                              ? i18n::tr("ir.result.queued_score_has_no_valid_ruleset_proof.message")
                              : input.snapshot.diagnostic;
    } else {
      result.canRetry = true;
      result.statusText = i18n::tr("ir.result.authentication_required.label");
      result.detailText =
          i18n::tr("ir.result.add_replace_api_key_in_settings_ir_then_retry.message");
    }
    break;
  case IrOutboxState::FailedPermanent:
    result.state = IrResultState::Failed;
    result.canRetry = true;
    result.statusText = i18n::tr("ir.result.submission_failed.label");
    result.detailText = input.snapshot.diagnostic.empty()
                            ? i18n::tr("ir.result.score_not_accepted_can_retry.message")
                            : input.snapshot.diagnostic;
    break;
  case IrOutboxState::Succeeded:
    result.state = IrResultState::Submitted;
    result.statusText = i18n::tr("ir.result.submitted.label");
    result.detailText =
        i18n::format("ir.result.submission.success",
                         {{"provider", result.providerDisplayName}});
    break;
  }
  return result;
}

} // namespace ir
