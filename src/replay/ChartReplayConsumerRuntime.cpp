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
      .loadContext =
          [context](std::string_view attemptId) {
            return context->load(attemptId);
          },
      .prepareChart = [](const std::filesystem::path &path,
                         const ReplaySetup &setup,
                         const ScoreProvenance &provenance,
                         std::atomic_bool &cancelled,
                         std::string &diagnostic) {
        bms_parser::ChartMeta savedMeta;
        savedMeta.BmsPath = path;
        savedMeta.MD5 = setup.chart.md5;
        savedMeta.SHA256 = setup.chart.sha256;
        savedMeta.KeyMode = setup.chart.keyMode;
        savedMeta.LnMode = setup.longNoteMode;
        auto runtimeSetup =
            makeReplayDataFromSetup(setup, provenance, savedMeta, diagnostic);
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
