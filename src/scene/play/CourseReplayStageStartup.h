#pragma once

#include "../../PlayOptionUtils.h"
#include "../../analysis/JudgedPlaybackAnalysis.h"
#include "GamePlayStartOptions.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>

namespace course_replay_startup {

struct PreparedStage {
  std::unique_ptr<bms_parser::Chart> chart;
  StartOptions options;
};

struct PrepareOutcome {
  std::optional<PreparedStage> value;
  std::string diagnostic;

  [[nodiscard]] bool prepared() const noexcept {
    return value.has_value() && value->chart != nullptr;
  }
};

// Selects the stage representation, parses its exact saved RANDOM branch, and
// derives the judged projection needed by ghosts and other presentation-only
// consumers. Native BRD input remains StartOptions' playback authority;
// migration-backed stages derive their judged compatibility adapter here.
[[nodiscard]] inline PrepareOutcome
prepareCurrentStage(const std::shared_ptr<CoursePlaySession> &session,
                    AppSettings::NotePriorityMode notePriorityMode,
                    std::atomic_bool &cancelled) {
  if (session == nullptr ||
      !session->hasCourseReplayStage(session->currentIndex)) {
    return {.diagnostic = "Course replay stage is unavailable"};
  }

  if (auto stagePlayback = session->currentCourseReplayStagePlayback()) {
    const auto *stageMeta = session->currentMeta();
    if (stageMeta == nullptr || stageMeta->BmsPath.empty()) {
      return {.diagnostic = "Course replay chart is unavailable"};
    }
    if (session->courseReplayResultContext == nullptr) {
      return {.diagnostic = "Course replay result context is unavailable"};
    }
    const auto stageResult = result_persistence::chartResultForCourseStage(
        *session->courseReplayResultContext, session->currentIndex);
    if (!stageResult.has_value()) {
      return {.diagnostic = "Course replay stage result is unavailable"};
    }

    auto chart = play_options::prepareReplayChart(stageMeta->BmsPath,
                                                  *stagePlayback, cancelled);
    if (chart == nullptr || cancelled) {
      return {.diagnostic = "Course replay chart could not be prepared"};
    }
    auto analysis = replay::makeJudgedPlaybackForAnalysis(
        *stagePlayback, *stageResult, *chart,
        {
            .notePriorityMode = notePriorityMode,
            .courseConstraints = session->constraints,
            .materializationSeed =
                {
                    .carriedGauge = session->carriedGauge,
                    .carriedCombo = session->carriedCombo,
                    .carriedMaxCombo = session->maxCombo,
                },
        });
    if (!analysis.has_value()) {
      return {.diagnostic = "Course replay input does not match its result"};
    }

    if (stagePlayback->legacy.has_value()) {
      auto legacyReplay =
          std::make_shared<JudgedPlaybackData>(std::move(*analysis));
      session->applyReplayStageSetup(*legacyReplay);
      return {.value = PreparedStage{
                  .chart = std::move(chart),
                  .options =
                      makeCourseReplayStageStartOptions(session, legacyReplay),
              }};
    }

    session->applyReplayStageSetup(*stagePlayback);
    auto sharedAnalysis =
        std::make_shared<const JudgedPlaybackData>(std::move(*analysis));
    return {.value = PreparedStage{
                .chart = std::move(chart),
                .options = makeCourseReplayStageStartOptions(
                    session, stagePlayback, std::move(sharedAnalysis)),
            }};
  }

  auto stageReplay = session->currentCourseReplayStageReplay();
  if (stageReplay == nullptr || stageReplay->chartMeta.BmsPath.empty()) {
    return {.diagnostic = "Migrated course replay stage is unavailable"};
  }
  auto chart = play_options::prepareReplayChart(stageReplay->chartMeta.BmsPath,
                                                *stageReplay, cancelled);
  if (chart == nullptr || cancelled) {
    return {.diagnostic = "Migrated course replay chart could not be prepared"};
  }

  session->applyReplayStageSetup(*stageReplay);
  return {
      .value = PreparedStage{
          .chart = std::move(chart),
          .options = makeCourseReplayStageStartOptions(session, stageReplay),
      }};
}

} // namespace course_replay_startup
