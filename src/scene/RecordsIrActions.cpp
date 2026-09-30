#include "../i18n/Localization.h"
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
    return i18n::tr("records.ir.ir_upload_queued.message");
  case ir::IrRecordState::Uploading:
    return i18n::tr("records.ir.ir_upload_in_progress.message");
  case ir::IrRecordState::AwaitingRemote:
    return i18n::tr("records.ir.ir_awaiting_remote_result.message");
  case ir::IrRecordState::Blocked:
    return i18n::tr("records.ir.ir_upload_blocked_check_settings_ir.message");
  case ir::IrRecordState::Uploaded:
    return i18n::tr("records.ir.ir_upload_complete.message");
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
    return i18n::tr("records.ir.enable_bokutachi_in_settings_ir_before_uploading.message");
  }
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  if (driver == nullptr) {
    return i18n::tr("records.ir.bokutachi_ir_unavailable.message");
  }
  const auto capabilities = driver->capabilities();
  if (capabilities.readOnly || !capabilities.scoreSubmission ||
      context.irSubmissionService == nullptr) {
    return i18n::tr("records.ir.bokutachi_score_submission_unavailable.message");
  }
  return std::nullopt;
}

std::string uploadSavedResult(ApplicationContext &context,
                              std::string_view attemptId) {
  const auto snapshot =
      context.replayRepository.LoadModernIrSubmissionSnapshot(attemptId);
  if (snapshot.status != ModernIrSnapshotReadStatus::Loaded ||
      !snapshot.snapshot.has_value()) {
    return i18n::tr("records.ir.saved_result_has_no_verified_ir_snapshot.message");
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
