#pragma once

#include "ChartReplayContext.h"
#include "ReplayPlaybackMaterializer.h"

#include "../ReplayData.h"
#include "../bms_parser.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace replay {

enum class ChartReplayConsumerState {
  Ready,
  InvalidRequest,
  ChartUnavailable,
  ReplayUnavailable,
  StaleResult,
  SetupUnavailable,
  PreparedChartMismatch,
  MaterializationFailed,
  ResultMismatch,
};

struct ChartReplayConsumerOutcome {
  ChartReplayConsumerState state = ChartReplayConsumerState::InvalidRequest;
  ChartReplayContextOutcome context;
  std::unique_ptr<bms_parser::Chart> chart;
  std::shared_ptr<ReplayData> replayData;
  std::string diagnostic;

  [[nodiscard]] bool ready() const noexcept {
    return state == ChartReplayConsumerState::Ready && chart != nullptr &&
           replayData != nullptr;
  }
  [[nodiscard]] ReplayState replayState() const noexcept {
    return context.replayState();
  }
};

struct ChartReplayConsumerDependencies {
  std::function<std::unique_ptr<bms_parser::Chart>(
      const std::filesystem::path &, const ReplayChartIdentity &,
      const ScoreProvenance &, std::atomic_bool &, std::string &)>
      parseBaseChart;
  std::function<ChartReplayContextOutcome(
      std::string_view, const ParsedChartReplayFacts &)>
      loadContext;
  std::function<std::unique_ptr<bms_parser::Chart>(
      const std::filesystem::path &, const ReplaySetup &,
      const ScoreProvenance &, const bms_parser::ChartMeta &,
      std::atomic_bool &, std::string &)>
      prepareChart;
  std::function<ReplayPlaybackMaterializationOutcome(
      const ReplayChartDocument &, ReplaySetupSource,
      const result_persistence::ModernChartResult &,
      const bms_parser::Chart &)>
      materialize;
};

// The sole modern chart replay preparation pipeline. Consumers receive an
// in-memory compatibility track only after selected-chart identity, BRD setup,
// strict result agreement, and replay judging all succeed.
class ChartReplayConsumer {
public:
  explicit ChartReplayConsumer(ChartReplayConsumerDependencies dependencies);

  [[nodiscard]] ChartReplayConsumerOutcome
  load(const ModernChartResultRecord &listedRecord,
       const std::filesystem::path &selectedChartPath,
       std::atomic_bool &cancelled) const noexcept;

private:
  ChartReplayConsumerDependencies dependencies_;
};

[[nodiscard]] ChartReplayConsumer makeRuntimeChartReplayConsumer(
    ReplayRepository &repository,
    ReplayLimits limits = kReplayLimits);

} // namespace replay
