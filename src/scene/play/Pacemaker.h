#pragma once

#include "../../ReplayData.h"
#include "../../ScoreDBHelper.h"
#include "RhythmState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <optional>
#include <string>
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
         event.action == ReplayEventAction::Release ||
         event.action == ReplayEventAction::Miss;
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

inline Target targetFromBestSnapshot(const bms_parser::ChartMeta &meta,
                                     const ScoreBestSnapshot &best,
                                     const ReplayData *replay = nullptr) {
  const int totalNotes = std::max(0, meta.TotalNotes);
  const int fallbackMaxScore = totalNotes * 2;
  Target target;
  target.enabled = totalNotes > 0 && best.score > 0;
  target.label = "BEST";
  target.finalScore = std::max(0, best.score);
  target.maxScore = best.maxScore > 0 ? best.maxScore : fallbackMaxScore;
  target.totalNotes = totalNotes;

  if (!target.enabled) {
    return target;
  }

  if (replay != nullptr && replay->finalScore == best.score) {
    std::vector<int> progression =
        buildReplayScoreProgression(*replay, totalNotes);
    if (!progression.empty() && progression.back() == target.finalScore) {
      target.usesReplayProgression = true;
      target.scoreAfterNotes = std::move(progression);
    }
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
  const int maxScore = totalNotes * 2;
  Target target;
  const std::string normalized = normalizeTargetId(targetId);
  target.label = displayTargetLabel(normalized);
  target.maxScore = maxScore;
  target.totalNotes = totalNotes;
  const std::optional<RateTargetFraction> fraction =
      gradeTargetFraction(normalized);
  target.enabled = totalNotes > 0 && fraction.has_value();
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
