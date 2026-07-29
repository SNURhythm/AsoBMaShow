#include "ChartReplayContext.h"

#include "BeatorajaReplayPath.h"
#include "ChartReplayAgreement.h"
#include "ReplayReferenceAgreement.h"

#include <memory>
#include <utility>

namespace replay {
namespace {

ChartReplayContextOutcome failure(
    ChartReplayContextState state, std::string diagnostic,
    std::optional<result_persistence::ModernChartResult> result = std::nullopt,
    std::optional<ModernReplayFileReference> reference = std::nullopt) {
  return {.state = state,
          .result = std::move(result),
          .reference = std::move(reference),
          .diagnostic = std::move(diagnostic)};
}

ChartReplayContextState fileFailureState(ReplayFileState state) noexcept {
  switch (state) {
  case ReplayFileState::Available:
    break;
  case ReplayFileState::Missing:
    return ChartReplayContextState::FileMissing;
  case ReplayFileState::Corrupt:
    return ChartReplayContextState::FileCorrupt;
  case ReplayFileState::Unsafe:
    return ChartReplayContextState::FileUnsafe;
  case ReplayFileState::IoFailure:
    return ChartReplayContextState::FileIoFailure;
  }
  return ChartReplayContextState::FileIoFailure;
}

} // namespace

ReplayState ChartReplayContextOutcome::replayState() const noexcept {
  switch (state) {
  case ChartReplayContextState::Ready:
    return ReplayState::Verified;
  case ChartReplayContextState::FileCorrupt:
    return ReplayState::Corrupt;
  case ChartReplayContextState::UnsupportedExtension:
  case ChartReplayContextState::UnsupportedCodecVersion:
    return ReplayState::UnsupportedExtension;
  case ChartReplayContextState::ChartMismatch:
  case ChartReplayContextState::LongNoteModeMismatch:
  case ChartReplayContextState::ReferenceMismatch:
  case ChartReplayContextState::FileUnsafe:
  case ChartReplayContextState::DecodeFailed:
  case ChartReplayContextState::ReplayInvalid:
  case ChartReplayContextState::SharedFactsMismatch:
    return ReplayState::Mismatched;
  case ChartReplayContextState::ReplayNotAttached:
  case ChartReplayContextState::FileMissing:
  case ChartReplayContextState::FileIoFailure:
    return ReplayState::Missing;
  case ChartReplayContextState::FileUserDeleted:
    return ReplayState::UserDeleted;
  case ChartReplayContextState::InvalidRequest:
  case ChartReplayContextState::ResultNotFound:
  case ChartReplayContextState::ResultUnavailable:
  case ChartReplayContextState::ResultInvalid:
    return ReplayState::NotApplicable;
  }
  return ReplayState::NotApplicable;
}

ChartReplayContext::ChartReplayContext(ReplayRepository &repository,
                                       ReplayLimits limits)
    : limits_(limits) {
  auto store = std::make_shared<ReplayFileStore>(
      repository.GetResolvedProfileRoot(), ReplayFileStoreFaults{}, limits);
  auto codec = std::make_shared<BeatorajaReplayCodec>(limits);
  dependencies_ = {
      .loadResult = [&repository](std::string_view attemptId) {
        return repository.LoadModernChartResultByAttempt(attemptId);
      },
      .readVerifiedFile =
          [store](const ReplayFileMetadata &metadata) {
            return store->readVerified(metadata);
          },
      .decode = [codec](std::span<const std::byte> bytes,
                        const ReplayDecodeContext &context) {
        return codec->decode(bytes, context);
      }};
}

ChartReplayContext::ChartReplayContext(
    ChartReplayContextDependencies dependencies, ReplayLimits limits)
    : dependencies_(std::move(dependencies)), limits_(limits) {}

ChartReplayContextOutcome ChartReplayContext::load(
    std::string_view attemptId,
    const ParsedChartReplayFacts &parsedChart) const noexcept {
  std::optional<result_persistence::ModernChartResult> preservedResult;
  try {
    if (!limits_.valid() || !dependencies_.loadResult ||
        !dependencies_.readVerifiedFile || !dependencies_.decode) {
      return failure(ChartReplayContextState::InvalidRequest,
                     "Chart replay context dependencies are incomplete.");
    }

    const auto loaded = dependencies_.loadResult(attemptId);
    switch (loaded.status) {
    case ModernChartResultReadStatus::NotFound:
      return failure(ChartReplayContextState::ResultNotFound,
                     loaded.diagnostic);
    case ModernChartResultReadStatus::StorageFailure:
      return failure(ChartReplayContextState::ResultUnavailable,
                     loaded.diagnostic);
    case ModernChartResultReadStatus::Invalid:
    case ModernChartResultReadStatus::IntegrityConflict:
      return failure(ChartReplayContextState::ResultInvalid,
                     loaded.diagnostic);
    case ModernChartResultReadStatus::Loaded:
      break;
    }
    if (!loaded.record) {
      return failure(ChartReplayContextState::ResultInvalid,
                     "Loaded modern result is absent.");
    }

    const auto &stored = loaded.record->result;
    std::string diagnostic;
    if (stored.attemptId != attemptId ||
        !result_persistence::validateModernChartResult(stored, diagnostic)) {
      return failure(ChartReplayContextState::ResultInvalid,
                     diagnostic.empty() ? "Modern result row is invalid."
                                        : std::move(diagnostic));
    }
    preservedResult = stored;

    const ReplayChartIdentity expected{.md5 = stored.score.chartMd5,
                                       .sha256 = stored.score.chartSha256,
                                       .keyMode = stored.keyMode};
    if (compareReplayChartIdentity(parsedChart.chart, expected) !=
        ReplayChartMatch::Match) {
      return failure(ChartReplayContextState::ChartMismatch,
                     "Parsed chart identity differs from the saved result.",
                     preservedResult);
    }
    const auto expectedLongNoteMode =
        result_persistence::replaySetupLongNoteMode(stored.score);
    if (!expectedLongNoteMode ||
        parsedChart.longNoteMode != *expectedLongNoteMode) {
      return failure(
          ChartReplayContextState::LongNoteModeMismatch,
          "Parsed chart long-note mode differs from the saved result.",
          preservedResult);
    }
    if (parsedChart.timeBounds.has_value() &&
        !parsedChart.timeBounds->valid()) {
      return failure(ChartReplayContextState::InvalidRequest,
                     "Parsed chart completion time is invalid.",
                     preservedResult);
    }
    if (!loaded.record->replayFile) {
      return failure(ChartReplayContextState::ReplayNotAttached,
                     "The saved result has no replay file reference.",
                     preservedResult);
    }
    if (loaded.record->replayFile->userDeleted) {
      return failure(ChartReplayContextState::FileUserDeleted,
                     "The replay file was deleted by the user.",
                     preservedResult, loaded.record->replayFile);
    }

    std::optional<ModernReplayFileReference> reference =
        *loaded.record->replayFile;
    const auto referenceAgreement =
        compareChartReplayReferenceToResult(*reference, stored, std::nullopt,
                                            limits_);
    if (!referenceAgreement.matches) {
      return failure(ChartReplayContextState::ReferenceMismatch,
                     referenceAgreement.diagnostic, preservedResult,
                     std::move(reference));
    }
    if (reference->metadata.codecVersion !=
        BeatorajaReplayCodec::kCodecVersion) {
      return failure(ChartReplayContextState::UnsupportedCodecVersion,
                     "Replay reference uses an unsupported codec version.",
                     preservedResult, std::move(reference));
    }

    ReplayFileReadOutcome bytes;
    try {
      bytes = dependencies_.readVerifiedFile(reference->metadata);
    } catch (...) {
      return failure(ChartReplayContextState::FileIoFailure,
                     "Replay file verification failed.", preservedResult,
                     std::move(reference));
    }
    if (bytes.state != ReplayFileState::Available || !bytes.bytes) {
      return failure(fileFailureState(bytes.state), bytes.diagnostic,
                     preservedResult, std::move(reference));
    }

    const ReplayDecodeContext decodeContext{
        .stageKeyModes = {parsedChart.chart.keyMode},
        .stageTimeBounds = {}};
    ReplayDecodeOutcome decoded;
    try {
      decoded = dependencies_.decode(*bytes.bytes, decodeContext);
    } catch (...) {
      return failure(ChartReplayContextState::DecodeFailed,
                     "Replay decoding failed.", preservedResult,
                     std::move(reference));
    }
    if (decoded.unsupportedAsoExtension) {
      return failure(ChartReplayContextState::UnsupportedExtension,
                     decoded.diagnostic.empty()
                         ? "Replay uses an unsupported Aso extension."
                         : decoded.diagnostic,
                     preservedResult, std::move(reference));
    }
    if (!decoded.chart || decoded.course || decoded.stageSources.size() != 1) {
      return failure(ChartReplayContextState::DecodeFailed,
                     decoded.diagnostic.empty()
                         ? "Replay file did not decode as one chart."
                         : decoded.diagnostic,
                     preservedResult, std::move(reference));
    }

    const auto source = decoded.stageSources.front();
    const auto decodedReferenceAgreement = compareChartReplayReferenceToResult(
        *reference, stored,
        decoded.chart->playback.setup.hasUndefinedLongNotes, limits_);
    if (!decodedReferenceAgreement.matches) {
      return failure(ChartReplayContextState::ReferenceMismatch,
                     decodedReferenceAgreement.diagnostic,
                     preservedResult, std::move(reference));
    }

    const auto agreement = compareChartReplayToResult(*decoded.chart, stored);
    if (!agreement.agrees()) {
      const auto state = agreement.issue == ChartReplayAgreementIssue::SharedSetup
                             ? ChartReplayContextState::SharedFactsMismatch
                             : agreement.issue ==
                                       ChartReplayAgreementIssue::LongNoteMode
                                   ? ChartReplayContextState::LongNoteModeMismatch
                                   : agreement.issue ==
                                             ChartReplayAgreementIssue::ChartIdentity
                                         ? ChartReplayContextState::ChartMismatch
                                         : ChartReplayContextState::ReplayInvalid;
      return failure(state, agreement.diagnostic, preservedResult,
                     std::move(reference));
    }

    VerifiedChartReplay verified{.result = stored,
                                 .reference = *reference,
                                 .document = *decoded.chart,
                                 .source = source};
    return {.state = ChartReplayContextState::Ready,
            .result = stored,
            .reference = *reference,
            .verified = std::move(verified)};
  } catch (...) {
    return failure(ChartReplayContextState::ResultUnavailable,
                   "Chart replay context loading failed.",
                   std::move(preservedResult));
  }
}

} // namespace replay
