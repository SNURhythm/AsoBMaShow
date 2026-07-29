#pragma once

#include "IrOutboxModels.h"
#include "IrSubmissionSnapshot.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ir {

inline constexpr std::size_t kMaximumIrUploadCandidateRows = 16384;
inline constexpr std::size_t kDefaultIrUploadSourcePageRows = 512;

// Repository-owned facts read from one SQLite snapshot. Replay files and
// legacy summary rows are deliberately absent from this input contract.
struct IrUploadCandidateSource {
  int modernChartResultId = 0;
  result_persistence::ModernChartResult result;
  IrSubmissionSnapshot snapshot;
  std::optional<IrSubmissionReceipt> receipt;
  std::optional<IrOutboxEntry> outbox;
};

// Provider-scoped durable state for a snapshot-backed modern result. The raw
// inputs are retained so a presentation can overlay live service activity via
// the same state resolver without overriding a durable receipt.
struct IrUploadRecord {
  int modernChartResultId = 0;
  std::string attemptId;
  bool eligible = false;
  bool hasReceipt = false;
  std::optional<std::string> receiptRemoteScoreId;
  std::optional<IrOutboxState> outboxState;
  std::string failureReason;

  [[nodiscard]] IrRecordState resolvedState(
      IrRecordActivity activity = IrRecordActivity::None) const noexcept {
    return resolveIrRecordState({.eligible = eligible,
                                 .hasReceipt = hasReceipt,
                                 .outboxState = outboxState,
                                 .activity = activity});
  }
};

struct IrUploadRecordProjection {
  std::vector<IrUploadRecord> records;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

struct IrUploadCandidate {
  int modernChartResultId = 0;
  result_persistence::ModernChartResult result;
  IrSubmissionSnapshot snapshot;
  IrRecordState state = IrRecordState::Hidden;
  std::string failureReason;

  [[nodiscard]] std::string_view attemptId() const noexcept {
    return result.attemptId;
  }
};

struct IrUploadCandidateProjection {
  std::vector<IrUploadCandidate> candidates;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

enum class IrUploadCandidateReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrUploadCandidateReadOutcome {
  IrUploadCandidateReadStatus status =
      IrUploadCandidateReadStatus::StorageFailure;
  std::vector<IrUploadCandidate> candidates;
  std::size_t omittedRows = 0;
  // Exclusive keyset cursor for the next older page, when one exists.
  std::optional<int> nextBeforeModernChartResultId;
  std::string diagnostic;
};

enum class IrUploadRecordReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrUploadRecordReadOutcome {
  IrUploadRecordReadStatus status = IrUploadRecordReadStatus::StorageFailure;
  std::vector<IrUploadRecord> records;
  std::size_t omittedRows = 0;
  // Exclusive keyset cursor for the next older page, when one exists.
  std::optional<int> nextBeforeModernChartResultId;
  std::string diagnostic;
};

// The single result/snapshot/receipt/outbox agreement authority used by both
// repository reads and in-memory projections.
[[nodiscard]] bool validateIrUploadCandidateSource(
    const IrUploadCandidateSource &source, std::string_view providerId,
    std::string_view serverOrigin, std::string &diagnostic) noexcept;

// Provider eligibility is evaluated from the immutable snapshot submission at
// every Records, manual-upload, and reconciliation projection boundary. Known
// providers may narrow the provider-neutral verified-provenance floor; the
// driver's draft builder remains authoritative for delivery-specific policy.
[[nodiscard]] bool isSubmissionEligibleForProvider(
    std::string_view providerId,
    const IrSubmission &submission) noexcept;

[[nodiscard]] IrUploadRecordProjection projectIrUploadRecords(
    std::span<const IrUploadCandidateSource> sources,
    std::string_view providerId, std::string_view serverOrigin) noexcept;

[[nodiscard]] IrUploadCandidateProjection projectIrUploadCandidates(
    std::span<const IrUploadCandidateSource> sources,
    std::string_view providerId, std::string_view serverOrigin) noexcept;

// Revalidates the copied durable result/snapshot pair immediately before a
// manual enqueue and returns the stored provider-neutral submission unchanged.
[[nodiscard]] std::optional<IrSubmission>
submissionForIrUploadCandidate(const IrUploadCandidate &candidate,
                               std::string &diagnostic) noexcept;

namespace detail {

template <typename CandidateRange>
void intersectIrUploadSelectionIndexed(
    std::unordered_set<std::string> &selectedAttemptIds,
    const CandidateRange &candidates) {
  std::unordered_set<std::string> publishedAttemptIds;
  publishedAttemptIds.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    publishedAttemptIds.emplace(candidate.attemptId());
  }
  std::erase_if(selectedAttemptIds, [&](const std::string &attemptId) {
    return !publishedAttemptIds.contains(attemptId);
  });
}

} // namespace detail

void intersectIrUploadSelection(
    std::unordered_set<std::string> &selectedAttemptIds,
    std::span<const IrUploadCandidate> candidates);

} // namespace ir
