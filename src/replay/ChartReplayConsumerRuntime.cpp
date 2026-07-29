#include "ChartReplayConsumer.h"

#include "ReplaySetupAdapter.h"

#include "../PlayOptionUtils.h"

#include <memory>
#include <utility>

namespace replay {

ChartReplayConsumer makeRuntimeChartReplayConsumer(
    ReplayRepository &repository, ReplayLimits limits) {
  auto context = std::make_shared<ChartReplayContext>(
      repository, limits);
  return ChartReplayConsumer({
      .parseBaseChart = [](const std::filesystem::path &path,
                           const ReplayChartIdentity &identity,
                           const ScoreProvenance &provenance,
                           std::atomic_bool &cancelled,
                           std::string &diagnostic) {
        bms_parser::ChartMeta savedChart;
        savedChart.MD5 = identity.md5;
        savedChart.SHA256 = identity.sha256;
        const auto setup = score_provenance::savedChartRandomParseSetup(
            provenance, savedChart, diagnostic);
        if (!setup) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        return play_options::parseChart(
            path, setup->randomSeed, setup->randomPrng, setup->randomValues,
            cancelled, "modern replay identity");
      },
      .loadContext =
          [context](std::string_view attemptId,
                    const ParsedChartReplayFacts &facts) {
            return context->load(attemptId, facts);
          },
      .prepareChart = [](const std::filesystem::path &path,
                         const ReplaySetup &setup,
                         const ScoreProvenance &provenance,
                         const bms_parser::ChartMeta &parsedMeta,
                         std::atomic_bool &cancelled,
                         std::string &diagnostic) {
        auto runtimeSetup =
            makeReplayDataFromSetup(setup, provenance, parsedMeta, diagnostic);
        if (!runtimeSetup.has_value()) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        auto prepared = play_options::prepareReplayChart(
            path, *runtimeSetup, cancelled);
        if (prepared == nullptr && diagnostic.empty()) {
          diagnostic = "The replay chart setup could not be applied.";
        }
        return prepared;
      },
      .materialize = [](const ReplayChartDocument &document,
                        const result_persistence::ModernChartResult &result,
                        const bms_parser::Chart &chart) {
        return ReplayPlaybackMaterializer::materializeForConsumers(
            document, result, chart);
      },
  });
}

} // namespace replay
