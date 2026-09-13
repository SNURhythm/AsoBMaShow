#include "RecordsIrActions.h"

#include "../context.h"
#include "../ir/IrSavedResultUpload.h"

namespace replay_records {

ir::IrRecordActivity recordActivity(ir::IrActiveRequestKind request) noexcept {
  switch (request) {
  case ir::IrActiveRequestKind::None:
    return ir::IrRecordActivity::None;
  case ir::IrActiveRequestKind::Submit:
    return ir::IrRecordActivity::Submitting;
  case ir::IrActiveRequestKind::Poll:
    return ir::IrRecordActivity::Polling;
  }
  return ir::IrRecordActivity::None;
}

std::string_view irStatusFeedback(ir::IrRecordState state) noexcept {
  switch (state) {
  case ir::IrRecordState::Queued:
    return "IR upload is queued.";
  case ir::IrRecordState::Uploading:
    return "IR upload is in progress.";
  case ir::IrRecordState::AwaitingRemote:
    return "IR is awaiting the remote result.";
  case ir::IrRecordState::Blocked:
    return "IR upload is blocked. Check Settings > IR.";
  case ir::IrRecordState::Uploaded:
    return "IR upload is complete.";
  case ir::IrRecordState::Hidden:
  case ir::IrRecordState::Eligible:
  case ir::IrRecordState::Failed:
    return {};
  }
  return {};
}

std::optional<std::string>
irUploadUnavailable(const ApplicationContext &context) {
  const auto provider = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (provider == context.settings.irProviders.end() || !provider->second.enabled) {
    return "Enable Bokutachi in Settings > IR before uploading.";
  }
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  if (driver == nullptr) {
    return "Bokutachi IR is unavailable.";
  }
  const auto capabilities = driver->capabilities();
  if (capabilities.readOnly || !capabilities.scoreSubmission ||
      context.irSubmissionService == nullptr) {
    return "Bokutachi score submission is unavailable.";
  }
  return std::nullopt;
}

std::string uploadSavedResult(ApplicationContext &context,
                              std::string_view attemptId) {
  const auto snapshot =
      context.replayRepository.LoadModernIrSubmissionSnapshot(attemptId);
  if (snapshot.status != ModernIrSnapshotReadStatus::Loaded ||
      !snapshot.snapshot.has_value()) {
    return "This saved result has no verified IR snapshot.";
  }
  const ir::IrSavedResultUploadDependencies dependencies{
      .loadOutbox = [&context](std::string_view provider, std::string_view attempt) {
        return context.replayRepository.LoadIrOutbox(provider, attempt);
      },
      .buildDraft = [&context](const ir::IrSubmission &submission) {
        return context.irDrivers.buildDraft(ir::kTachiProviderId, submission);
      },
      .enqueue = [&context](const ir::IrOutboxDraft &draft) {
        return context.irSubmissionService->enqueueManual(draft);
      },
      .retry = [&context](std::int64_t rowId) {
        return context.irSubmissionService->retry(rowId);
      },
  };
  return ir::executeIrSavedResultUpload(
             ir::kTachiProviderId, snapshot.snapshot->submission, dependencies)
      .message;
}

} // namespace replay_records
