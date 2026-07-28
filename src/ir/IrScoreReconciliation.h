#pragma once

#include "IrOutboxModels.h"
#include "IrRemoteScoreModels.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct IrLocalReceiptCandidate {
  int modernChartResultId = 0;
  std::string attemptId;
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int score = 0;
  int lampRank = 0;
  bool eligible = false;
  std::optional<IrSubmissionReceipt> currentReceipt;
  std::optional<std::int64_t> outboxRowId;
  std::optional<IrOutboxState> outboxState;
};

struct IrScoreReconciliationPlan {
  enum class Status { Planned, Invalid };

  Status status = Status::Planned;
  std::vector<IrSubmissionReceipt> upsertedReceipts;
  std::vector<std::int64_t> deletedReceiptIds;
  std::vector<std::int64_t> settledOutboxRowIds;
  std::vector<std::int64_t> purgedSucceededOutboxRowIds;
  int ambiguousReceiptsPreserved = 0;
  std::string diagnostic;
};

[[nodiscard]] IrScoreReconciliationPlan planScoreReconciliation(
    std::string_view providerId, std::string_view serverOrigin,
    std::span<const IrLocalReceiptCandidate> local,
    std::span<const IrRemoteScore> remote, std::int64_t confirmedAtUnixMillis);

struct IrReconciliationReadOutcome {
  enum class Status { Loaded, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  std::vector<IrLocalReceiptCandidate> candidates;
  // Exclusive keyset cursor for the next older page, when one exists.
  std::optional<int> nextBeforeModernChartResultId;
  std::string diagnostic;
};

} // namespace ir
