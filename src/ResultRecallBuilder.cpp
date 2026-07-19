#include "ResultRecallBuilder.h"

#include "PlayOptionUtils.h"
#include "ReplayResultStateBuilder.h"

#include <exception>
#include <utility>

namespace result_recall {
namespace {

ReplayChartLoader effectiveLoader(ReplayChartLoader loader) {
  if (loader) {
    return loader;
  }
  return [](const ReplayData &replay, std::atomic_bool &cancelled) {
    return play_options::prepareReplayChart(replay.chartMeta.BmsPath, replay,
                                            cancelled);
  };
}

std::optional<HistoricalIrContext>
historicalIrFor(const ReplayResultRecord &record,
                const bms_parser::ChartMeta &meta, const RhythmState &state) {
  if (!record.attemptId.has_value() ||
      !record.attemptFingerprint.has_value() ||
      record.attemptFingerprint->empty() || record.playedAtUnixMillis <= 0) {
    return std::nullopt;
  }

  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      *record.attemptId, meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  if (!attempt.has_value() ||
      attempt->payloadFingerprint != *record.attemptFingerprint) {
    return std::nullopt;
  }
  auto submission =
      ir::makeIrSubmission(*attempt, record.playedAtUnixMillis);
  if (!submission.value.has_value()) {
    return std::nullopt;
  }

  auto attemptPtr =
      std::make_shared<const result_persistence::ChartResultAttempt>(
          std::move(*attempt));
  auto submissionPtr = std::make_shared<const ir::IrSubmission>(
      std::move(*submission.value));
  result_persistence::SaveOutcome saved{
      .state = result_persistence::SaveState::Saved,
      .receipt = result_persistence::StageReceipt{
          .attemptId = attemptPtr->attemptId,
          .replayId = record.replay.id,
          .createdAt = record.replay.createdAt,
          .scorePending = false}};
  return HistoricalIrContext{.attempt = std::move(attemptPtr),
                             .submission = std::move(submissionPtr),
                             .saveOutcome = std::move(saved)};
}

} // namespace

ChartBuildOutcome BuildChartResult(ReplayResultRecord record,
                                   std::atomic_bool &cancelled,
                                   ReplayChartLoader loader) {
  try {
    auto loadChart = effectiveLoader(std::move(loader));
    auto chart = loadChart(record.replay, cancelled);
    if (chart == nullptr || cancelled.load()) {
      return {.diagnostic = "saved chart is unavailable"};
    }

    RhythmState state =
        replay_result::BuildResultState(*chart, record.replay);
    auto historicalIr = historicalIrFor(record, chart->Meta, state);
    return {.value = ChartResult{.chart = std::move(chart),
                                 .replay = std::move(record.replay),
                                 .state = std::move(state),
                                 .historicalIr = std::move(historicalIr)}};
  } catch (const std::exception &) {
    return {.diagnostic = "saved chart result could not be reconstructed"};
  } catch (...) {
    return {.diagnostic = "saved chart result could not be reconstructed"};
  }
}

} // namespace result_recall
