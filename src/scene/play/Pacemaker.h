#pragma once

#include "../../CoursePlaySession.h"
#include "../../ReplayData.h"
#include "../../ResultContracts.h"
#include "../../repositories/ScoreRepository.h"
#include "RhythmState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pacemaker {

inline constexpr const char *kTargetOff = "OFF";
inline constexpr const char *kTargetBest = "BEST";
inline constexpr const char *kTargetA = "A";
inline constexpr const char *kTargetAA = "AA";
inline constexpr const char *kTargetAAA = "AAA";
inline constexpr const char *kTargetMaxMinus = "MAX-";
inline constexpr const char *kTargetMax = "MAX";

inline constexpr std::array<const char *, 7> kSelectableTargets = {
    kTargetOff, kTargetBest, kTargetA, kTargetAA,
    kTargetAAA, kTargetMaxMinus, kTargetMax};

struct Target {
  bool enabled = false;
  std::string label;
  int finalScore = 0;
  int maxScore = 0;
  int totalNotes = 0;
  bool usesReplayProgression = false;
  std::vector<int> scoreAfterNotes;
};

struct Snapshot {
  bool enabled = false;
  std::string label;
  int currentScore = 0;
  int targetScore = 0;
  int finalTargetScore = 0;
  int maxScore = 0;
  int delta = 0;
  int playedNotes = 0;
  int totalNotes = 0;
  bool usesReplayProgression = false;
};

inline bool judgementCountsAsPlayedNote(Judgement judgement) {
  return judgement != None && judgement != Kpoor;
}

inline bool replayEventCountsAsPlayedNote(const ReplayEvent &event) {
  if (!judgementCountsAsPlayedNote(event.judgement)) {
    return false;
  }
  return event.action == ReplayEventAction::Press ||
         event.action == ReplayEventAction::MultiBad ||
         event.action == ReplayEventAction::Release ||
         event.action == ReplayEventAction::Miss;
}

// Replay export has no gameplay simulation to own its score state.  Reduce
// each accepted replay judgement exactly once so normal and course export
// expose the same pacemaker snapshot to gameplay skins.
inline void applyReplayEventToState(RhythmState &state,
                                    const ReplayEvent &event) {
  if (!replayEventCountsAsPlayedNote(event)) {
    return;
  }

  const JudgeResult judgeResult(event.judgement, event.diffMicros);
  state.judgeCount[event.judgement]++;
  if (judgeResult.isComboBreak()) {
    state.combo = 0;
    state.comboBreak++;
  } else if (event.judgement != Kpoor) {
    state.combo++;
    state.maxCombo = std::max(state.maxCombo, state.combo);
  }
  state.recordFastSlow(judgeResult);
  state.combo = event.combo;
  state.maxCombo = std::max(state.maxCombo, event.combo);
  state.gaugeType = event.gaugeType;
  state.currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state.gaugeValues.size())) {
    state.gaugeValues[gaugeIndex] = event.gauge;
  }
}

inline std::unordered_map<std::string, bms_parser::Note *>
buildReplayNoteLookup(bms_parser::Chart &chart) {
  std::unordered_map<std::string, bms_parser::Note *> lookup;
  for (const auto &measure : chart.Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (auto *note : timeline->Notes) {
        if (note != nullptr) {
          lookup[replay_note::key(note->Lane, timeline->Timing)] = note;
        }
      }
      for (auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          lookup[replay_note::key(note->Lane, timeline->Timing)] = note;
        }
      }
    }
  }
  return lookup;
}

inline bms_parser::Note *findReplayNote(
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it =
      lookup.find(replay_note::key(event.lane, event.noteTimeMicros));
  return it == lookup.end() ? nullptr : it->second;
}

inline bool replayEventCountsAsPlayedNote(
    bms_parser::Chart &chart,
    const std::unordered_map<std::string, bms_parser::Note *> &lookup,
    const ReplayEvent &event) {
  if (!replayEventCountsAsPlayedNote(event)) {
    return false;
  }
  if (event.action != ReplayEventAction::Press) {
    return true;
  }

  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  auto *note = findReplayNote(lookup, event);
  if (note == nullptr || !note->IsLongNote()) {
    return true;
  }

  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  return longNote->IsTail() || !recordedJudge.isNotePlayed() ||
         effectiveLongNoteIsCharge(longNote, chart);
}

