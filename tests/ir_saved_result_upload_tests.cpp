#include "../src/ir/IrSavedResultUpload.h"

#include <cassert>
#include <string>

namespace {

constexpr const char *kProviderId = "bokutachi";
constexpr const char *kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

ir::IrSubmission submission() {
  ir::IrSubmission value;
  value.attemptId = kAttemptId;
  value.chartMd5 = "0123456789abcdef0123456789abcdef";
  value.chartSha256 =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
  return value;
}

ir::IrOutboxEntry entry(ir::IrOutboxState state) {
  ir::IrOutboxEntry value;
  value.id = 19;
  value.providerId = kProviderId;
  value.attemptId = kAttemptId;
  value.state = state;
  return value;
}

ir::IrOutboxDraft draft() {
  ir::IrOutboxDraft value;
  value.providerId = kProviderId;
  value.attemptId = kAttemptId;
  return value;
}

void testNewAttemptBuildsAndQueuesOneDraft() {
  int buildCalls = 0;
  int enqueueCalls = 0;
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view, std::string_view) {
    return ir::IrOutboxReadOutcome{.status =
                                       ir::IrOutboxReadStatus::NotFound};
  };
  dependencies.buildDraft = [&](const ir::IrSubmission &) {
    ++buildCalls;
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draft()};
  };
  dependencies.enqueue = [&](const ir::IrOutboxDraft &) {
    ++enqueueCalls;
    return ir::IrOutboxInsertOutcome{.status =
                                         ir::IrOutboxInsertStatus::Inserted};
  };
  dependencies.retry = [](std::int64_t) {
    assert(false);
    return ir::IrOutboxMutationOutcome{};
  };

  const auto outcome = ir::executeIrSavedResultUpload(
      kProviderId, submission(), dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::Queued);
  assert(outcome.accepted);
  assert(buildCalls == 1);
  assert(enqueueCalls == 1);
}

void testExistingRetryableRowsAreReused() {
  for (const auto state : {ir::IrOutboxState::Pending,
                           ir::IrOutboxState::AwaitingRemoteResult,
                           ir::IrOutboxState::BlockedConfiguration,
                           ir::IrOutboxState::FailedPermanent}) {
    int retryCalls = 0;
    ir::IrSavedResultUploadDependencies dependencies;
    dependencies.loadOutbox = [state](std::string_view, std::string_view) {
      return ir::IrOutboxReadOutcome{.status = ir::IrOutboxReadStatus::Found,
                                     .entry = entry(state)};
    };
    dependencies.buildDraft = [](const ir::IrSubmission &) {
      assert(false);
      return ir::BuildDraftOutcome{};
    };
    dependencies.enqueue = [](const ir::IrOutboxDraft &) {
      assert(false);
      return ir::IrOutboxInsertOutcome{};
    };
    dependencies.retry = [&](std::int64_t rowId) {
      ++retryCalls;
      assert(rowId == 19);
      return ir::IrOutboxMutationOutcome{
          .status = ir::IrOutboxMutationStatus::Updated, .affectedRows = 1};
    };

    const auto outcome = ir::executeIrSavedResultUpload(
        kProviderId, submission(), dependencies);
    assert(outcome.state == ir::IrSavedResultUploadState::RetryQueued);
    assert(outcome.accepted);
    assert(retryCalls == 1);
  }
}

void testExistingFailureRetriesWithoutSubmissionSnapshot() {
  int retryCalls = 0;
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view providerId,
                               std::string_view attemptId) {
    assert(providerId == kProviderId);
    assert(attemptId == kAttemptId);
    return ir::IrOutboxReadOutcome{
        .status = ir::IrOutboxReadStatus::Found,
        .entry = entry(ir::IrOutboxState::FailedPermanent)};
  };
  dependencies.buildDraft = [](const ir::IrSubmission &) {
    assert(false);
    return ir::BuildDraftOutcome{};
  };
  dependencies.enqueue = [](const ir::IrOutboxDraft &) {
    assert(false);
    return ir::IrOutboxInsertOutcome{};
  };
  dependencies.retry = [&](std::int64_t rowId) {
    ++retryCalls;
    assert(rowId == 19);
    return ir::IrOutboxMutationOutcome{
        .status = ir::IrOutboxMutationStatus::Updated, .affectedRows = 1};
  };

  const auto outcome = ir::executeIrSavedResultUpload(kProviderId, kAttemptId,
                                                      nullptr, dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::RetryQueued);
  assert(outcome.accepted);
  assert(retryCalls == 1);
}

void testFreshUploadWithoutSubmissionSnapshotFailsClosed() {
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view, std::string_view) {
    return ir::IrOutboxReadOutcome{.status = ir::IrOutboxReadStatus::NotFound};
  };
  dependencies.buildDraft = [](const ir::IrSubmission &) {
    assert(false);
    return ir::BuildDraftOutcome{};
  };
  dependencies.enqueue = [](const ir::IrOutboxDraft &) {
    assert(false);
    return ir::IrOutboxInsertOutcome{};
  };
  dependencies.retry = [](std::int64_t) {
    assert(false);
    return ir::IrOutboxMutationOutcome{};
  };

  const auto outcome = ir::executeIrSavedResultUpload(kProviderId, kAttemptId,
                                                      nullptr, dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::Failed);
  assert(!outcome.accepted);
  assert(outcome.message ==
         "This saved result has no independently stored IR snapshot.");
}

