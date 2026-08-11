#pragma once

#include "repositories/ChartRepository.h"
#include "ReplayData.h"
#include "replay/BestReplayResolver.h"
#include "repositories/ScoreRepository.h"
#include "scene/play/Pacemaker.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace result_presentation {
inline std::optional<ResultPreviousBestData>
previousBestDataFromSnapshot(const ScoreBestSnapshot &snapshot) {
  if (snapshot.source != ScoreBestSource::Local ||
      !snapshot.maxCombo.has_value() || !snapshot.comboBreak.has_value() ||
      !snapshot.finalGauge.has_value() || !snapshot.createdAt.has_value()) {
    return std::nullopt;
  }
  return ResultPreviousBestData{
      .score = snapshot.score,
      .maxScore = snapshot.maxScore,
      .maxCombo = *snapshot.maxCombo,
      .comboBreak = *snapshot.comboBreak,
      .finalGauge = *snapshot.finalGauge,
      .clearType = snapshot.clearType,
      .createdAt = *snapshot.createdAt,
      .attemptId = snapshot.attemptId,
  };
}

inline ScoreBestSnapshot
scoreBestSnapshotFromPreviousBest(const ResultPreviousBestData &previousBest) {
  return {.score = previousBest.score,
          .maxScore = previousBest.maxScore,
          .maxCombo = previousBest.maxCombo,
          .comboBreak = previousBest.comboBreak,
          .finalGauge = previousBest.finalGauge,
          .clearType = previousBest.clearType,
          .createdAt = previousBest.createdAt,
          .attemptId = previousBest.attemptId};
}

inline std::shared_ptr<ReplayData> replayForBestSnapshotChart(
    ApplicationContext &context, const bms_parser::ChartMeta &meta,
    const std::optional<ScoreBestSnapshot> &best, const std::string &targetId,
    std::atomic_bool &cancelled) {
  if (pacemaker::normalizeTargetId(targetId) != pacemaker::kTargetBest ||
      !best.has_value() || !best->attemptId.has_value()) {
    return {};
  }
  auto resolver =
      replay::makeRuntimeBestReplayResolver(context.replayRepository);
  return resolver.load(*best->attemptId, meta.BmsPath, cancelled);
}

inline std::shared_ptr<ReplayData> replayForPreviousBestChart(
    ApplicationContext &context, const bms_parser::ChartMeta &meta,
    const std::optional<ResultPreviousBestData> &previousBest,
    const std::string &targetId,
    std::atomic_bool &cancelled) {
  std::optional<ScoreBestSnapshot> best;
  if (previousBest.has_value()) {
    best = scoreBestSnapshotFromPreviousBest(*previousBest);
  }
  return replayForBestSnapshotChart(context, meta, best, targetId, cancelled);
}

inline std::optional<ResultPacemakerData> pacemakerDataForResult(
    const bms_parser::ChartMeta &meta, const RhythmState &state,
    const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const ReplayData *bestReplay = nullptr) {
  const std::string normalized = pacemaker::normalizeTargetId(targetId);
  if (normalized == pacemaker::kTargetOff) {
    return std::nullopt;
  }

  std::optional<ScoreBestSnapshot> best;
  if (previousBest.has_value()) {
    best = scoreBestSnapshotFromPreviousBest(*previousBest);
  }

  const pacemaker::Target target =
      pacemaker::targetFromSelection(meta, normalized, best, bestReplay);
  if (!target.enabled) {
    return std::nullopt;
  }

  return ResultPacemakerData{
      .label = target.label,
      .targetScore = target.finalScore,
      .delta = state.getScore() - target.finalScore,
      .usesReplayProgression = target.usesReplayProgression,
  };
}

inline pacemaker::Target pacemakerTargetForReplay(
    bms_parser::Chart &chart, const ReplayData &replay,
    const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const ReplayData *bestReplay) {
  const std::string normalized = pacemaker::normalizeTargetId(targetId);
  if (replay.autoPlay || normalized == pacemaker::kTargetOff) {
    return {};
  }

  std::optional<ScoreBestSnapshot> best;
  if (previousBest.has_value()) {
    best = scoreBestSnapshotFromPreviousBest(*previousBest);
  }
  return pacemaker::targetFromSelection(chart, normalized, best, bestReplay);
}

// ScoreDataProperty's personal-best channel is independent from the selected
// pacemaker target. BMSPlayer always initializes it from the saved score and
// decoded best ghost, including when the selected target is OFF or a grade.
inline pacemaker::Target bestScoreTargetForReplay(
    bms_parser::Chart &chart, const ReplayData &replay,
    const ResultPreviousBestData &previousBest,
    const ReplayData *bestReplay = nullptr) {
  if (replay.autoPlay) {
    return {};
  }
  return pacemaker::targetFromBestSnapshot(
      chart, scoreBestSnapshotFromPreviousBest(previousBest), bestReplay);
}

inline std::optional<ResultPacemakerData> pacemakerDataForReplayResult(
    bms_parser::Chart &chart, const RhythmState &state,
    const ReplayData &replay, const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const ReplayData *bestReplay) {
  const pacemaker::Target target =
      pacemakerTargetForReplay(chart, replay, targetId, previousBest,
                              bestReplay);
  if (!target.enabled) {
    return std::nullopt;
  }

  return ResultPacemakerData{
      .label = target.label,
      .targetScore = target.finalScore,
      .delta = state.getScore() - target.finalScore,
      .usesReplayProgression = target.usesReplayProgression,
  };
}

inline std::optional<ResultPreviousBestData>
previousBestForReplayChart(ScoreRepository &scores,
                           const bms_parser::ChartMeta &meta,
                           const ReplayData &replay) {
  std::optional<std::string> beforeCreatedAt;
  if (!replay.autoPlay && !replay.createdAt.empty()) {
    beforeCreatedAt = replay.createdAt;
  }
  if (const auto best = scores.LoadBestScore(meta, beforeCreatedAt);
      best.has_value()) {
    return previousBestDataFromSnapshot(*best);
  }
  return std::nullopt;
}

inline std::string difficultyLabelForChart(
    ChartRepository &charts, const bms_parser::ChartMeta &meta) {
  auto session = charts.OpenSession();
  return session.has_value()
             ? session->DifficultyTableLabelsForChart(meta)
             : std::string{};
}

inline bms_parser::ChartMeta courseResultMeta(
    const std::string &courseName, const std::string &courseGroupName,
    std::size_t chartCount, int totalNotes, long long playLength) {
  bms_parser::ChartMeta meta;
  meta.Title = courseName.empty() ? "Course Result" : courseName;
  meta.Artist = courseGroupName.empty() ? "Course Mode" : courseGroupName;
  meta.TotalNotes = totalNotes;
  meta.PlayLevel = static_cast<double>(chartCount);
  meta.PlayLength = playLength;
  meta.TotalLength = meta.PlayLength;
  meta.Bpm = 0.0;
  meta.MinBpm = 0.0;
  meta.MaxBpm = 0.0;
  return meta;
}

inline bool isFullComboCourseResult(int completedCharts, int totalCharts,
                                    std::size_t resultStageCount,
                                    const RhythmState &state,
                                    const bms_parser::ChartMeta &courseMeta) {
  if (completedCharts < 0 || totalCharts < 0) {
    return false;
  }
  return completedCharts == totalCharts &&
         static_cast<std::size_t>(totalCharts) == resultStageCount &&
         state.currentGauge > 0.0f && state.comboBreak == 0 &&
         state.maxCombo >= std::max(0, courseMeta.TotalNotes);
}
} // namespace result_presentation
