#pragma once

#include "CoursePlaySession.h"
#include "repositories/ReplayRepository.h"

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

inline float perfectPlayGauge(const bms_parser::ChartMeta &meta,
                              GaugeType gaugeType,
                              GaugeAutoShiftMode gaugeAutoShift,
                              GameplayRuleset ruleset) {
  bms_parser::Chart chart;
  chart.Meta = meta;
  RhythmState state(&chart, false, ruleset);
  state.configureGauge(gaugeType, gaugeAutoShift);
  for (int note = 0; note < std::max(0, meta.TotalNotes); ++note) {
    state.applyGaugeJudgement(PGreat);
  }
  return state.currentGauge;
}

inline ReplaySummary BuildSummary(
    const bms_parser::ChartMeta &meta, GaugeType gaugeType, GaugeAutoShiftMode gaugeAutoShift,
    const std::optional<std::string> &playOption = std::nullopt,
    const std::optional<long long> &playOptionSeed = std::nullopt,
    const std::optional<std::string> &playOption2 = std::nullopt,
    const std::optional<long long> &playOption2Seed = std::nullopt,
    const std::string &assistOption = assist_options::kOff,
    audio::PlaybackRate playback = {},
    GameplayRuleset ruleset = kDefaultGameplayRuleset) {
  ReplaySummary summary;
  summary.id = kReplayId;
  summary.autoPlay = true;
  summary.initialGaugeType = gaugeType;
  summary.gaugeAutoShift = gaugeAutoShift;
  summary.finalScore = std::max(0, meta.TotalNotes) * 2;
  summary.maxScore = summary.finalScore;
  summary.maxCombo = std::max(0, meta.TotalNotes);
  summary.finalGauge =
      perfectPlayGauge(meta, gaugeType, gaugeAutoShift, ruleset);
  summary.clearType = clear_policy::fullComboRankForPlayback(
      kClearTypeFullComboRank, true, playback);
  summary.createdAt = kLabel;
  summary.chartMeta = meta;
  summary.playOption = playOption;
  summary.playOptionSeed = playOptionSeed;
  summary.playOption2 = playOption2;
  summary.playOption2Seed = playOption2Seed;
  summary.assistOption = assist_options::normalize(assistOption);
  summary.playback = playback;
  return summary;
}

inline JudgedPlaybackData BuildReplayData(
    bms_parser::Chart &chart, GaugeType gaugeType,
    GaugeAutoShiftMode gaugeAutoShift, audio::PlaybackRate playback = {},
    const std::optional<std::string> &playOption = std::nullopt,
    const std::optional<long long> &playOptionSeed = std::nullopt,
    const std::optional<std::string> &playOption2 = std::nullopt,
    const std::optional<long long> &playOption2Seed = std::nullopt,
    const std::string &assistOption = assist_options::kOff,
    bool clubMode = false,
    GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
    GameplayRuleset ruleset = kDefaultGameplayRuleset,
    std::optional<bool> hasUndefinedLongNotes = std::nullopt) {
  JudgedPlaybackData replay;
  replay.autoPlay = true;
  replay.chartMeta = chart.Meta;
  const RulesetDescriptor rulesetDescriptor = RulesetDescriptor::For(ruleset);
  replay.context.ruleset = rulesetDescriptor;
  replay.createdAt = kLabel;
  replay.events.reserve(static_cast<size_t>(std::max(0, chart.Meta.TotalNotes)) *
                        2U);

  RhythmState state(&chart, false, ruleset);
  state.configureGauge(gaugeType, gaugeAutoShift, GaugeProfile::Standard,
                       gaugeAutoShiftLowerBound);
  ::replay::ChartPlaybackSetup setup{
      .chartMd5 = chart.Meta.MD5,
      .chartSha256 = chart.Meta.SHA256,
      .keyMode = chart.Meta.KeyMode,
      .longNoteMode = normalizeChartLongNoteModeValue(chart.Meta.LnMode),
      .hasUndefinedLongNotes = hasUndefinedLongNotes.value_or(
          ::replay::hasUndefinedLongNotesForReplay(
              chart.Meta.LnMode, chart.Meta.TotalLongNotes,
              chart.Meta.TotalBackSpinNotes)),
      .randomSeed = chart.Meta.RandomSeed,
      .randomPrng = chart.Meta.RandomPrng,
      .randomValues = chart.Meta.RandomValues,
      .playOption = playOption,
      .playOptionSeed = playOptionSeed,
      .playOption2 = playOption2,
      .playOption2Seed = playOption2Seed,
      .assistOption = assist_options::normalize(assistOption),
      .initialGaugeType = gaugeType,
      .gaugeProfile = state.gaugeProfile,
      .gaugeAutoShift = gaugeAutoShift,
      .gaugeAutoShiftLowerBound = gaugeAutoShiftLowerBound,
      .playbackRulesetId = rulesetDescriptor.id,
      .playbackRulesetRevision = rulesetDescriptor.version,
      .playbackRatePercent = playback.percent,
      .playbackMode = playback.mode,
      .startingGaugePercent = state.currentGauge,
      .startingGaugeState = state.gaugeSnapshot(),
      .clubMode = clubMode,
  };
  replay.setup = std::move(setup);
  state.setAssistClearMark(assist_options::isEnabled(replay.setup.assistOption));
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
      {.percent = replay.setup.playbackRatePercent,
       .mode = replay.setup.playbackMode});
  return replay;
}
} // namespace replay_autoplay
