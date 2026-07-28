#include "IrUploadCandidates.h"

#include "IrProfileSettings.h"
#include "tachi/TachiEligibility.h"

#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace ir {
namespace {

bool sourceAgrees(const IrUploadCandidateSource &source,
                  std::string_view providerId, std::string_view serverOrigin,
                  std::string &diagnostic) noexcept {
  if (source.modernChartResultId <= 0 ||
      source.result.resultId != source.modernChartResultId ||
      !result_persistence::validateModernChartResult(source.result,
                                                     diagnostic)) {
    if (diagnostic.empty()) {
      diagnostic = "IR upload modern result ownership is invalid";
    }
    return false;
  }

  const auto expectedSnapshot =
      captureIrSubmissionSnapshot(source.result, diagnostic);
  if (!expectedSnapshot || *expectedSnapshot != source.snapshot) {
    if (diagnostic.empty()) {
      diagnostic = "IR upload snapshot disagrees with its modern result";
    }
    return false;
  }

  const auto &submission = source.snapshot.submission;
  if (submission.attemptId != source.result.attemptId ||
      submission.chartMd5 != source.result.score.chartMd5 ||
      submission.chartSha256 != source.result.score.chartSha256) {
    diagnostic = "IR upload snapshot identity disagrees with its modern result";
    return false;
  }

  if (source.receipt) {
    if (!validateIrSubmissionReceipt(*source.receipt, diagnostic) ||
        source.receipt->providerId != providerId ||
        source.receipt->serverOrigin != serverOrigin ||
        source.receipt->replayId != 0 ||
        source.receipt->modernChartResultId != source.modernChartResultId ||
        source.receipt->attemptId != source.result.attemptId ||
        (!source.result.score.chartMd5.empty() &&
         !source.receipt->chartMd5.empty() &&
         source.receipt->chartMd5 != source.result.score.chartMd5) ||
        source.receipt->chartSha256 != source.result.score.chartSha256) {
      if (diagnostic.empty()) {
        diagnostic = "IR upload receipt disagrees with its modern result";
      }
      return false;
    }
  }

  if (source.outbox) {
    if (!validateIrOutboxEntry(*source.outbox, diagnostic) ||
        source.outbox->providerId != providerId ||
        source.outbox->attemptId != source.result.attemptId ||
        !source.outbox->localResultReady ||
        (!source.result.score.chartMd5.empty() &&
         !source.outbox->chartMd5.empty() &&
         source.outbox->chartMd5 != source.result.score.chartMd5) ||
        source.outbox->chartSha256 != source.result.score.chartSha256) {
      if (diagnostic.empty()) {
        diagnostic = "IR upload outbox row disagrees with its modern result";
      }
      return false;
    }
  }
  diagnostic.clear();
  return true;
}

template <typename Projection>
void setOmissionDiagnostic(Projection &result) {
  if (result.omittedRows == 0) {
    return;
  }
  result.diagnostic = sanitizeDiagnostic(
      std::to_string(result.omittedRows) +
      " modern result rows were omitted because durable IR facts disagreed.");
}

} // namespace

bool isSubmissionEligibleForProvider(
    std::string_view providerId,
    const IrSubmission &submission) noexcept {
  if (!ir::isValidProviderId(providerId)) {
    return false;
  }
  if (providerId == tachi::kProviderId) {
    return tachi::validateBokutachiEligibility(submission).eligible();
  }

  std::string diagnostic;
  return validateIrSubmission(submission, diagnostic) &&
         submission.provenance.eligibility == ScoreEligibility::Verified;
}

bool validateIrUploadCandidateSource(
    const IrUploadCandidateSource &source, std::string_view providerId,
    std::string_view serverOrigin, std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    const auto normalizedOrigin = normalizeServerOrigin(serverOrigin);
    if (!ir::isValidProviderId(providerId) || !normalizedOrigin ||
        *normalizedOrigin != serverOrigin) {
      diagnostic = "IR upload candidate scope is invalid";
      return false;
    }
    return sourceAgrees(source, providerId, serverOrigin, diagnostic);
  } catch (...) {
    diagnostic = "IR upload candidate validation failed";
    return false;
  }
}

