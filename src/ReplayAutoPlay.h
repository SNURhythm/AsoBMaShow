#pragma once

#include "CoursePlaySession.h"
#include "ReplayDBHelper.h"

#include <algorithm>

namespace replay_autoplay {
inline constexpr int kReplayId = -1;
inline constexpr const char *kLabel = "AUTO PLAY";

inline bool isAutoPlayReplayId(int replayId) { return replayId == kReplayId; }

inline void applyAutoPlayJudge(RhythmState &state,
                               const JudgeResult &judgeResult) {
  state.judgeCount[judgeResult.judgement]++;
  if (judgeResult.isComboBreak()) {
    state.combo = 0;
    state.comboBreak++;
  } else if (judgeResult.judgement != Kpoor) {
    state.combo++;
    state.maxCombo = std::max(state.maxCombo, state.combo);
  }
  state.recordFastSlow(judgeResult);
  state.applyGaugeJudgement(judgeResult.judgement);
}

inline ReplayEvent makeAutoPlayEvent(ReplayEventAction action,
                                     const bms_parser::Note *note,
                                     long long songTimeMicros,
                                     const JudgeResult &judgeResult,
                                     const RhythmState &state) {
  ReplayEvent event;
  event.action = action;
  event.lane = note != nullptr ? note->Lane : -1;
  event.noteTimeMicros = note != nullptr && note->Timeline != nullptr
                             ? note->Timeline->Timing
                             : -1;
  event.songTimeMicros = songTimeMicros;
  event.judgeTimeMicros = songTimeMicros;
  event.judgement = judgeResult.judgement;
  event.diffMicros = judgeResult.Diff;
  event.gauge = state.currentGauge;
  event.gaugeType = state.gaugeType;
  event.combo = state.combo;
  event.score = state.getScore();
  return event;
}

inline ReplaySummary BuildSummary(
    const bms_parser::ChartMeta &meta, GaugeType gaugeType, bool gaugeAutoShift,
    const std::optional<std::string> &playOption = std::nullopt,
    const std::optional<long long> &playOptionSeed = std::nullopt,
    const std::optional<std::string> &playOption2 = std::nullopt,
    const std::optional<long long> &playOption2Seed = std::nullopt,
    const std::string &assistOption = assist_options::kOff) {
  ReplaySummary summary;
  summary.id = kReplayId;
  summary.autoPlay = true;
  summary.initialGaugeType = gaugeType;
  summary.gaugeAutoShift = gaugeAutoShift;
  summary.finalScore = std::max(0, meta.TotalNotes) * 2;
  summary.maxScore = summary.finalScore;
  summary.maxCombo = std::max(0, meta.TotalNotes);
  summary.finalGauge = 100.0f;
  summary.clearType = kClearTypeFullComboRank;
  summary.createdAt = kLabel;
  summary.eventCount = std::max(0, meta.TotalNotes);
  summary.touchSampleCount = 0;
  summary.chartMeta = meta;
  summary.playOption = playOption;
  summary.playOptionSeed = playOptionSeed;
  summary.playOption2 = playOption2;
  summary.playOption2Seed = playOption2Seed;
  summary.assistOption = assist_options::normalize(assistOption);
  return summary;
}

inline ReplayData BuildReplayData(
    bms_parser::Chart &chart, GaugeType gaugeType, bool gaugeAutoShift,
    audio::PlaybackRate playback = {},
    const std::optional<std::string> &playOption = std::nullopt,
    const std::optional<long long> &playOptionSeed = std::nullopt,
    const std::optional<std::string> &playOption2 = std::nullopt,
    const std::optional<long long> &playOption2Seed = std::nullopt,
    const std::string &assistOption = assist_options::kOff) {
  ReplayData replay;
  replay.id = kReplayId;
  replay.autoPlay = true;
  replay.chartMeta = chart.Meta;
  replay.randomSeed = chart.Meta.RandomSeed;
  replay.randomPrng = chart.Meta.RandomPrng;
  replay.randomValues = chart.Meta.RandomValues;
  replay.playOption = playOption;
  replay.playOptionSeed = playOptionSeed;
  replay.playOption2 = playOption2;
  replay.playOption2Seed = playOption2Seed;
  replay.assistOption = assist_options::normalize(assistOption);
  replay.initialGaugeType = gaugeType;
  replay.gaugeAutoShift = gaugeAutoShift;
  replay.provenance.playback = playback;
  replay.provenance.autoPlay = true;
  replay.createdAt = kLabel;
  replay.events.reserve(static_cast<size_t>(std::max(0, chart.Meta.TotalNotes)) *
                        2U);

  RhythmState state(&chart, false);
  state.configureGauge(gaugeType, gaugeAutoShift);
  state.setAssistClearMark(assist_options::isEnabled(replay.assistOption));
  const JudgeResult perfect(PGreat, 0);
  const JudgeResult noJudge(None, 0);

  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || note->IsLandmineNote()) {
          continue;
        }
        if (note->IsLongNote()) {
          auto *longNote = static_cast<bms_parser::LongNote *>(note);
          if (!longNote->IsTail()) {
            if (effectiveLongNoteIsCharge(longNote, chart)) {
              applyAutoPlayJudge(state, perfect);
            }
            replay.events.push_back(makeAutoPlayEvent(
                ReplayEventAction::Press, note, timeline->Timing, perfect,
                state));
          } else {
            applyAutoPlayJudge(state, perfect);
            replay.events.push_back(makeAutoPlayEvent(
                ReplayEventAction::Release, note, timeline->Timing, perfect,
                state));
          }
          continue;
        }

        applyAutoPlayJudge(state, perfect);
        replay.events.push_back(makeAutoPlayEvent(
            ReplayEventAction::Press, note, timeline->Timing, perfect, state));
        replay.events.push_back(makeAutoPlayEvent(
            ReplayEventAction::Release, note, timeline->Timing, noJudge,
            state));
      }
    }
  }

  replay.finalScore = state.getScore();
  replay.maxCombo = state.maxCombo;
  replay.finalGauge = state.currentGauge;
  replay.clearType = clear_policy::fullComboRankForPlayback(
      state.getClearTypeRank(), state.comboBreak == 0,
      replay.provenance.playback);
  return replay;
}
} // namespace replay_autoplay
