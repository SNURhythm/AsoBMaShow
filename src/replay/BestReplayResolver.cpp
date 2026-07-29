#include "BestReplayResolver.h"

#include <utility>

namespace replay {

BestReplayResolver::BestReplayResolver(
    BestReplayResolverDependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

std::shared_ptr<ReplayData> BestReplayResolver::load(
    std::string_view attemptId,
    const std::filesystem::path &selectedChartPath,
    std::atomic_bool &cancelled) const noexcept {
  if (attemptId.empty() || !dependencies_.loadResult ||
      !dependencies_.loadReplay) {
    return {};
  }
  try {
    const auto result = dependencies_.loadResult(attemptId);
    if (result.status != ModernChartResultReadStatus::Loaded ||
        !result.record.has_value() ||
        result.record->result.attemptId != attemptId) {
      return {};
    }
    return dependencies_.loadReplay(*result.record, selectedChartPath,
                                    cancelled);
  } catch (...) {
    return {};
  }
}

} // namespace replay
