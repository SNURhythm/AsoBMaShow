#pragma once

#include "../scene/play/Judgement.h"

namespace player_settings {

enum class JudgementTimingDisplayCriteria {
  GreatOrBelow = 0,
  PGreatOrBelow = 1,
  GoodOrBelow = 2,
  BadOrBelow = 3,
  Off = 4,
};

inline JudgementTimingDisplayCriteria sanitizeJudgementTimingDisplayCriteria(
    JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case JudgementTimingDisplayCriteria::GreatOrBelow:
  case JudgementTimingDisplayCriteria::PGreatOrBelow:
  case JudgementTimingDisplayCriteria::GoodOrBelow:
  case JudgementTimingDisplayCriteria::BadOrBelow:
  case JudgementTimingDisplayCriteria::Off:
    return criteria;
  }
  return JudgementTimingDisplayCriteria::GreatOrBelow;
}

inline bool judgementMeetsTimingCriteria(
    Judgement judgement, JudgementTimingDisplayCriteria criteria) {
  if (judgement < PGreat || judgement > Poor) return false;
  switch (sanitizeJudgementTimingDisplayCriteria(criteria)) {
  case JudgementTimingDisplayCriteria::PGreatOrBelow:
    return true;
  case JudgementTimingDisplayCriteria::GreatOrBelow:
    return judgement != PGreat;
  case JudgementTimingDisplayCriteria::GoodOrBelow:
    return judgement >= Good;
  case JudgementTimingDisplayCriteria::BadOrBelow:
    return judgement >= Bad;
  case JudgementTimingDisplayCriteria::Off:
    return false;
  }
  return false;
}

} // namespace player_settings
