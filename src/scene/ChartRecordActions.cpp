#include "ChartRecordActions.h"

#include "../replay/ChartReplayConsumer.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ReplayRepository.h"

#include <utility>

namespace chart_records {

PreparedChartResult prepareChartResult(
    ReplayRepository &repository, const ChartMetaRecord &record,
    std::string_view attemptId, std::atomic_bool &cancelled) {
  try {
    if (cancelled.load()) return {};
    const auto exact = repository.LoadModernChartResultByAttempt(attemptId);
    if (cancelled.load()) return {};
    if (exact.status != ModernChartResultReadStatus::Loaded || !exact.record) {
      return {.diagnostic = exact.diagnostic.empty()
                                ? "saved result was not found"
                                : exact.diagnostic};
    }

    auto consumer = replay::makeRuntimeChartReplayConsumer(repository);
    auto replayLoad = consumer.load(*exact.record, record.meta.BmsPath, cancelled);
    if (cancelled.load()) return {};

    std::shared_ptr<ReplayData> retryData;
    result_recall::ModernChartLoader preparedChartLoader;
    if (replayLoad.ready()) {
      retryData = std::move(replayLoad.replayData);
      auto preparedChart =
          std::make_shared<std::unique_ptr<bms_parser::Chart>>(
              std::move(replayLoad.chart));
      preparedChartLoader =
          [preparedChart](const std::filesystem::path &,
                          std::atomic_bool &) mutable {
            return std::move(*preparedChart);
          };
    }

    auto recalled = result_recall::BuildChartResult(
        exact.record->result, cancelled, record.meta.BmsPath,
        std::move(preparedChartLoader));
    if (cancelled.load()) return {};
    if (!recalled.value) {
      return {.diagnostic = recalled.diagnostic.empty()
                                ? "saved result was not found"
                                : recalled.diagnostic};
    }
    return {.completion =
                std::make_shared<ChartResultCompletion>(ChartResultCompletion{
                    .view = std::move(*recalled.value),
                    .retryData = std::move(retryData),
                })};
  } catch (...) {
    if (cancelled.load()) return {};
    return {.diagnostic = "saved chart result could not be recalled"};
  }
}

} // namespace chart_records