void testSnapshotAttemptIdentityMustMatchRequestedAttempt() {
  int outboxReads = 0;
  int buildCalls = 0;
  int enqueueCalls = 0;
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [&](std::string_view, std::string_view) {
    ++outboxReads;
    return ir::IrOutboxReadOutcome{.status = ir::IrOutboxReadStatus::NotFound};
  };
  dependencies.buildDraft = [&](const ir::IrSubmission &) {
    ++buildCalls;
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draft()};
  };
  dependencies.enqueue = [&](const ir::IrOutboxDraft &) {
    ++enqueueCalls;
    return ir::IrOutboxInsertOutcome{.status =
                                         ir::IrOutboxInsertStatus::Inserted};
  };

  auto mismatched = submission();
  mismatched.attemptId = "123e4567-e89b-42d3-a456-426614174099";
  const auto outcome = ir::executeIrSavedResultUpload(
      kProviderId, kAttemptId, &mismatched, dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::Failed);
  assert(!outcome.accepted);
  assert(outboxReads == 0);
  assert(buildCalls == 0);
  assert(enqueueCalls == 0);
}

void testActiveAndSubmittedRowsDoNotMutate() {
  for (const auto state : {ir::IrOutboxState::Uploading,
                           ir::IrOutboxState::Succeeded}) {
    ir::IrSavedResultUploadDependencies dependencies;
    dependencies.loadOutbox = [state](std::string_view, std::string_view) {
      return ir::IrOutboxReadOutcome{.status = ir::IrOutboxReadStatus::Found,
                                     .entry = entry(state)};
    };
    dependencies.buildDraft = [](const ir::IrSubmission &) {
      assert(false);
      return ir::BuildDraftOutcome{};
    };
    dependencies.enqueue = [](const ir::IrOutboxDraft &) {
      assert(false);
      return ir::IrOutboxInsertOutcome{};
    };
    dependencies.retry = [](std::int64_t) {
      assert(false);
      return ir::IrOutboxMutationOutcome{};
    };

    const auto outcome = ir::executeIrSavedResultUpload(
        kProviderId, submission(), dependencies);
    assert(outcome.state ==
           (state == ir::IrOutboxState::Uploading
                ? ir::IrSavedResultUploadState::AlreadyActive
                : ir::IrSavedResultUploadState::AlreadySubmitted));
    assert(!outcome.accepted);
  }
}

void testConcurrentInsertIsTreatedAsQueued() {
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view, std::string_view) {
    return ir::IrOutboxReadOutcome{.status =
                                       ir::IrOutboxReadStatus::NotFound};
  };
  dependencies.buildDraft = [](const ir::IrSubmission &) {
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draft()};
  };
  dependencies.enqueue = [](const ir::IrOutboxDraft &) {
    return ir::IrOutboxInsertOutcome{
        .status = ir::IrOutboxInsertStatus::AlreadyExists};
  };
  dependencies.retry = [](std::int64_t) {
    return ir::IrOutboxMutationOutcome{};
  };

  const auto outcome = ir::executeIrSavedResultUpload(
      kProviderId, submission(), dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::Queued);
  assert(outcome.accepted);
}

void testReceiptOnlyEnqueueIsReportedAsAlreadySubmitted() {
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view, std::string_view) {
    return ir::IrOutboxReadOutcome{.status =
                                       ir::IrOutboxReadStatus::NotFound};
  };
  dependencies.buildDraft = [](const ir::IrSubmission &) {
    return ir::BuildDraftOutcome{.status = ir::BuildDraftStatus::Built,
                                 .draft = draft()};
  };
  dependencies.enqueue = [](const ir::IrOutboxDraft &) {
    return ir::IrOutboxInsertOutcome{
        .status = ir::IrOutboxInsertStatus::AlreadySubmitted};
  };

  const auto outcome = ir::executeIrSavedResultUpload(
      kProviderId, submission(), dependencies);

  assert(outcome.state == ir::IrSavedResultUploadState::AlreadySubmitted);
  assert(!outcome.accepted);
  assert(outcome.message == "This score has already been submitted.");
}

void testFailuresAreSanitized() {
  ir::IrSavedResultUploadDependencies dependencies;
  dependencies.loadOutbox = [](std::string_view, std::string_view) {
    return ir::IrOutboxReadOutcome{
        .status = ir::IrOutboxReadStatus::StorageFailure,
        .diagnostic = std::string(800, 'x') + "\nsecret"};
  };

  const auto outcome = ir::executeIrSavedResultUpload(
      kProviderId, submission(), dependencies);
  assert(outcome.state == ir::IrSavedResultUploadState::Failed);
  assert(!outcome.accepted);
  assert(outcome.message.size() <= ir::kMaximumDiagnosticBytes);
  assert(outcome.message.find('\n') == std::string::npos);
}

} // namespace

int main() {
  testNewAttemptBuildsAndQueuesOneDraft();
  testExistingRetryableRowsAreReused();
  testExistingFailureRetriesWithoutSubmissionSnapshot();
  testFreshUploadWithoutSubmissionSnapshotFailsClosed();
  testSnapshotAttemptIdentityMustMatchRequestedAttempt();
  testActiveAndSubmittedRowsDoNotMutate();
  testConcurrentInsertIsTreatedAsQueued();
  testReceiptOnlyEnqueueIsReportedAsAlreadySubmitted();
  testFailuresAreSanitized();
  return 0;
}