inline std::string normalizeTargetId(std::string value) {
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(),
                               [](unsigned char ch) {
                                 return std::isspace(ch) != 0;
                               }));
  value.erase(std::find_if_not(value.rbegin(), value.rend(),
                               [](unsigned char ch) {
                                 return std::isspace(ch) != 0;
                               }).base(),
              value.end());
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ' || ch == '-') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  value.erase(std::unique(value.begin(), value.end(),
                          [](char lhs, char rhs) {
                            return lhs == '-' && rhs == '-';
                          }),
              value.end());
  if (value == "NONE" || value == "DISABLED") {
    return kTargetOff;
  }
  if (value == "HIGH-SCORE" || value == "HIGHSCORE" ||
      value == "PB" || value == "PERSONAL-BEST") {
    return kTargetBest;
  }
  if (value == "MAX" || value == "PERFECT") {
    return kTargetMax;
  }
  if (value == "MAX-" || value == "MAX-MINUS" || value == "MAXMINUS" ||
      value == "RATE-MAX-" || value == "RANK-MAX-") {
    return kTargetMaxMinus;
  }
  for (const char *target : kSelectableTargets) {
    if (value == target) {
      return target;
    }
  }
  return kTargetBest;
}

inline std::string displayTargetLabel(const std::string &targetId) {
  const std::string normalized = normalizeTargetId(targetId);
  if (normalized == kTargetMaxMinus) {
    return "MAX -";
  }
  return normalized;
}

inline bool targetIsGrade(const std::string &targetId) {
  const std::string normalized = normalizeTargetId(targetId);
  return normalized == kTargetA || normalized == kTargetAA ||
         normalized == kTargetAAA || normalized == kTargetMaxMinus ||
         normalized == kTargetMax;
}

inline int playedNotesForState(const RhythmState &state, int totalNotes) {
  int played = 0;
  for (const Judgement judgement : {PGreat, Great, Good, Bad, Poor}) {
    const auto it = state.judgeCount.find(judgement);
    if (it != state.judgeCount.end()) {
      played += it->second;
    }
  }
  return std::clamp(played, 0, std::max(0, totalNotes));
}

inline std::vector<int> buildReplayScoreProgression(const ReplayData &replay,
                                                    int totalNotes) {
  if (totalNotes <= 0) {
    return {};
  }

  std::vector<int> progression(static_cast<std::size_t>(totalNotes) + 1U, 0);
  int played = 0;
  for (const ReplayEvent &event : replay.events) {
    if (!replayEventCountsAsPlayedNote(event)) {
      continue;
    }
    ++played;
    if (played > totalNotes) {
      return {};
    }
    progression[static_cast<std::size_t>(played)] =
        std::max(0, event.score);
  }

  if (played != totalNotes) {
    return {};
  }
  return progression;
}

inline std::vector<int> buildReplayScoreProgression(bms_parser::Chart &chart,
                                                    const ReplayData &replay) {
  const int totalNotes = std::max(0, chart.Meta.TotalNotes);
  if (totalNotes <= 0) {
    return {};
  }

  const auto lookup = buildReplayNoteLookup(chart);
  std::vector<int> progression(static_cast<std::size_t>(totalNotes) + 1U, 0);
  int played = 0;
  for (const ReplayEvent &event : replay.events) {
    if (!replayEventCountsAsPlayedNote(chart, lookup, event)) {
      continue;
    }
    ++played;
    if (played > totalNotes) {
      return {};
    }
    progression[static_cast<std::size_t>(played)] =
        std::max(0, event.score);
  }

  if (played != totalNotes) {
    return {};
  }
  return progression;
}

inline Target targetFromBestSnapshot(const bms_parser::ChartMeta &meta,
                                     const ScoreBestSnapshot &best,
                                     const ReplayData *replay = nullptr) {
  (void)replay;
  const int totalNotes = std::max(0, meta.TotalNotes);
  const int fallbackMaxScore =
      result_contract::maximumScoreForNotes(totalNotes).value_or(0);
  Target target;
  target.enabled = totalNotes > 0 && best.score > 0;
  target.label = "BEST";
  target.finalScore = std::max(0, best.score);
  target.maxScore = best.maxScore > 0 ? best.maxScore : fallbackMaxScore;
  target.totalNotes = totalNotes;
  return target;
}

inline Target targetFromBestSnapshot(bms_parser::Chart &chart,
                                     const ScoreBestSnapshot &best,
                                     const ReplayData *replay = nullptr) {
  Target target = targetFromBestSnapshot(chart.Meta, best);
  if (!target.enabled || replay == nullptr || replay->finalScore != best.score) {
    return target;
  }

  std::vector<int> progression = buildReplayScoreProgression(chart, *replay);
  if (!progression.empty() && progression.back() == target.finalScore) {
    target.usesReplayProgression = true;
    target.scoreAfterNotes = std::move(progression);
  }
  return target;
}

