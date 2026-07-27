#include "ReplayReferenceAgreement.h"

#include "../CourseConstraintUtils.h"
#include "../repositories/ReplayRepository.h"

#include <array>
#include <utility>

namespace replay {
namespace {

ReplayReferenceAgreement mismatch(std::string diagnostic) {
  return {.matches = false, .diagnostic = std::move(diagnostic)};
}

bool baseReferenceAgrees(const ModernReplayFileReference &reference,
                         int resultId, const ReplayLimits &limits,
                         std::string &diagnostic) {
  if (reference.id <= 0 || reference.resultId != resultId || resultId <= 0 ||
      reference.identity.relativePath != reference.metadata.relativePath) {
    diagnostic = "Replay reference does not belong to the saved result.";
    return false;
  }
  const auto canonical = pathForStem(reference.identity.stem,
                                     reference.identity.historyIndex,
                                     diagnostic, limits);
  if (!canonical || *canonical != reference.identity) {
    if (diagnostic.empty()) {
      diagnostic = "Replay reference path identity is inconsistent.";
    }
    return false;
  }
  return true;
}

} // namespace

CoursePathInput courseReplayPathInputForResult(
    const result_persistence::ModernCourseResult &result,
    bool hasUndefinedLongNotes) {
  CoursePathInput path{
      .longNoteMode = result.longNoteMode,
      .hasUndefinedLongNotes = hasUndefinedLongNotes,
      .beatorajaConstraintIds =
          beatorajaCourseConstraintIdsFromJson(result.constraintJson),
  };
  path.stageSha256.reserve(result.stages.size());
  for (const auto &stage : result.stages) {
    path.stageSha256.push_back(stage.score.chartSha256);
  }
  return path;
}

ReplayReferenceAgreement compareChartReplayReferenceToResult(
    const ModernReplayFileReference &reference,
    const result_persistence::ModernChartResult &result,
    const ReplayLimits &limits) noexcept {
  try {
    std::string diagnostic;
    if (!result_persistence::validateModernChartResult(result, diagnostic)) {
      return mismatch(diagnostic.empty() ? "Saved chart result is invalid."
                                         : std::move(diagnostic));
    }
    if (!baseReferenceAgrees(reference, result.resultId, limits, diagnostic) ||
        !chartStemMatches(reference.identity.stem, result.score.chartSha256,
                          result.score.longNoteMode, std::nullopt, diagnostic,
                          limits)) {
      return mismatch(diagnostic.empty()
                          ? "Replay reference does not match the saved chart."
                          : std::move(diagnostic));
    }
    return {.matches = true};
  } catch (...) {
    return mismatch("Chart replay reference agreement failed.");
  }
}

ReplayReferenceAgreement compareCourseReplayReferenceToResult(
    const ModernReplayFileReference &reference,
    const result_persistence::ModernCourseResult &result,
    std::optional<bool> hasUndefinedLongNotes,
    const ReplayLimits &limits) noexcept {
  try {
    std::string diagnostic;
    if (!result_persistence::validateModernCourseResult(result, diagnostic)) {
      return mismatch(diagnostic.empty() ? "Saved course result is invalid."
                                         : std::move(diagnostic));
    }
    if (!baseReferenceAgrees(reference, result.resultId, limits, diagnostic)) {
      return mismatch(std::move(diagnostic));
    }
    const std::array candidates = {false, true};
    for (const bool undefined : candidates) {
      if (hasUndefinedLongNotes && undefined != *hasUndefinedLongNotes) {
        continue;
      }
      const auto input = courseReplayPathInputForResult(result, undefined);
      std::string candidateDiagnostic;
      const auto expectedStem = courseStem(input, candidateDiagnostic, limits);
      if (expectedStem && reference.identity.stem == *expectedStem) {
        return {.matches = true};
      }
      if (diagnostic.empty()) {
        diagnostic = std::move(candidateDiagnostic);
      }
    }
    return mismatch(diagnostic.empty()
                        ? "Replay reference does not match the saved course."
                        : std::move(diagnostic));
  } catch (...) {
    return mismatch("Course replay reference agreement failed.");
  }
}

} // namespace replay
