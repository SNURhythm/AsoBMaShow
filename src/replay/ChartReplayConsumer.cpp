#include "ChartReplayConsumer.h"

#include <utility>

namespace replay {
namespace {

ChartReplayConsumerOutcome failure(
    ChartReplayConsumerState state, std::string diagnostic,
    ChartReplayContextOutcome context = {}) {
  return {.state = state,
          .context = std::move(context),
          .diagnostic = std::move(diagnostic)};
}

} // namespace

ChartReplayConsumer::ChartReplayConsumer(
    ChartReplayConsumerDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

ChartReplayConsumerOutcome ChartReplayConsumer::load(
    const ModernChartResultRecord &listedRecord,
    const std::filesystem::path &selectedChartPath,
    std::atomic_bool &cancelled) const noexcept {
  try {
    std::string diagnostic;
    if (selectedChartPath.empty() || cancelled.load() ||
        !dependencies_.parseBaseChart || !dependencies_.loadContext ||
        !dependencies_.prepareChart || !dependencies_.materialize ||
        !result_persistence::validateModernChartResult(listedRecord.result,
                                                       diagnostic)) {
      return failure(ChartReplayConsumerState::InvalidRequest,
                     diagnostic.empty()
                         ? "Chart replay consumer request is incomplete."
                         : std::move(diagnostic));
    }

    const ReplayChartIdentity savedIdentity{
        .md5 = listedRecord.result.score.chartMd5,
        .sha256 = listedRecord.result.score.chartSha256,
        .keyMode = listedRecord.result.keyMode,
    };
    auto baseChart = dependencies_.parseBaseChart(
        selectedChartPath, savedIdentity,
        listedRecord.result.score.provenance, cancelled, diagnostic);
    if (baseChart == nullptr || cancelled.load()) {
      return failure(ChartReplayConsumerState::ChartUnavailable,
                     diagnostic.empty()
                         ? "The selected chart could not be parsed."
                         : std::move(diagnostic));
    }

    const auto expectedLongNoteMode =
        result_persistence::replaySetupLongNoteMode(listedRecord.result.score);
    if (!expectedLongNoteMode) {
      return failure(ChartReplayConsumerState::InvalidRequest,
                     "Saved result has no unique replay setup.");
    }
    const ParsedChartReplayFacts facts = makeParsedChartReplayFacts(
        baseChart->Meta, *expectedLongNoteMode);
    auto context = dependencies_.loadContext(listedRecord.result.attemptId,
                                             facts);
    if (!context.replayAvailable() || !context.verified.has_value()) {
      const std::string contextDiagnostic = context.diagnostic.empty()
                                                ? "Replay is unavailable."
                                                : context.diagnostic;
      return failure(ChartReplayConsumerState::ReplayUnavailable,
                     contextDiagnostic, std::move(context));
    }
    if (context.verified->result != listedRecord.result) {
      return failure(ChartReplayConsumerState::StaleResult,
                     "The selected result changed while loading its replay.",
                     std::move(context));
    }

    const VerifiedChartReplay &verified = *context.verified;
    auto preparedChart = dependencies_.prepareChart(
        selectedChartPath, verified.document.playback.setup,
        verified.result.score.provenance, baseChart->Meta, cancelled,
        diagnostic);
    if (preparedChart == nullptr || cancelled.load()) {
      return failure(ChartReplayConsumerState::SetupUnavailable,
                     diagnostic.empty()
                         ? "The replay setup could not be reproduced."
                         : std::move(diagnostic),
                     std::move(context));
    }

    const auto preparedFacts = makeParsedChartReplayFacts(
        preparedChart->Meta, *expectedLongNoteMode);
    if (compareReplayChartIdentity(preparedFacts.chart, savedIdentity) !=
            ReplayChartMatch::Match ||
        preparedFacts.longNoteMode != *expectedLongNoteMode) {
      return failure(ChartReplayConsumerState::PreparedChartMismatch,
                     "Prepared replay chart differs from the saved result.",
                     std::move(context));
    }

    auto materialized = dependencies_.materialize(
        verified.document, verified.result, *preparedChart);
    if (!materialized.playable()) {
      return failure(ChartReplayConsumerState::MaterializationFailed,
                     materialized.diagnostic.empty()
                         ? "Replay judging could not produce playback data."
                         : std::move(materialized.diagnostic),
                     std::move(context));
    }

    return {.state = ChartReplayConsumerState::Ready,
            .context = std::move(context),
            .chart = std::move(preparedChart),
            .replayData = std::move(materialized.replayData),
            .diagnostic = std::move(materialized.diagnostic)};
  } catch (...) {
    return failure(ChartReplayConsumerState::MaterializationFailed,
                   "Chart replay preparation failed.");
  }
}

} // namespace replay
