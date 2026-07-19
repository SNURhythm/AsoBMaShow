#include "IrSavedResultBatchUpload.h"

#include <exception>
#include <optional>
#include <utility>

namespace ir {
namespace {

std::string failureDiagnostic(std::string_view diagnostic,
                              std::string_view fallback) {
  std::string sanitized = sanitizeDiagnostic(diagnostic);
  return sanitized.empty() ? std::string(fallback) : sanitized;
}

} // namespace

detail::IrManualBatchOutcomeIndex::IrManualBatchOutcomeIndex(
    std::span<const IrManualBatchItemOutcome> outcomes) {
  outcomes_.reserve(outcomes.size());
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    ++operationCount_;
    const auto [found, inserted] = outcomes_.try_emplace(
        outcomes[index].attemptId, IndexedOutcome{.index = index});
    if (!inserted) {
      found->second.duplicate = true;
    }
  }
}

std::optional<std::size_t>
detail::IrManualBatchOutcomeIndex::findUnique(std::string_view attemptId) {
  ++operationCount_;
  const auto found = outcomes_.find(attemptId);
  if (found == outcomes_.end() || found->second.duplicate) {
    return std::nullopt;
  }
  return found->second.index;
}

IrSavedResultBatchUploadResult executeIrSavedResultBatchUpload(
    std::string_view providerId, std::span<const IrSubmission> submissions,
    const IrSavedResultBatchUploadDependencies &dependencies) noexcept {
  IrSavedResultBatchUploadResult result;
  result.items.resize(submissions.size());
  std::vector<IrOutboxDraft> drafts;
  std::vector<std::size_t> draftIndexes;
  drafts.reserve(submissions.size());
  draftIndexes.reserve(submissions.size());

  for (std::size_t index = 0; index < submissions.size(); ++index) {
    result.items[index].attemptId = submissions[index].attemptId;
    if (!dependencies.buildDraft) {
      result.items[index].diagnostic = "IR draft construction is unavailable.";
      ++result.buildFailures;
      continue;
    }
    try {
      BuildDraftOutcome built = dependencies.buildDraft(submissions[index]);
      if (built.status != BuildDraftStatus::Built || !built.draft) {
        result.items[index].diagnostic = failureDiagnostic(
            built.diagnostic, "This score is not eligible for IR upload.");
        ++result.buildFailures;
        continue;
      }
      if (built.draft->providerId != providerId ||
          built.draft->attemptId != submissions[index].attemptId) {
        result.items[index].diagnostic =
            "IR draft identity does not match the saved score.";
        ++result.buildFailures;
        continue;
      }
      drafts.push_back(std::move(*built.draft));
      draftIndexes.push_back(index);
    } catch (const std::exception &error) {
      result.items[index].diagnostic =
          failureDiagnostic(error.what(), "IR draft construction failed.");
      ++result.buildFailures;
    } catch (...) {
      result.items[index].diagnostic = "IR draft construction failed.";
      ++result.buildFailures;
    }
  }

  if (drafts.empty()) {
    return result;
  }
  if (!dependencies.enqueueBatch) {
    result.diagnostic = "IR batch enqueue is unavailable.";
    for (const std::size_t index : draftIndexes) {
      result.items[index].diagnostic = result.diagnostic;
    }
    return result;
  }

  try {
    IrManualBatchEnqueueOutcome enqueued = dependencies.enqueueBatch(drafts);
    result.diagnostic = sanitizeDiagnostic(enqueued.diagnostic);
    detail::IrManualBatchOutcomeIndex indexed(enqueued.items);
    for (std::size_t draftIndex = 0; draftIndex < drafts.size(); ++draftIndex) {
      const std::size_t resultIndex = draftIndexes[draftIndex];
      const std::optional<std::size_t> match =
          indexed.findUnique(drafts[draftIndex].attemptId);
      if (match) {
        result.items[resultIndex] = std::move(enqueued.items[*match]);
        result.items[resultIndex].diagnostic =
            sanitizeDiagnostic(result.items[resultIndex].diagnostic);
      } else {
        result.items[resultIndex].diagnostic = failureDiagnostic(
            result.diagnostic, "IR batch enqueue returned no outcome.");
      }
    }
  } catch (const std::exception &error) {
    result.diagnostic =
        failureDiagnostic(error.what(), "IR batch enqueue failed.");
    for (const std::size_t index : draftIndexes) {
      result.items[index].diagnostic = result.diagnostic;
    }
  } catch (...) {
    result.diagnostic = "IR batch enqueue failed.";
    for (const std::size_t index : draftIndexes) {
      result.items[index].diagnostic = result.diagnostic;
    }
  }
  return result;
}

} // namespace ir
