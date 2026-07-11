#include "Judge.h"
#include <algorithm>
#include <utility>

namespace {
void collapseJudgementWindow(
    std::map<Judgement, std::pair<long long, long long>> &timingWindows,
    Judgement target, Judgement replacement) {
  const auto replacementIt = timingWindows.find(replacement);
  const auto targetIt = timingWindows.find(target);
  if (replacementIt == timingWindows.end() || targetIt == timingWindows.end()) {
    return;
  }
  targetIt->second = replacementIt->second;
}

long long scaleWindowEdge(long long value, int playbackRatePercent,
                          int judgeScalePercent) {
  constexpr long long denominator = 10000LL;
  const long long numerator = value *
                              static_cast<long long>(playbackRatePercent) *
                              static_cast<long long>(judgeScalePercent);
  const long long roundingOffset = denominator / 2;
  return numerator >= 0 ? (numerator + roundingOffset) / denominator
                        : (numerator - roundingOffset) / denominator;
}
} // namespace

Judge::Judge(const int Rank) {
  const int Clamped = clampRank(Rank);
  timingWindows = TimingWindowsByRank[Clamped];
}

void Judge::applyCourseJudgementConstraint(
    CourseJudgementConstraint constraint) {
  switch (constraint) {
  case CourseJudgementConstraint::NoGood:
    collapseJudgementWindow(timingWindows, Good, Great);
    return;
  case CourseJudgementConstraint::NoGreat:
    collapseJudgementWindow(timingWindows, Great, PGreat);
    collapseJudgementWindow(timingWindows, Good, PGreat);
    return;
  case CourseJudgementConstraint::None:
    return;
  }
}

void Judge::applyWindowScale(int playbackRatePercent, int judgeScalePercent) {
  for (auto &[judgement, window] : timingWindows) {
    (void)judgement;
    window.first =
        scaleWindowEdge(window.first, playbackRatePercent, judgeScalePercent);
    window.second =
        scaleWindowEdge(window.second, playbackRatePercent, judgeScalePercent);
  }
}

void Judge::setAllowedNoteRange(std::optional<NoteTimeRange> range) {
  allowedNoteRange = std::move(range);
}

bool Judge::allowsNote(const bms_parser::Note *note) const {
  return note != nullptr &&
         (!allowedNoteRange.has_value() || allowedNoteRange->contains(note));
}

bool Judge::checkRange(const long long Diff, const long long Early,
                       const long long Late) {
  return Early <= Diff && Diff <= Late;
}

JudgeResult Judge::judgeNow(const bms_parser::Note *Note,
                            const long long InputTime) {
  if (!allowsNote(Note)) {
    return JudgeResult{None, 0};
  }
  const auto &timeline = Note->Timeline;
  const long long diff = InputTime - timeline->Timing;
  // check range for each judgement
  for (const auto &window : timingWindows) {
    const auto &judgement = window.first;
    const auto &range = window.second;
    if (checkRange(diff, range.first, range.second)) {
      return JudgeResult{judgement, diff};
    }
  }

  return JudgeResult{None, diff};
}

int Judge::clampRank(const int rank) { return std::clamp(rank, 0, 3); };

std::string Judge::getRankDescription(const int Rank) {
  switch (clampRank(Rank)) {
  case 0:
    return "VERY HARD";
  case 1:
    return "HARD";
  case 2:
    return "NORMAL";
  case 3:
    return "EASY";
  default:
    return "EASY";
  }
}