struct RateTargetFraction {
  int numerator = 0;
  int denominator = 1;
};

inline std::optional<RateTargetFraction>
gradeTargetFraction(const std::string &targetId) {
  const std::string normalized = normalizeTargetId(targetId);
  if (normalized == kTargetA) {
    return RateTargetFraction{6, 9};
  }
  if (normalized == kTargetAA) {
    return RateTargetFraction{7, 9};
  }
  if (normalized == kTargetAAA) {
    return RateTargetFraction{8, 9};
  }
  if (normalized == kTargetMaxMinus) {
    return RateTargetFraction{26, 27};
  }
  if (normalized == kTargetMax) {
    return RateTargetFraction{9, 9};
  }
  return std::nullopt;
}

inline Target targetFromGrade(const bms_parser::ChartMeta &meta,
                              const std::string &targetId) {
  const int totalNotes = std::max(0, meta.TotalNotes);
  const auto maximumScore = result_contract::maximumScoreForNotes(totalNotes);
  const int maxScore = maximumScore.value_or(0);
  Target target;
  const std::string normalized = normalizeTargetId(targetId);
  target.label = displayTargetLabel(normalized);
  target.maxScore = maxScore;
  target.totalNotes = totalNotes;
  const std::optional<RateTargetFraction> fraction =
      gradeTargetFraction(normalized);
  target.enabled = totalNotes > 0 && maximumScore && fraction.has_value();
  if (target.enabled) {
    target.finalScore = static_cast<int>(
        std::ceil(static_cast<double>(maxScore) *
                  static_cast<double>(fraction->numerator) /
                  static_cast<double>(fraction->denominator)));
  }
  return target;
}

inline Target targetFromSelection(const bms_parser::ChartMeta &meta,
                                  const std::string &targetId,
                                  const std::optional<ScoreBestSnapshot> &best,
                                  const ReplayData *bestReplay = nullptr) {
  const std::string normalized = normalizeTargetId(targetId);
  if (normalized == kTargetOff) {
    return {};
  }
  if (targetIsGrade(normalized)) {
    return targetFromGrade(meta, normalized);
  }
  if (best.has_value()) {
    return targetFromBestSnapshot(meta, *best, bestReplay);
  }
  return {};
}

inline Target targetFromSelection(bms_parser::Chart &chart,
                                  const std::string &targetId,
                                  const std::optional<ScoreBestSnapshot> &best,
                                  const ReplayData *bestReplay = nullptr) {
  const std::string normalized = normalizeTargetId(targetId);
  if (normalized == kTargetOff) {
    return {};
  }
  if (targetIsGrade(normalized)) {
    return targetFromGrade(chart.Meta, normalized);
  }
  if (best.has_value()) {
    return targetFromBestSnapshot(chart, *best, bestReplay);
  }
  return {};
}

inline int targetScoreAtPlayedNotes(const Target &target, int playedNotes) {
  if (!target.enabled || target.totalNotes <= 0) {
    return 0;
  }

  const int clampedNotes =
      std::clamp(playedNotes, 0, std::max(0, target.totalNotes));
  if (target.usesReplayProgression &&
      target.scoreAfterNotes.size() ==
          static_cast<std::size_t>(target.totalNotes) + 1U) {
    return target.scoreAfterNotes[static_cast<std::size_t>(clampedNotes)];
  }

  const long long scaled =
      static_cast<long long>(target.finalScore) * clampedNotes /
      std::max(1, target.totalNotes);
  return static_cast<int>(std::clamp<long long>(scaled, 0, target.finalScore));
}

inline Snapshot snapshotForState(const Target &target,
                                 const RhythmState &state) {
  Snapshot snapshot;
  snapshot.enabled = target.enabled;
  snapshot.label = target.label;
  snapshot.currentScore = state.getScore();
  snapshot.finalTargetScore = target.finalScore;
  snapshot.maxScore = target.maxScore;
  snapshot.totalNotes = target.totalNotes;
  snapshot.usesReplayProgression = target.usesReplayProgression;
  snapshot.playedNotes = playedNotesForState(state, target.totalNotes);
  snapshot.targetScore = targetScoreAtPlayedNotes(target, snapshot.playedNotes);
  snapshot.delta = snapshot.currentScore - snapshot.targetScore;
  return snapshot;
}

} // namespace pacemaker
