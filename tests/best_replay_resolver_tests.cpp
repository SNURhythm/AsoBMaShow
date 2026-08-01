#include "replay/BestReplayResolver.h"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

void testLoadsTheExactBestAttemptThroughTheSharedConsumerBoundary() {
  bool resultLoaded = false;
  bool replayLoaded = false;
  replay::BestReplayResolver resolver({
      .loadResult = [&](std::string_view attemptId) {
        resultLoaded = true;
        assert(attemptId == "best-attempt");
        ModernChartResultRecord record;
        record.result.attemptId = std::string(attemptId);
        return ModernChartResultReadOutcome{
            .status = ModernChartResultReadStatus::Loaded,
            .record = std::move(record),
        };
      },
      .loadReplay = [&](const ModernChartResultRecord &record,
                        const std::filesystem::path &selectedChartPath,
                        std::atomic_bool &) {
        replayLoaded = true;
        assert(record.result.attemptId == "best-attempt");
        assert(selectedChartPath == "charts/song.bms");
        auto result = std::make_shared<ReplayData>();
        result->finalScore = 1234;
        return result;
      },
  });
  std::atomic_bool cancelled = false;

  const auto replay =
      resolver.load("best-attempt", "charts/song.bms", cancelled);

  assert(resultLoaded);
  assert(replayLoaded);
  assert(replay != nullptr && replay->finalScore == 1234);
}

void testUnavailableResultFallsBackWithoutLoadingReplay() {
  bool replayLoaded = false;
  replay::BestReplayResolver resolver({
      .loadResult = [](std::string_view) {
        return ModernChartResultReadOutcome{
            .status = ModernChartResultReadStatus::NotFound};
      },
      .loadReplay = [&](const ModernChartResultRecord &,
                        const std::filesystem::path &, std::atomic_bool &) {
        replayLoaded = true;
        return std::make_shared<ReplayData>();
      },
  });
  std::atomic_bool cancelled = false;

  assert(resolver.load("missing", "charts/song.bms", cancelled) == nullptr);
  assert(!replayLoaded);
}

void testEmptyAttemptAndConsumerFailureFallBackSafely() {
  int resultLoads = 0;
  replay::BestReplayResolver resolver({
      .loadResult = [&](std::string_view attemptId) {
        ++resultLoads;
        ModernChartResultRecord record;
        record.result.attemptId = std::string(attemptId);
        return ModernChartResultReadOutcome{
            .status = ModernChartResultReadStatus::Loaded,
            .record = std::move(record),
        };
      },
      .loadReplay = [](const ModernChartResultRecord &,
                       const std::filesystem::path &, std::atomic_bool &) {
        return std::shared_ptr<ReplayData>{};
      },
  });
  std::atomic_bool cancelled = false;

  assert(resolver.load("", "charts/song.bms", cancelled) == nullptr);
  assert(resultLoads == 0);
  assert(resolver.load("best-attempt", "charts/song.bms", cancelled) ==
         nullptr);
  assert(resultLoads == 1);
}

void testResultAttemptMismatchCannotSelectAnotherReplay() {
  bool replayLoaded = false;
  replay::BestReplayResolver resolver({
      .loadResult = [](std::string_view) {
        ModernChartResultRecord record;
        record.result.attemptId = "different-attempt";
        return ModernChartResultReadOutcome{
            .status = ModernChartResultReadStatus::Loaded,
            .record = std::move(record),
        };
      },
      .loadReplay = [&](const ModernChartResultRecord &,
                        const std::filesystem::path &, std::atomic_bool &) {
        replayLoaded = true;
        return std::make_shared<ReplayData>();
      },
  });
  std::atomic_bool cancelled = false;

  assert(resolver.load("best-attempt", "charts/song.bms", cancelled) ==
         nullptr);
  assert(!replayLoaded);
}

} // namespace

int main() {
  testLoadsTheExactBestAttemptThroughTheSharedConsumerBoundary();
  testUnavailableResultFallsBackWithoutLoadingReplay();
  testEmptyAttemptAndConsumerFailureFallBackSafely();
  testResultAttemptMismatchCannotSelectAnotherReplay();
  return 0;
}
