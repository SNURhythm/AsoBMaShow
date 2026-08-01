#pragma once

#include "../ReplayData.h"
#include "../repositories/ReplayRepository.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace replay {

struct BestReplayResolverDependencies {
  std::function<ModernChartResultReadOutcome(std::string_view)> loadResult;
  std::function<std::shared_ptr<ReplayData>(
      const ModernChartResultRecord &, const std::filesystem::path &,
      std::atomic_bool &)>
      loadReplay;
};

// Joins a score-database BEST snapshot to its modern result and then delegates
// all BRD identity, setup, result-agreement, and materialization checks to the
// shared chart replay consumer.
class BestReplayResolver {
public:
  explicit BestReplayResolver(BestReplayResolverDependencies dependencies);

  [[nodiscard]] std::shared_ptr<ReplayData>
  load(std::string_view attemptId,
       const std::filesystem::path &selectedChartPath,
       std::atomic_bool &cancelled) const noexcept;

private:
  BestReplayResolverDependencies dependencies_;
};

[[nodiscard]] BestReplayResolver
makeRuntimeBestReplayResolver(ReplayRepository &repository);

} // namespace replay
