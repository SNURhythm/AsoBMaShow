#pragma once

#include "repositories/ChartRepository.h"
#include "repositories/ReplayRepository.h"
#include "analysis/JudgedPlaybackData.h"
#include "repositories/ScoreRepository.h"
#include "scene/play/Pacemaker.h"
#include "scene/play/ReplayResultContext.h"
#include "analysis/JudgedPlaybackAnalysis.h"
#include "skin/SkinTypes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace result_presentation {
inline constexpr std::size_t kMaximumSynthesizedCourseGaugeSamples = 1'000'000U;
inline constexpr std::int64_t kCourseGaugeSampleIntervalMicros = 500'000;

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
          .createdAt = previousBest.createdAt};
}

inline std::optional<JudgedPlaybackData>
bestReplayForSnapshot(ReplayRepository &replays, bms_parser::Chart &chart,
                      const ScoreBestSnapshot &best,
                      const ReplayComparisonQuery &comparison = {}) {
  if (best.score <= 0 || chart.Meta.TotalNotes <= 0) {
    return std::nullopt;
  }

  const auto summaries = replays.ListReplays(chart.Meta, 100);
  for (const ReplaySummary &summary : summaries) {
    if (summary.courseReplay || summary.autoPlay ||
        summary.finalScore != best.score) {
      continue;
    }
    if (comparison.beforeCreatedAt.has_value() &&
        !comparison.beforeCreatedAt->empty() && !summary.createdAt.empty() &&
        summary.createdAt >= *comparison.beforeCreatedAt) {
      continue;
    }
    if (comparison.excludeAttemptId.has_value() &&
        summary.attemptId == comparison.excludeAttemptId) {
      continue;
    }

    auto loadedReplay =
        replay::loadJudgedPlaybackForAnalysis(replays, summary.id, chart.Meta);
    if (!loadedReplay.has_value() || loadedReplay->finalScore != best.score) {
      continue;
    }

    const std::vector<int> progression =
        pacemaker::buildReplayScoreProgression(chart, *loadedReplay);
    if (!progression.empty() && progression.back() == best.score) {
      return loadedReplay;
    }
  }
  return std::nullopt;
}

inline std::optional<ResultPacemakerData> pacemakerDataForResult(
    const bms_parser::ChartMeta &meta, const RhythmState &state,
    const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const JudgedPlaybackData *bestReplay = nullptr) {
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
    ReplayRepository &replays, bms_parser::Chart &chart,
    const JudgedPlaybackData &replay, const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const ReplayComparisonQuery &comparison = {}) {
  const std::string normalized = pacemaker::normalizeTargetId(targetId);
  if (replay.autoPlay || normalized == pacemaker::kTargetOff) {
    return {};
  }

  std::optional<ScoreBestSnapshot> best;
  std::optional<JudgedPlaybackData> bestReplay;
  if (previousBest.has_value()) {
    best = scoreBestSnapshotFromPreviousBest(*previousBest);
  }

  if (normalized == pacemaker::kTargetBest && best.has_value()) {
    bestReplay = bestReplayForSnapshot(replays, chart, *best, comparison);
  }

  return pacemaker::targetFromSelection(
      chart, normalized, best, bestReplay.has_value() ? &*bestReplay : nullptr);
}

inline std::optional<ResultPacemakerData> pacemakerDataForReplayResult(
    ReplayRepository &replays, bms_parser::Chart &chart,
    const RhythmState &state, const JudgedPlaybackData &replay,
    const std::string &targetId,
    const std::optional<ResultPreviousBestData> &previousBest,
    const ReplayComparisonQuery &comparison = {}) {
  const pacemaker::Target target = pacemakerTargetForReplay(
      replays, chart, replay, targetId, previousBest, comparison);
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
                           const ReplayComparisonQuery &comparison = {}) {
  if (const auto best = scores.LoadBestScore(meta, comparison.beforeCreatedAt,
                                             comparison.excludeAttemptId);
      best.has_value()) {
    return previousBestDataFromSnapshot(*best);
  }
  return std::nullopt;
}

inline std::string difficultyLabelForChart(ChartRepository &charts,
                                           const bms_parser::ChartMeta &meta) {
  auto session = charts.OpenSession();
  return session.has_value() ? session->DifficultyTableLabelsForChart(meta)
                             : std::string{};
}

