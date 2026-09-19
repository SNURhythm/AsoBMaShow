#pragma once

#include "../ModernResultRecallBuilder.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

struct ChartMetaRecord;
struct ReplayData;
class ReplayRepository;

namespace chart_records {

struct ChartResultCompletion {
  result_recall::ModernChartResultView view;
  std::shared_ptr<ReplayData> retryData;
};

struct PreparedChartResult {
  std::shared_ptr<ChartResultCompletion> completion;
  // Cancellation returns no completion and no diagnostic.
  std::string diagnostic;
};

// Reads the exact saved attempt, retaining the prepared replay chart when one
// is available. Callers retain responsibility for preview cleanup and scenes.
[[nodiscard]] PreparedChartResult prepareChartResult(
    ReplayRepository &repository, const ChartMetaRecord &record,
    std::string_view attemptId, std::atomic_bool &cancelled);

} // namespace chart_records
