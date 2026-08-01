#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace ir {

enum class IrOutboxState : int;

enum class IrRecordState {
  Hidden,
  Eligible,
  Queued,
  Uploading,
  AwaitingRemote,
  Blocked,
  Failed,
  Uploaded,
};

enum class IrRecordActivity {
  None,
  Submitting,
  Polling,
};

struct IrRecordStateInput {
  bool eligible = false;
  bool hasReceipt = false;
  std::optional<IrOutboxState> outboxState;
  IrRecordActivity activity = IrRecordActivity::None;
};

[[nodiscard]] IrRecordState
resolveIrRecordState(IrRecordStateInput input) noexcept;

enum class IrReceiptConfirmationSource : int {
  Submission = 0,
  Snapshot = 1,
};

struct IrSuccessfulReceiptDraft {
  std::string serverOrigin;
  std::optional<std::int64_t> remoteUserId;
  std::string remoteChartId;
  std::string remoteScoreId;
  IrReceiptConfirmationSource source =
      IrReceiptConfirmationSource::Submission;
  bool observedInSnapshot = false;
  std::int64_t confirmedAtUnixMillis = 0;
};

struct IrSubmissionReceipt {
  std::int64_t id = 0;
  std::string providerId;
  std::string serverOrigin;
  int replayId = 0;
  int modernChartResultId = 0;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::optional<std::int64_t> remoteUserId;
  std::string remoteChartId;
  std::string remoteScoreId;
  IrReceiptConfirmationSource source =
      IrReceiptConfirmationSource::Submission;
  bool observedInSnapshot = false;
  std::int64_t confirmedAtUnixMillis = 0;
};

enum class IrReceiptReadStatus { Found, NotFound, Invalid, StorageFailure };

struct IrReceiptReadOutcome {
  IrReceiptReadStatus status = IrReceiptReadStatus::StorageFailure;
  std::optional<IrSubmissionReceipt> receipt;
  std::string diagnostic;
};

[[nodiscard]] bool validateIrSuccessfulReceiptDraft(
    const IrSuccessfulReceiptDraft &draft, std::string &diagnostic) noexcept;

[[nodiscard]] bool
validateIrSubmissionReceipt(const IrSubmissionReceipt &receipt,
                            std::string &diagnostic) noexcept;

} // namespace ir
