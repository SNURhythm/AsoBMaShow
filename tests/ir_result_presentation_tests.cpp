#include "ir/IrResultPresentation.h"

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

std::shared_ptr<const ir::IrSubmission> submission() {
  auto value = std::make_shared<ir::IrSubmission>();
  value->attemptId = "123e4567-e89b-42d3-a456-426614174000";
  value->playedAtUnixMillis = 1'700'000'000'123LL;
  return value;
}

ir::IrResultPresentationInput baseInput() {
  return {
      .providerId = "tachi",
      .providerDisplayName = "Bokutachi",
      .capabilities = {.chartRankings = true,
                       .scoreSubmission = true,
                       .deferredSubmission = true},
      .settings = {.enabled = true,
                   .autoSubmit = false,
                   .serverOrigin = "https://boku.tachi.ac"},
      .saveOutcome = {.state = result_persistence::SaveState::Saved},
      .submission = submission(),
  };
}

void testNoRowIsNotSubmittedAndAllowsManualSubmit() {
  const auto presentation = ir::makeIrResultPresentation(baseInput());
  REQUIRE(presentation.visible);
  REQUIRE(presentation.state == ir::IrResultState::NotSubmitted);
  REQUIRE(presentation.statusText == "Not submitted");
  REQUIRE(presentation.showSubmit);
  REQUIRE(presentation.canSubmit);
  REQUIRE(!presentation.canRetry);
  REQUIRE(!presentation.blocksResultActions);
  REQUIRE(!presentation.persistenceDecisionRequired);
}

void testIneligibleResultKeepsDisabledSubmitAndDiagnostic() {
  auto input = baseInput();
  input.draftOutcome = ir::BuildDraftOutcome{
      .status = ir::BuildDraftStatus::Unsupported,
      .reason = ir::SubmissionEligibilityReason::RulesetMismatch,
      .diagnostic = "Beatoraja ruleset scores cannot be submitted.",
  };
  const auto presentation = ir::makeIrResultPresentation(std::move(input));
  REQUIRE(presentation.visible);
  REQUIRE(presentation.state == ir::IrResultState::NotSubmitted);
  REQUIRE(presentation.statusText == "Not eligible");
  REQUIRE(presentation.showSubmit);
  REQUIRE(!presentation.canSubmit);
  REQUIRE(presentation.detailText ==
          "Beatoraja ruleset scores cannot be submitted.");
}

void testEveryOutboxStateMapsWithoutBlockingLocalActions() {
  struct Expectation {
    ir::IrOutboxState outbox;
    ir::IrResultState result;
    const char *label;
    bool retry;
  };
  const Expectation expectations[] = {
      {ir::IrOutboxState::Pending, ir::IrResultState::Queued, "Queued", true},
      {ir::IrOutboxState::Uploading, ir::IrResultState::Submitting,
       "Submitting", false},
      {ir::IrOutboxState::AwaitingRemoteResult, ir::IrResultState::Waiting,
       "Waiting for Bokutachi", true},
      {ir::IrOutboxState::BlockedConfiguration,
       ir::IrResultState::AuthenticationRequired, "Authentication required",
       true},
      {ir::IrOutboxState::FailedPermanent, ir::IrResultState::Failed,
       "Submission failed", true},
      {ir::IrOutboxState::Succeeded, ir::IrResultState::Submitted, "Submitted",
       false},
  };

  for (const auto &expectation : expectations) {
    auto input = baseInput();
    input.snapshot = {.revision = 8,
                      .found = true,
                      .rowId = 42,
                      .state = expectation.outbox,
                      .diagnostic = "safe diagnostic"};
    const auto presentation = ir::makeIrResultPresentation(std::move(input));
    REQUIRE(presentation.state == expectation.result);
    REQUIRE(presentation.statusText == expectation.label);
    REQUIRE(!presentation.canSubmit);
    REQUIRE(!presentation.showSubmit);
    REQUIRE(presentation.canRetry == expectation.retry);
    REQUIRE(presentation.rowId == 42);
    REQUIRE(presentation.snapshotRevision == 8);
    REQUIRE(!presentation.blocksResultActions);
    REQUIRE(!presentation.persistenceDecisionRequired);
  }
}

