#pragma once

#include "../scene/play/Judgement.h"

#include <array>

namespace player_settings {

struct JudgementTextVisibility {
  bool pgreat = true;
  bool great = true;
  bool good = true;
  bool bad = true;
  bool poor = true;
  bool kpoor = true;
  bool combo = true;

  [[nodiscard]] bool isVisible(Judgement judgement) const;
  bool operator==(const JudgementTextVisibility &) const = default;
};

struct JudgementTextVisibilityOption {
  const char *label;
  Judgement judgement;
  bool JudgementTextVisibility::*member;
};

inline constexpr std::array<JudgementTextVisibilityOption, 6>
    kJudgementTextVisibilityOptions{{
        {"PGREAT", PGreat, &JudgementTextVisibility::pgreat},
        {"GREAT", Great, &JudgementTextVisibility::great},
        {"GOOD", Good, &JudgementTextVisibility::good},
        {"BAD", Bad, &JudgementTextVisibility::bad},
        {"POOR", Poor, &JudgementTextVisibility::poor},
        {"KPOOR", Kpoor, &JudgementTextVisibility::kpoor},
    }};

inline bool JudgementTextVisibility::isVisible(Judgement judgement) const {
  for (const auto &option : kJudgementTextVisibilityOptions) {
    if (option.judgement == judgement) return this->*option.member;
  }
  return false;
}

} // namespace player_settings
