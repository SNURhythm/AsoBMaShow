#include "IrSavedResultUpload.h"

#include <exception>
#include <utility>

namespace ir {
namespace {

IrSavedResultUploadResult failure(std::string_view diagnostic,
                                  std::string_view fallback) {
  std::string message = sanitizeDiagnostic(diagnostic);
  if (message.empty()) {
    message = std::string(fallback);
  }
  return {.state = IrSavedResultUploadState::Failed,
          .accepted = false,
          .message = std::move(message)};
}

IrSavedResultUploadResult retryExisting(
    const IrOutboxEntry &entry,
    const IrSavedResultUploadDependencies &dependencies) {
  if (!dependencies.retry) {
    return failure({}, "IR retry is unavailable.");
  }
  const auto retried = dependencies.retry(entry.id);
  if (retried.status != IrOutboxMutationStatus::Updated) {
    return failure(retried.diagnostic, "IR retry could not be queued.");
  }
  return {.state = IrSavedResultUploadState::RetryQueued,
          .accepted = true,
          .message = "IR retry queued."};
}

} // namespace

IrSavedResultUploadResult executeIrSavedResultUpload(
    std::string_view providerId, const IrSubmission &submission,
    const IrSavedResultUploadDependencies &dependencies) noexcept {
  try {
    if (!dependencies.loadOutbox) {
      return failure({}, "IR outbox is unavailable.");
    }
    const auto loaded =
        dependencies.loadOutbox(providerId, submission.attemptId);
    if (loaded.status == IrOutboxReadStatus::Found) {
      if (!loaded.entry.has_value()) {
        return failure({}, "IR outbox entry is invalid.");
      }
      switch (loaded.entry->state) {
      case IrOutboxState::Uploading:
        return {.state = IrSavedResultUploadState::AlreadyActive,
                .accepted = false,
                .message = "IR upload is already in progress."};
      case IrOutboxState::Succeeded:
        return {.state = IrSavedResultUploadState::AlreadySubmitted,
                .accepted = false,
                .message = "This score has already been submitted."};
      case IrOutboxState::Pending:
      case IrOutboxState::AwaitingRemoteResult:
      case IrOutboxState::BlockedConfiguration:
      case IrOutboxState::FailedPermanent:
        return retryExisting(*loaded.entry, dependencies);
      }
    }
    if (loaded.status != IrOutboxReadStatus::NotFound) {
      return failure(loaded.diagnostic, "IR outbox could not be read.");
    }

    if (!dependencies.buildDraft || !dependencies.enqueue) {
      return failure({}, "IR upload is unavailable.");
    }
    auto built = dependencies.buildDraft(submission);
    if (built.status != BuildDraftStatus::Built || !built.draft.has_value()) {
      return failure(built.diagnostic,
                     "This score is not eligible for IR upload.");
    }
    auto inserted = dependencies.enqueue(*built.draft);
    if (inserted.status == IrOutboxInsertStatus::AlreadySubmitted) {
      return {.state = IrSavedResultUploadState::AlreadySubmitted,
              .accepted = false,
              .message = "This score has already been submitted."};
    }
    if (inserted.status != IrOutboxInsertStatus::Inserted &&
        inserted.status != IrOutboxInsertStatus::AlreadyExists) {
      return failure(inserted.diagnostic, "IR upload could not be queued.");
    }
    return {.state = IrSavedResultUploadState::Queued,
            .accepted = true,
            .message = "IR upload queued."};
  } catch (const std::exception &) {
    return failure({}, "IR upload could not be queued.");
  } catch (...) {
    return failure({}, "IR upload could not be queued.");
  }
}

} // namespace ir