void testActivePollHasDistinctPresentation() {
  auto input = baseInput();
  input.snapshot = {
      .revision = 9,
      .found = true,
      .rowId = 43,
      .state = ir::IrOutboxState::Uploading,
      .activeRequest = ir::IrActiveRequestKind::Poll,
  };
  const auto presentation = ir::makeIrResultPresentation(std::move(input));
  REQUIRE(presentation.state == ir::IrResultState::Polling);
  REQUIRE(presentation.statusText == "Polling Bokutachi");
  REQUIRE(presentation.detailText ==
          "Checking the queued import result with Bokutachi.");
}

void testUnsupportedProviderHasNoSubmissionAction() {
  auto input = baseInput();
  input.capabilities = {.readOnly = true, .chartRankings = true};
  const auto presentation = ir::makeIrResultPresentation(std::move(input));
  REQUIRE(presentation.visible);
  REQUIRE(presentation.state == ir::IrResultState::Unsupported);
  REQUIRE(presentation.statusText == "Submission unsupported");
  REQUIRE(!presentation.canSubmit);
  REQUIRE(!presentation.showSubmit);
  REQUIRE(!presentation.canRetry);
}

void testLegacyQueuedProofShowsBlockingReason() {
  auto input = baseInput();
  input.snapshot = {
      .found = true,
      .rowId = 42,
      .state = ir::IrOutboxState::BlockedConfiguration,
      .errorCode = "legacy_ruleset_proof_missing",
      .diagnostic =
          "Submission blocked because this queued score predates ruleset "
          "proof.",
  };
  const auto presentation = ir::makeIrResultPresentation(std::move(input));
  REQUIRE(presentation.statusText == "Submission blocked");
  REQUIRE(presentation.detailText ==
          "Submission blocked because this queued score predates ruleset "
          "proof.");
  REQUIRE(!presentation.canRetry);
}

void testLocalPersistenceMustBeSavedBeforeAnyIrControlAppears() {
  const result_persistence::SaveState blockedStates[] = {
      result_persistence::SaveState::InvalidAttempt,
      result_persistence::SaveState::Unstaged,
      result_persistence::SaveState::PendingScore,
      result_persistence::SaveState::PendingAcknowledgement,
      result_persistence::SaveState::UnstagedConflict,
      result_persistence::SaveState::PendingConflict,
  };
  for (const auto state : blockedStates) {
    auto input = baseInput();
    input.saveOutcome.state = state;
    input.snapshot = {.revision = 9,
                      .found = true,
                      .rowId = 42,
                      .state = ir::IrOutboxState::FailedPermanent};
    const auto presentation = ir::makeIrResultPresentation(std::move(input));
    REQUIRE(!presentation.visible);
    REQUIRE(presentation.state == ir::IrResultState::Hidden);
    REQUIRE(!presentation.canSubmit);
    REQUIRE(!presentation.canRetry);
  }

  auto missingSubmission = baseInput();
  missingSubmission.submission.reset();
  REQUIRE(!ir::makeIrResultPresentation(std::move(missingSubmission)).visible);

  auto disabled = baseInput();
  disabled.settings.enabled = false;
  REQUIRE(!ir::makeIrResultPresentation(std::move(disabled)).visible);
}

} // namespace

int main() {
  testNoRowIsNotSubmittedAndAllowsManualSubmit();
  testIneligibleResultKeepsDisabledSubmitAndDiagnostic();
  testEveryOutboxStateMapsWithoutBlockingLocalActions();
  testActivePollHasDistinctPresentation();
  testUnsupportedProviderHasNoSubmissionAction();
  testLegacyQueuedProofShowsBlockingReason();
  testLocalPersistenceMustBeSavedBeforeAnyIrControlAppears();
  return 0;
}
