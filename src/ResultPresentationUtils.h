#pragma once

#include "ChartDBHelper.h"
#include "ReplayData.h"
#include "ScoreDBHelper.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace result_presentation {
inline ResultPreviousBestData
previousBestDataFromSnapshot(const ScoreBestSnapshot &snapshot) {
  return {.score = snapshot.score,
          .maxScore = snapshot.maxScore,
          .maxCombo = snapshot.maxCombo,
          .comboBreak = snapshot.comboBreak,
          .finalGauge = snapshot.finalGauge,
          .clearType = snapshot.clearType,
          .createdAt = snapshot.createdAt};
}

inline std::optional<ResultPreviousBestData>
previousBestForReplayChart(const bms_parser::ChartMeta &meta,
                           const ReplayData &replay) {
  std::optional<std::string> beforeCreatedAt;
  if (!replay.autoPlay && !replay.createdAt.empty()) {
    beforeCreatedAt = replay.createdAt;
  }
  if (const auto best =
          ScoreDBHelper::GetInstance().LoadBestScore(meta, beforeCreatedAt);
      best.has_value()) {
    return previousBestDataFromSnapshot(*best);
  }
  return std::nullopt;
}

inline std::string difficultyLabelForChart(
    const bms_parser::ChartMeta &meta) {
  return ChartDBHelper::GetInstance().DifficultyTableLabelsForChart(meta);
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