inline bms_parser::ChartMeta
courseResultMeta(const std::string &courseName,
                 const std::string &courseGroupName, std::size_t chartCount,
                 int totalNotes, long long playLength) {
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

inline bms_parser::ChartMeta courseResultMetaFromEntryFacts(
    const std::string &courseName, const std::string &courseGroupName,
    std::span<const analysis::JudgedCourseEntryFacts> entryFacts) {
  std::int64_t totalNotes = 0;
  std::int64_t playLengthMicros = 0;
  for (const auto &facts : entryFacts) {
    const std::int64_t notes = std::max<std::int64_t>(0, facts.totalNotes);
    totalNotes = std::min<std::int64_t>(std::numeric_limits<int>::max(),
                                        totalNotes + notes);

    const std::int64_t duration =
        std::max<std::int64_t>(0, facts.playLengthMicros);
    if (duration >
        std::numeric_limits<std::int64_t>::max() - playLengthMicros) {
      playLengthMicros = std::numeric_limits<std::int64_t>::max();
    } else {
      playLengthMicros += duration;
    }
  }
  return courseResultMeta(courseName, courseGroupName, entryFacts.size(),
                          static_cast<int>(totalNotes), playLengthMicros);
}

inline bms_parser::ChartMeta courseResultMetaFromReplayFacts(
    const std::string &courseName, const std::string &courseGroupName,
    std::span<const analysis::JudgedCourseEntryFacts> completeEntryFacts,
    std::span<const analysis::JudgedCourseEntryFacts> playedEntryFacts) {
  return courseResultMetaFromEntryFacts(
      courseName, courseGroupName,
      completeEntryFacts.empty() ? playedEntryFacts : completeEntryFacts);
}

inline std::size_t missingCourseGaugeSampleCount(
    std::span<const analysis::JudgedCourseEntryFacts> entryFacts,
    std::size_t startIndex) noexcept {
  std::size_t total = 0;
  for (std::size_t index = std::min(startIndex, entryFacts.size());
       index < entryFacts.size() &&
       total < kMaximumSynthesizedCourseGaugeSamples;
       ++index) {
    const std::int64_t duration =
        std::max<std::int64_t>(0, entryFacts[index].playLengthMicros);
    const std::uint64_t samples =
        static_cast<std::uint64_t>(duration /
                                   kCourseGaugeSampleIntervalMicros) +
        1U;
    const std::size_t remaining = kMaximumSynthesizedCourseGaugeSamples - total;
    total += samples >= static_cast<std::uint64_t>(remaining)
                 ? remaining
                 : static_cast<std::size_t>(samples);
  }
  return total;
}

inline void appendCourseGaugeHistorySamples(std::vector<float> &destination,
                                            std::span<const float> samples) {
  if (destination.size() >= kMaximumSynthesizedCourseGaugeSamples) {
    return;
  }
  const std::size_t count =
      std::min(samples.size(),
               kMaximumSynthesizedCourseGaugeSamples - destination.size());
  destination.insert(destination.end(), samples.begin(),
                     samples.begin() + static_cast<std::ptrdiff_t>(count));
}

inline void appendMissingCourseGaugeHistorySamples(
    std::vector<float> &destination,
    std::span<const analysis::JudgedCourseEntryFacts> entryFacts,
    std::size_t startIndex) {
  if (destination.size() >= kMaximumSynthesizedCourseGaugeSamples) {
    return;
  }
  const std::size_t count =
      std::min(missingCourseGaugeSampleCount(entryFacts, startIndex),
               kMaximumSynthesizedCourseGaugeSamples - destination.size());
  destination.insert(destination.end(), count, 0.0F);
}

inline int saturatingCourseCounterSum(int left, int right) noexcept {
  return static_cast<int>(std::clamp<std::int64_t>(
      static_cast<std::int64_t>(left) + right, std::numeric_limits<int>::min(),
      std::numeric_limits<int>::max()));
}

inline void appendCourseResultCounters(RhythmState &destination,
                                       const RhythmState &stage) {
  for (int index = 0; index < JudgementCount; ++index) {
    const auto judgement = static_cast<Judgement>(index);
    if (const auto count = stage.judgeCount.find(judgement);
        count != stage.judgeCount.end()) {
      destination.judgeCount[judgement] = saturatingCourseCounterSum(
          destination.judgeCount[judgement], count->second);
    }
    if (const auto timing = stage.judgementFastSlowCount.find(judgement);
        timing != stage.judgementFastSlowCount.end()) {
      auto &aggregate = destination.judgementFastSlowCount[judgement];
      aggregate.fast =
          saturatingCourseCounterSum(aggregate.fast, timing->second.fast);
      aggregate.slow =
          saturatingCourseCounterSum(aggregate.slow, timing->second.slow);
    }
  }
  destination.comboBreak =
      saturatingCourseCounterSum(destination.comboBreak, stage.comboBreak);
  destination.fastCount =
      saturatingCourseCounterSum(destination.fastCount, stage.fastCount);
  destination.slowCount =
      saturatingCourseCounterSum(destination.slowCount, stage.slowCount);
  destination.maxCombo = std::max(destination.maxCombo, stage.maxCombo);
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
