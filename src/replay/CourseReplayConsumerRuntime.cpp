#include "CourseReplayConsumer.h"

#include "ReplaySetupAdapter.h"

#include "../PlayOptionUtils.h"

#include <memory>
#include <utility>

namespace replay {

CourseReplayConsumer makeRuntimeCourseReplayConsumer(
    ReplayRepository &repository, ReplayLimits limits) {
  auto context = std::make_shared<CourseReplayContext>(repository, limits);
  return CourseReplayConsumer({
      .parseBaseChart = [](const std::filesystem::path &path,
                           std::atomic_bool &cancelled) {
        return play_options::parseChart(path, cancelled,
                                        "modern course replay identity");
      },
      .loadContext = [context](std::string_view attemptId,
                               const ParsedCourseReplayFacts &facts) {
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
        if (!runtimeSetup) {
          return std::unique_ptr<bms_parser::Chart>{};
        }
        auto prepared = play_options::prepareReplayChart(
            path, *runtimeSetup, cancelled);
        if (!prepared && diagnostic.empty()) {
          diagnostic = "The course replay stage setup could not be applied.";
        }
        return prepared;
      },
      .materializeStage = [](const ReplayChartDocument &document,
                             ReplaySetupSource source,
                             const result_persistence::ModernChartResult &result,
                             const bms_parser::Chart &chart,
                             const ReplayPlaybackCarryState &carry) {
        return ReplayPlaybackMaterializer::materializeForConsumers(
            document, source, result, chart, carry);
      },
  });
}

} // namespace replay
