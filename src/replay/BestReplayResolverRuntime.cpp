#include "BestReplayResolver.h"

#include "ChartReplayConsumer.h"

#include <memory>

namespace replay {

BestReplayResolver makeRuntimeBestReplayResolver(ReplayRepository &repository) {
  auto consumer = std::make_shared<ChartReplayConsumer>(
      makeRuntimeChartReplayConsumer(repository));
  return BestReplayResolver({
      .loadResult = [&repository](std::string_view attemptId) {
        return repository.LoadModernChartResultByAttempt(attemptId);
      },
      .loadReplay =
          [consumer](const ModernChartResultRecord &record,
                     const std::filesystem::path &selectedChartPath,
                     std::atomic_bool &cancelled) {
            auto outcome =
                consumer->load(record, selectedChartPath, cancelled);
            return outcome.ready() ? std::move(outcome.replayData)
                                   : std::shared_ptr<ReplayData>{};
          },
  });
}

} // namespace replay
