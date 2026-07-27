#include "ReplayPlaybackMaterializer.h"

#include <utility>

namespace replay {

ReplayPlaybackMaterializationOutcome ReplayPlaybackMaterializer::materialize(
    const ReplayChartDocument &document, ReplaySetupSource source,
    const result_persistence::ModernChartResult &savedResult,
    const ReplayJudgingSink &judge, std::size_t eventBudget) {
  if (!judge.advanceTo || !judge.applyInput || !judge.finish) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = "Replay judging sink is incomplete."};
  }

  ReplayPlaybackDriver driver(document, source);
  if (!driver.valid()) {
    return {.state = ReplayPlaybackMaterializationState::InvalidReplay,
            .diagnostic = driver.diagnostic()};
  }

  ReplayPlaybackSink sink;
  sink.inputBatch = [&](std::span<const InputTransition> events,
                        std::string &diagnostic) {
    if (events.empty() ||
        !judge.advanceTo(events.front().songTimeMicros, diagnostic)) {
      return false;
    }
    for (const auto &event : events) {
      if (!judge.applyInput(event, diagnostic)) {
        return false;
      }
    }
    return true;
  };
  const auto advanced = driver.advanceTo(
      document.timeBounds.completionSongTimeMicros, sink, eventBudget);
  if (advanced.state == ReplayPlaybackDriverState::WorkLimitExceeded) {
    return {.state = ReplayPlaybackMaterializationState::WorkLimitExceeded,
            .diagnostic = advanced.diagnostic};
  }
  if (!advanced.advanced() || !driver.complete()) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = advanced.diagnostic.empty()
                              ? "Replay driver did not reach completion."
                              : advanced.diagnostic};
  }

  std::string diagnostic;
  if (!judge.advanceTo(document.timeBounds.completionSongTimeMicros,
                       diagnostic)) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = std::move(diagnostic)};
  }
  const auto judged = judge.finish(diagnostic);
  if (!judged) {
    return {.state = ReplayPlaybackMaterializationState::JudgingFailed,
            .diagnostic = std::move(diagnostic)};
  }
  const auto agreement =
      result_persistence::compareModernChartResultFacts(savedResult, *judged);
  return {.state = agreement.agrees()
                       ? ReplayPlaybackMaterializationState::Matched
                       : ReplayPlaybackMaterializationState::ResultMismatch,
          .agreement = agreement,
          .diagnostic = agreement.diagnostic};
}

} // namespace replay
