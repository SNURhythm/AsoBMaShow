#pragma once

#include "CourseReplayContext.h"
#include "CourseContinuation.h"
#include "ReplayPlaybackMaterializer.h"

#include "../ReplayData.h"
#include "../bms_parser.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct CoursePlaySession;

namespace replay {

enum class CourseReplayConsumerState {
  Ready,
  InvalidRequest,
  ChartUnavailable,
  ReplayUnavailable,
  StaleResult,
  SetupUnavailable,
  PreparedStageMismatch,
  MaterializationFailed,
  ContinuationFailed,
  ResultMismatch,
};

struct CourseReplayMaterializedStage {
  GaugeStateSnapshot initialGaugeState;
  GaugeStateSnapshot finalGaugeState;
  result_persistence::ModernChartResult judgedResult;
  int endingCombo = 0;
};

struct CourseReplayConsumerOutcome {
  CourseReplayConsumerState state = CourseReplayConsumerState::InvalidRequest;
  CourseReplayContextOutcome context;
  std::vector<std::unique_ptr<bms_parser::Chart>> charts;
  std::vector<CourseReplayMaterializedStage> materializedStages;
  std::shared_ptr<CourseReplayData> replayData;
  std::optional<CourseContinuationState> continuation;
  std::string diagnostic;

  [[nodiscard]] bool ready() const noexcept {
    return state == CourseReplayConsumerState::Ready && !charts.empty() &&
           charts.size() == materializedStages.size() &&
           replayData != nullptr && continuation.has_value();
  }
  [[nodiscard]] ReplayState replayState() const noexcept {
    const ReplayState state = context.replayState();
    return !ready() && state == ReplayState::Verified
               ? ReplayState::Mismatched
               : state;
  }
};

enum class CourseReplayLaunchMode {
  Watch,
  RetrySame,
};

struct CourseReplayConsumerDependencies {
  std::function<std::unique_ptr<bms_parser::Chart>(
      const std::filesystem::path &, const ReplayChartIdentity &,
      const ScoreProvenance &, std::atomic_bool &, std::string &)>
      parseBaseChart;
  std::function<CourseReplayContextOutcome(
      std::string_view, const ParsedCourseReplayFacts &)>
      loadContext;
  std::function<std::unique_ptr<bms_parser::Chart>(
      const std::filesystem::path &, const ReplaySetup &,
      const ScoreProvenance &, const bms_parser::ChartMeta &,
      std::atomic_bool &, std::string &)>
      prepareChart;
  std::function<ReplayPlaybackMaterializationOutcome(
      const ReplayChartDocument &, ReplaySetupSource,
      const result_persistence::ModernChartResult &,
      const bms_parser::Chart &, const ReplayPlaybackCarryState &)>
      materializeStage;
};

// The sole modern course replay preparation pipeline. The compatibility
// CourseReplayData is memory-only and is emitted after every stage and carried
// state transition is playable. Saved-result disagreement remains diagnostic.
class CourseReplayConsumer {
public:
  explicit CourseReplayConsumer(CourseReplayConsumerDependencies dependencies);

  [[nodiscard]] CourseReplayConsumerOutcome load(
      const ModernCourseResultRecord &listedRecord,
      const std::vector<std::filesystem::path> &completedChartPaths,
      std::atomic_bool &cancelled) const noexcept;

private:
  CourseReplayConsumerDependencies dependencies_;
};

[[nodiscard]] CourseReplayConsumer makeRuntimeCourseReplayConsumer(
    ReplayRepository &repository, ReplayLimits limits = kReplayLimits);

// Converts only a fully verified consumer outcome into the temporary scene
// adapter. Watch retains raw replay playback; Retry Same retains only the
// validated setup and already-prepared chart patterns for a new live attempt.
[[nodiscard]] std::shared_ptr<CoursePlaySession> makeCourseReplayLaunchSession(
    CourseReplayConsumerOutcome outcome, CourseReplayLaunchMode mode,
    bool renderTouchPoints = false, bool renderGhosts = false);

} // namespace replay
