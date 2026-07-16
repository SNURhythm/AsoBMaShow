#pragma once

#include <string>

enum Judgement { PGreat, Great, Good, Bad, Kpoor, Poor, None, JudgementCount };

enum class CourseJudgementConstraint { None, NoGood, NoGreat };

class JudgeResult {
public:
  JudgeResult(Judgement judgement, long long diffMicros)
      : judgement(judgement), Diff(diffMicros) {}

  Judgement judgement = None;
  long long Diff = 0;

  [[nodiscard]] bool isComboBreak() const {
    return judgement == Bad || judgement == Poor;
  }

  [[nodiscard]] bool isNotePlayed() const {
    return judgement != Kpoor && judgement != None;
  }

  [[nodiscard]] std::string toString() const {
    switch (judgement) {
    case PGreat:
      return "PGREAT";
    case Great:
      return "GREAT";
    case Good:
      return "GOOD";
    case Bad:
      return "BAD";
    case Kpoor:
      return "KPOOR";
    case Poor:
      return "POOR";
    case None:
    case JudgementCount:
      return "NONE";
    }
    return "NONE";
  }
};
