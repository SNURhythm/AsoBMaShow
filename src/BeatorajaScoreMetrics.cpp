#include "BeatorajaScoreMetrics.h"

#include "CoursePlaySession.h"
#include "scene/play/Judge.h"

#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <utility>

std::optional<BeatorajaResultTimingStatistics>
beatorajaResultTimingStatistics(const ReplayData *replay, int totalNotes,
                                bms_parser::Chart *chart) {
  if (replay == nullptr || totalNotes <= 0) return std::nullopt;
  constexpr long long rangeMicros = 150'000;
  std::unordered_map<std::string, bms_parser::Note *> notes;
  std::unordered_set<const bms_parser::LongNote *> classicHeadsWithTailResult;
  if (chart != nullptr) {
    for (auto *measure : chart->Measures) {
      if (measure == nullptr) continue;
      for (auto *timeline : measure->TimeLines) {
        if (timeline == nullptr) continue;
        for (auto *note : timeline->Notes) {
          if (note != nullptr) {
            notes.emplace(replay_note::key(note->Lane, timeline->Timing), note);
          }
        }
      }
    }
    for (const auto &event : replay->events) {
      if (event.judgement == None ||
          (event.action != ReplayEventAction::Release &&
           event.action != ReplayEventAction::Miss)) {
        continue;
      }
      const auto found =
          notes.find(replay_note::key(event.lane, event.noteTimeMicros));
      if (found == notes.end() || !found->second->IsLongNote()) continue;
      const auto *tail =
          static_cast<const bms_parser::LongNote *>(found->second);
      if (tail->IsTail() && tail->Head != nullptr &&
          !effectiveLongNoteIsCharge(tail, chart)) {
        classicHeadsWithTailResult.insert(tail->Head);
      }
    }
  }
  const auto countsInResult = [&](const ReplayEvent &event) {
    if (chart == nullptr || event.action != ReplayEventAction::Press) {
      return true;
    }
    const auto found =
        notes.find(replay_note::key(event.lane, event.noteTimeMicros));
    if (found == notes.end() || !found->second->IsLongNote()) return true;
    const auto *longNote =
        static_cast<const bms_parser::LongNote *>(found->second);
    const JudgeResult judge(event.judgement, event.diffMicros);
    return longNote->IsTail() || !judge.isNotePlayed() ||
           effectiveLongNoteIsCharge(longNote, chart) ||
           (event.judgement == Bad &&
            !classicHeadsWithTailResult.contains(longNote));
  };
  std::vector<int> distribution(301, 0);
  long long durationMicros = 0;
  int playedNotes = 0;
  for (const auto &event : replay->events) {
    const bool scoreTimingEvent =
        (event.action == ReplayEventAction::Press ||
         event.action == ReplayEventAction::MultiBad ||
         event.action == ReplayEventAction::Release) &&
        (event.judgement == PGreat || event.judgement == Great ||
         event.judgement == Good || event.judgement == Bad);
    if (scoreTimingEvent && countsInResult(event) &&
        playedNotes < totalNotes) {
      durationMicros += std::llabs(event.diffMicros);
      ++playedNotes;
    }
    const bool timingEvent =
        (event.action == ReplayEventAction::Press ||
         event.action == ReplayEventAction::MultiBad ||
         event.action == ReplayEventAction::Release) &&
        event.judgement != None && event.judgement != Kpoor &&
        event.diffMicros >= -rangeMicros && event.diffMicros <= rangeMicros;
    const long long beatorajaDiffMicros = -event.diffMicros;
    if (timingEvent && countsInResult(event)) {
      const int millis = static_cast<int>(beatorajaDiffMicros / 1'000LL);
      if (millis >= -150 && millis <= 150) {
        ++distribution[static_cast<std::size_t>(millis + 150)];
      }
    }
  }
  int timingSampleCount = 0;
  long long timingSum = 0;
  for (std::size_t index = 0; index < distribution.size(); ++index) {
    const int count = distribution[index];
    const int timingMillis = static_cast<int>(index) - 150;
    timingSampleCount += count;
    timingSum += static_cast<long long>(count) * timingMillis;
  }
  const float timingAverage = timingSampleCount > 0
                                  ? static_cast<float>(timingSum) /
                                        timingSampleCount
                                  : 0.0F;
  float timingVariance = 0.0F;
  for (std::size_t index = 0; index < distribution.size(); ++index) {
    const int timingMillis = static_cast<int>(index) - 150;
    const float delta = timingMillis - timingAverage;
    timingVariance += distribution[index] * delta * delta;
  }
  return BeatorajaResultTimingStatistics{
      .hasTimingSamples = timingSampleCount > 0,
      .timingSampleCount = static_cast<std::size_t>(timingSampleCount),
      .averageMillis = timingAverage,
      .standardDeviationMillis = static_cast<double>(
          timingSampleCount == 0
              ? 0.0F
              : std::sqrt(timingVariance / timingSampleCount)),
      .averageJudgeMicros =
          (durationMicros + static_cast<long long>(totalNotes - playedNotes) *
                                1'000'000LL) /
          totalNotes,
      .distribution = std::move(distribution)};
}
