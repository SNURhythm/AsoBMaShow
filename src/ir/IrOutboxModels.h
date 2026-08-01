#pragma once

#include "IrProfileSettings.h"
#include "IrReceiptModels.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

inline constexpr std::size_t kMaximumDiagnosticBytes = 512;
inline constexpr std::size_t kMaximumIrPayloadBytes = 64 * 1024;
inline constexpr std::size_t kMaximumIrRemoteValueBytes = 2 * 1024;
inline constexpr std::size_t kMaximumIrErrorCodeBytes = 128;

struct IrRulesetProof {
  std::string rulesetId;
  int rulesetRevision = 0;
  std::string validationFingerprint;

  bool operator==(const IrRulesetProof &) const = default;
};

enum class IrOutboxState : int {
  Pending = 0,
  Uploading = 1,
  AwaitingRemoteResult = 2,
  BlockedConfiguration = 3,
  FailedPermanent = 4,
  Succeeded = 5,
};

struct IrOutboxDraft {
  std::string providerId;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::string payloadJson;
  IrRulesetProof rulesetProof;
  std::int64_t createdAtUnixMillis = 0;

  bool operator==(const IrOutboxDraft &) const = default;
};

struct IrOutboxEntry {
  std::int64_t id = 0;
  std::string providerId;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::string payloadJson;
  IrRulesetProof rulesetProof;
  IrOutboxState state = IrOutboxState::Pending;
  bool localResultReady = false;
  int requestAttemptCount = 0;
  int consecutiveFailureCount = 0;
  int remotePollCount = 0;
  std::optional<std::int64_t> nextAttemptAtUnixMillis;
  bool nextRequestUserIntent = false;
  std::string remoteJobId;
  std::string remoteOrigin;
  std::string lastErrorCode;
  std::string lastErrorMessage;
  std::int64_t createdAtUnixMillis = 0;
  std::int64_t updatedAtUnixMillis = 0;
  std::optional<std::int64_t> completedAtUnixMillis;

  bool operator==(const IrOutboxEntry &) const = default;
};

struct IrOutboxDeliveryUpdate {
  std::int64_t rowId = 0;
  IrOutboxState nextState = IrOutboxState::Pending;
  int consecutiveFailureCount = 0;
  int remotePollCount = 0;
  std::optional<std::int64_t> nextAttemptAtUnixMillis;
  std::optional<std::string> remoteJobId;
  std::optional<std::string> remoteOrigin;
  std::string lastErrorCode;
  std::string lastErrorMessage;
  std::int64_t updatedAtUnixMillis = 0;
  std::optional<std::int64_t> completedAtUnixMillis;
  std::optional<IrSuccessfulReceiptDraft> successfulReceipt;
};

enum class IrOutboxInsertStatus {
  Inserted,
  AlreadyExists,
  AlreadySubmitted,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrOutboxInsertOutcome {
  IrOutboxInsertStatus status = IrOutboxInsertStatus::StorageFailure;
  std::optional<IrOutboxEntry> entry;
  std::string diagnostic;
};

enum class IrManualBatchItemStatus {
  Inserted,
  RetryQueued,
  AlreadyQueued,
  AlreadySubmitted,
  Failed,
};

struct IrManualBatchItemOutcome {
  std::string attemptId;
  IrManualBatchItemStatus status = IrManualBatchItemStatus::Failed;
  std::optional<IrOutboxEntry> entry;
  std::string diagnostic;
};

struct IrManualBatchEnqueueOutcome {
  bool storageAvailable = false;
  std::vector<IrManualBatchItemOutcome> items;
  std::string diagnostic;
};

enum class IrOutboxReadStatus {
  Found,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrOutboxReadOutcome {
  IrOutboxReadStatus status = IrOutboxReadStatus::StorageFailure;
  std::optional<IrOutboxEntry> entry;
  std::string diagnostic;
};

enum class IrOutboxBatchStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrOutboxBatchOutcome {
  IrOutboxBatchStatus status = IrOutboxBatchStatus::StorageFailure;
  std::vector<IrOutboxEntry> entries;
  std::string diagnostic;
};

enum class IrOutboxClaimStatus {
  Claimed,
  NotFound,
  StateMismatch,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrOutboxClaimRequest {
  std::int64_t rowId = 0;
  IrOutboxState expectedState = IrOutboxState::Pending;
};

struct IrOutboxBatchClaimOutcome {
  IrOutboxClaimStatus status = IrOutboxClaimStatus::StorageFailure;
  std::vector<IrOutboxEntry> entries;
  bool consumedUserIntent = false;
  std::string diagnostic;
};

struct IrOutboxClaimOutcome {
  IrOutboxClaimStatus status = IrOutboxClaimStatus::StorageFailure;
  std::optional<IrOutboxEntry> entry;
  bool consumedUserIntent = false;
  std::string diagnostic;
};

enum class IrOutboxMutationStatus {
  Updated,
  NotFound,
  StateMismatch,
  Invalid,
  StorageFailure,
};

struct IrOutboxMutationOutcome {
  IrOutboxMutationStatus status = IrOutboxMutationStatus::StorageFailure;
  std::size_t affectedRows = 0;
  std::string diagnostic;
};

struct IrOutboxCounts {
  bool storageAvailable = false;
  std::size_t pending = 0;
  std::size_t uploading = 0;
  std::size_t awaitingRemoteResult = 0;
  std::size_t blockedConfiguration = 0;
  std::size_t failedPermanent = 0;
  std::size_t succeeded = 0;
  std::size_t total = 0;
  std::string diagnostic;
};

[[nodiscard]] std::string sanitizeDiagnostic(std::string_view value);
[[nodiscard]] bool isKnownIrOutboxState(int value) noexcept;
[[nodiscard]] bool validateIrOutboxDraft(const IrOutboxDraft &draft,
                                         std::string &diagnostic) noexcept;
[[nodiscard]] bool validateIrOutboxEntry(const IrOutboxEntry &entry,
                                         std::string &diagnostic) noexcept;

} // namespace ir
