#include "CourseReplayContext.h"

#include "BeatorajaReplayPath.h"
#include "CourseReplayAgreement.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <utility>

namespace replay {
namespace {

CourseReplayContextOutcome failure(
    CourseReplayContextState state, std::string diagnostic,
    std::optional<result_persistence::ModernCourseResult> result = std::nullopt,
    std::optional<ModernReplayFileReference> reference = std::nullopt) {
  return {.state = state,
          .result = std::move(result),
          .reference = std::move(reference),
          .diagnostic = std::move(diagnostic)};
}

CourseReplayContextState fileFailureState(ReplayFileState state) noexcept {
  switch (state) {
  case ReplayFileState::Available:
    break;
  case ReplayFileState::Missing:
    return CourseReplayContextState::FileMissing;
  case ReplayFileState::Corrupt:
    return CourseReplayContextState::FileCorrupt;
  case ReplayFileState::Unsafe:
    return CourseReplayContextState::FileUnsafe;
  case ReplayFileState::IoFailure:
    return CourseReplayContextState::FileIoFailure;
  }
  return CourseReplayContextState::FileIoFailure;
}

ReplaySetupSource setupSource(ReplayStageDecodeSource source) noexcept {
  return source == ReplayStageDecodeSource::AsoExtension
             ? ReplaySetupSource::AsoExtension
             : ReplaySetupSource::StockBeatoraja;
}

CourseReplayContextState agreementFailureState(
    CourseReplayAgreementIssue issue) noexcept {
  switch (issue) {
  case CourseReplayAgreementIssue::StageIdentity:
    return CourseReplayContextState::StageMismatch;
  case CourseReplayAgreementIssue::LongNoteMode:
    return CourseReplayContextState::LongNoteModeMismatch;
  case CourseReplayAgreementIssue::SharedSetup:
    return CourseReplayContextState::SharedFactsMismatch;
  case CourseReplayAgreementIssue::CourseShape:
    return CourseReplayContextState::CourseShapeMismatch;
  case CourseReplayAgreementIssue::Path:
    return CourseReplayContextState::ReferenceMismatch;
  case CourseReplayAgreementIssue::None:
  case CourseReplayAgreementIssue::Result:
  case CourseReplayAgreementIssue::Playback:
    return CourseReplayContextState::ReplayInvalid;
  }
  return CourseReplayContextState::ReplayInvalid;
}

bool referenceAgrees(const ModernReplayFileReference &reference,
                     const result_persistence::ModernCourseResult &result,
                     const CoursePathInput &pathInput,
                     const ReplayLimits &limits,
                     std::string &diagnostic) {
  const auto agreement =
      compareCourseReplayPathToResult(pathInput, result, limits);
  if (!agreement.agrees()) {
    diagnostic = agreement.diagnostic;
    return false;
  }
  const auto expectedStem = courseStem(pathInput, diagnostic, limits);
  if (reference.id <= 0 || reference.resultId != result.resultId ||
      reference.identity.relativePath != reference.metadata.relativePath ||
      !expectedStem || reference.identity.stem != *expectedStem) {
    if (diagnostic.empty()) {
      diagnostic = "Replay reference does not belong to the saved course.";
    }
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

CourseReplayContext::CourseReplayContext(ReplayRepository &repository,
                                         ReplayLimits limits)
    : limits_(limits) {
  auto store = std::make_shared<ReplayFileStore>(
      repository.GetResolvedProfileRoot(), ReplayFileStoreFaults{}, limits);
  auto codec = std::make_shared<BeatorajaReplayCodec>(limits);
  dependencies_ = {
      .loadResult = [&repository](std::string_view attemptId) {
        return repository.LoadModernCourseResultByAttempt(attemptId);
      },
      .readVerifiedFile = [store](const ReplayFileMetadata &metadata) {
        return store->readVerified(metadata);
      },
      .decode = [codec](std::span<const std::byte> bytes,
                        const ReplayDecodeContext &context) {
        return codec->decode(bytes, context);
      },
  };
}

CourseReplayContext::CourseReplayContext(
    CourseReplayContextDependencies dependencies, ReplayLimits limits)
    : dependencies_(std::move(dependencies)), limits_(limits) {}

CourseReplayContextOutcome CourseReplayContext::load(
    std::string_view attemptId,
    const ParsedCourseReplayFacts &parsedCourse) const noexcept {
  std::optional<result_persistence::ModernCourseResult> preservedResult;
  try {
    if (!limits_.valid() || !dependencies_.loadResult ||
        !dependencies_.readVerifiedFile || !dependencies_.decode) {
      return failure(CourseReplayContextState::InvalidRequest,
                     "Course replay context dependencies are incomplete.");
    }

    const auto loaded = dependencies_.loadResult(attemptId);
    switch (loaded.status) {
    case ModernCourseResultReadStatus::NotFound:
      return failure(CourseReplayContextState::ResultNotFound,
                     loaded.diagnostic);
    case ModernCourseResultReadStatus::StorageFailure:
      return failure(CourseReplayContextState::ResultUnavailable,
                     loaded.diagnostic);
    case ModernCourseResultReadStatus::Invalid:
    case ModernCourseResultReadStatus::IntegrityConflict:
      return failure(CourseReplayContextState::ResultInvalid,
                     loaded.diagnostic);
    case ModernCourseResultReadStatus::Loaded:
      break;
    }
    if (!loaded.record) {
      return failure(CourseReplayContextState::ResultInvalid,
                     "Loaded modern course result is absent.");
    }

    const auto &stored = loaded.record->result;
    std::string diagnostic;
    if (stored.attemptId != attemptId ||
        !result_persistence::validateModernCourseResult(stored, diagnostic)) {
      return failure(CourseReplayContextState::ResultInvalid,
                     diagnostic.empty() ? "Modern course result row is invalid."
                                        : std::move(diagnostic));
    }
    preservedResult = stored;

    if (parsedCourse.stages.size() != stored.stages.size()) {
      return failure(
          CourseReplayContextState::CourseShapeMismatch,
          "Parsed course prefix differs from the saved completed prefix.",
          preservedResult);
    }
    bool anyTimeBounds = false;
    bool allTimeBounds = true;
    bool hasUndefinedLongNotes = false;
    for (std::size_t index = 0; index < stored.stages.size(); ++index) {
      const auto &parsed = parsedCourse.stages[index];
      const auto &saved = stored.stages[index];
      const ReplayChartIdentity expected{.md5 = saved.score.chartMd5,
                                         .sha256 = saved.score.chartSha256,
                                         .keyMode = saved.keyMode};
      if (compareReplayChartIdentity(parsed.chart, expected) !=
          ReplayChartMatch::Match) {
        return failure(
            CourseReplayContextState::StageMismatch,
            "Parsed course stage identity differs from the saved result.",
            preservedResult);
      }
      if (parsed.longNoteMode != saved.score.longNoteMode ||
          parsed.longNoteMode != stored.longNoteMode) {
        return failure(
            CourseReplayContextState::LongNoteModeMismatch,
            "Parsed course stage long-note mode differs from the saved result.",
            preservedResult);
      }
      if (parsed.timeBounds && !parsed.timeBounds->valid()) {
        return failure(CourseReplayContextState::InvalidRequest,
                       "Parsed course stage completion time is invalid.",
                       preservedResult);
      }
      anyTimeBounds = anyTimeBounds || parsed.timeBounds.has_value();
      allTimeBounds = allTimeBounds && parsed.timeBounds.has_value();
      hasUndefinedLongNotes =
          hasUndefinedLongNotes || parsed.hasUndefinedLongNotes;
    }
    if (anyTimeBounds != allTimeBounds) {
      return failure(CourseReplayContextState::InvalidRequest,
                     "Parsed course time bounds must be all present or absent.",
                     preservedResult);
    }
    if (!loaded.record->replayFile) {
      return failure(CourseReplayContextState::ReplayNotAttached,
                     "The saved course result has no replay file reference.",
                     preservedResult);
    }
    if (loaded.record->replayFile->userDeleted) {
      return failure(CourseReplayContextState::FileUserDeleted,
                     "The course replay file was deleted by the user.",
                     preservedResult, loaded.record->replayFile);
    }

    std::optional<ModernReplayFileReference> reference =
        *loaded.record->replayFile;
    const auto parsedPath =
        courseReplayPathInputForResult(stored, hasUndefinedLongNotes);
    if (!referenceAgrees(*reference, stored, parsedPath, limits_, diagnostic)) {
      return failure(CourseReplayContextState::ReferenceMismatch,
                     std::move(diagnostic), preservedResult,
                     std::move(reference));
    }
    if (reference->metadata.codecVersion !=
        BeatorajaReplayCodec::kCodecVersion) {
      return failure(CourseReplayContextState::UnsupportedCodecVersion,
                     "Replay reference uses an unsupported codec version.",
                     preservedResult, std::move(reference));
    }

    ReplayFileReadOutcome bytes;
    try {
      bytes = dependencies_.readVerifiedFile(reference->metadata);
    } catch (...) {
      return failure(CourseReplayContextState::FileIoFailure,
                     "Course replay file verification failed.",
                     preservedResult, std::move(reference));
    }
    if (bytes.state != ReplayFileState::Available || !bytes.bytes) {
      return failure(fileFailureState(bytes.state), bytes.diagnostic,
                     preservedResult, std::move(reference));
    }

    ReplayDecodeContext decodeContext;
    decodeContext.stageKeyModes.reserve(parsedCourse.stages.size());
    decodeContext.stageTimeBounds.reserve(parsedCourse.stages.size());
    for (const auto &parsed : parsedCourse.stages) {
      decodeContext.stageKeyModes.push_back(parsed.chart.keyMode);
      if (allTimeBounds) {
        decodeContext.stageTimeBounds.push_back(*parsed.timeBounds);
      }
    }
    ReplayDecodeOutcome decoded;
    try {
      decoded = dependencies_.decode(*bytes.bytes, decodeContext);
    } catch (...) {
      return failure(CourseReplayContextState::DecodeFailed,
                     "Course replay decoding failed.", preservedResult,
                     std::move(reference));
    }
    if (decoded.unsupportedAsoExtension) {
      return failure(CourseReplayContextState::UnsupportedExtension,
                     decoded.diagnostic.empty()
                         ? "Course replay uses an unsupported Aso extension."
                         : decoded.diagnostic,
                     preservedResult, std::move(reference));
    }
    if (!decoded.course || decoded.chart ||
        decoded.stageSources.size() != stored.stages.size()) {
      return failure(CourseReplayContextState::DecodeFailed,
                     decoded.diagnostic.empty()
                         ? "Replay file did not decode as the saved course."
                         : decoded.diagnostic,
                     preservedResult, std::move(reference));
    }
    if (allTimeBounds && decoded.course->timeBounds !=
                             decodeContext.stageTimeBounds) {
      return failure(CourseReplayContextState::ReplayInvalid,
                     "Decoded course completion bounds differ from parsed stages.",
                     preservedResult, std::move(reference));
    }

    std::vector<ReplaySetupSource> sources;
    sources.reserve(decoded.stageSources.size());
    for (const auto source : decoded.stageSources) {
      sources.push_back(setupSource(source));
    }
    const auto agreement = compareCourseReplayToResult(
        *decoded.course, stored, sources, limits_);
    if (!agreement.agrees()) {
      return failure(agreementFailureState(agreement.issue),
                     agreement.diagnostic, preservedResult,
                     std::move(reference));
    }

    for (std::size_t index = 0; index < stored.stages.size(); ++index) {
      const auto &parsed = parsedCourse.stages[index];
      const auto &setup = decoded.course->playback.stages[index].setup;
      if (compareReplayChartIdentity(setup.chart, parsed.chart) !=
          ReplayChartMatch::Match) {
        return failure(CourseReplayContextState::StageMismatch,
                       "Decoded replay stage differs from the parsed chart.",
                       preservedResult, std::move(reference));
      }
      if (setup.longNoteMode != parsed.longNoteMode) {
        return failure(
            CourseReplayContextState::LongNoteModeMismatch,
            "Decoded replay stage long-note mode differs from the parsed chart.",
            preservedResult, std::move(reference));
      }
      if (decoded.stageSources[index] ==
              ReplayStageDecodeSource::AsoExtension &&
          setup.hasUndefinedLongNotes != parsed.hasUndefinedLongNotes) {
        return failure(CourseReplayContextState::SharedFactsMismatch,
                       "Decoded undefined-LN setup differs from the parsed chart.",
                       preservedResult, std::move(reference));
      }
    }
    const auto decodedUndefinedLongNotes =
        decoded.replayPathHasUndefinedLongNotes();
    if (decodedUndefinedLongNotes.has_value() &&
        *decodedUndefinedLongNotes != hasUndefinedLongNotes) {
      return failure(CourseReplayContextState::SharedFactsMismatch,
                     "Decoded undefined-LN path fact differs from parsed charts.",
                     preservedResult, std::move(reference));
    }
    const auto decodedPath = courseReplayPathInputForResult(
        stored, decodedUndefinedLongNotes.value_or(hasUndefinedLongNotes));
    if (!referenceAgrees(*reference, stored, decodedPath, limits_, diagnostic)) {
      return failure(CourseReplayContextState::ReferenceMismatch,
                     std::move(diagnostic), preservedResult,
                     std::move(reference));
    }

    VerifiedCourseReplay verified{
        .result = stored,
        .reference = *reference,
        .document = *decoded.course,
        .stageSources = decoded.stageSources,
        .pathInput = decodedPath,
    };
    return {.state = CourseReplayContextState::Ready,
            .result = stored,
            .reference = *reference,
            .verified = std::move(verified)};
  } catch (...) {
    return failure(CourseReplayContextState::ResultUnavailable,
                   "Course replay context loading failed.",
                   std::move(preservedResult));
  }
}

} // namespace replay