IrUploadRecordProjection projectIrUploadRecords(
    std::span<const IrUploadCandidateSource> sources,
    std::string_view providerId, std::string_view serverOrigin) noexcept {
  IrUploadRecordProjection result;
  try {
    const auto normalizedOrigin = normalizeServerOrigin(serverOrigin);
    if (!ir::isValidProviderId(providerId) || !normalizedOrigin ||
        *normalizedOrigin != serverOrigin ||
        sources.size() > kMaximumIrUploadCandidateRows) {
      result.omittedRows = sources.size();
      result.diagnostic = "IR upload candidate scope is invalid or oversized.";
      return result;
    }

    std::unordered_map<int, std::size_t> ownerCounts;
    std::unordered_map<std::string_view, std::size_t> attemptCounts;
    ownerCounts.reserve(sources.size());
    attemptCounts.reserve(sources.size());
    for (const auto &source : sources) {
      ++ownerCounts[source.modernChartResultId];
      ++attemptCounts[source.result.attemptId];
    }

    result.records.reserve(sources.size());
    for (const auto &source : sources) {
      std::string diagnostic;
      if (ownerCounts[source.modernChartResultId] != 1 ||
          attemptCounts[source.result.attemptId] != 1 ||
          !validateIrUploadCandidateSource(source, providerId, serverOrigin,
                                           diagnostic)) {
        ++result.omittedRows;
        continue;
      }
      const bool eligible =
          isSubmissionEligibleForProvider(providerId,
                                          source.snapshot.submission);
      const IrRecordState state = resolveIrRecordState({
          .eligible = eligible,
          .hasReceipt = source.receipt.has_value(),
          .outboxState = source.outbox
                             ? std::optional(source.outbox->state)
                             : std::nullopt,
      });
      result.records.push_back({
          .modernChartResultId = source.modernChartResultId,
          .attemptId = source.result.attemptId,
          .eligible = eligible,
          .hasReceipt = source.receipt.has_value(),
          .outboxState = source.outbox
                             ? std::optional(source.outbox->state)
                             : std::nullopt,
          .failureReason =
              state == IrRecordState::Failed && source.outbox
                  ? sanitizeDiagnostic(source.outbox->lastErrorMessage)
                  : std::string{},
      });
    }
    setOmissionDiagnostic(result);
  } catch (...) {
    result.records.clear();
    result.omittedRows = sources.size();
    result.diagnostic = "IR upload records are unavailable.";
  }
  return result;
}

IrUploadCandidateProjection projectIrUploadCandidates(
    std::span<const IrUploadCandidateSource> sources,
    std::string_view providerId, std::string_view serverOrigin) noexcept {
  IrUploadCandidateProjection result;
  try {
    auto records = projectIrUploadRecords(sources, providerId, serverOrigin);
    result.omittedRows = records.omittedRows;
    result.diagnostic = std::move(records.diagnostic);

    std::unordered_map<int, const IrUploadRecord *> recordsByOwner;
    recordsByOwner.reserve(records.records.size());
    for (const auto &record : records.records) {
      recordsByOwner.emplace(record.modernChartResultId, &record);
    }

    result.candidates.reserve(records.records.size());
    for (const auto &source : sources) {
      const auto found = recordsByOwner.find(source.modernChartResultId);
      if (found == recordsByOwner.end()) {
        continue;
      }
      const IrUploadRecord &record = *found->second;
      const IrRecordState state = record.resolvedState();
      if (!record.eligible ||
          (state != IrRecordState::Eligible &&
           state != IrRecordState::Failed)) {
        continue;
      }
      result.candidates.push_back({
          .modernChartResultId = source.modernChartResultId,
          .result = source.result,
          .snapshot = source.snapshot,
          .state = state,
          .failureReason = record.failureReason,
      });
    }
  } catch (...) {
    result.candidates.clear();
    result.omittedRows = sources.size();
    result.diagnostic = "IR upload candidates are unavailable.";
  }
  return result;
}

std::optional<IrSubmission>
submissionForIrUploadCandidate(const IrUploadCandidate &candidate,
                               std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    if (candidate.modernChartResultId <= 0 ||
        candidate.result.resultId != candidate.modernChartResultId ||
        !result_persistence::validateModernChartResult(candidate.result,
                                                       diagnostic)) {
      if (diagnostic.empty()) {
        diagnostic = "IR upload candidate result is invalid";
      }
      return std::nullopt;
    }
    const auto expected =
        captureIrSubmissionSnapshot(candidate.result, diagnostic);
    if (!expected || *expected != candidate.snapshot) {
      if (diagnostic.empty()) {
        diagnostic = "IR upload candidate snapshot disagrees with its result";
      }
      return std::nullopt;
    }
    return candidate.snapshot.submission;
  } catch (...) {
    diagnostic = "IR upload candidate validation failed";
    return std::nullopt;
  }
}

void intersectIrUploadSelection(
    std::unordered_set<std::string> &selectedAttemptIds,
    std::span<const IrUploadCandidate> candidates) {
  detail::intersectIrUploadSelectionIndexed(selectedAttemptIds, candidates);
}

} // namespace ir
