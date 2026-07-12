#pragma once

#include <algorithm>
#include <cmath>
#include <string>

namespace score_rank {

inline int targetScoreForFraction(int maxScore, int numerator,
                                  int denominator) {
  if (maxScore <= 0 || numerator <= 0 || denominator <= 0) {
    return 0;
  }
  return static_cast<int>(std::ceil(static_cast<double>(maxScore) *
                                    static_cast<double>(numerator) /
                                    static_cast<double>(denominator)));
}

inline std::string labelForScore(int score, int maxScore) {
  if (maxScore <= 0) {
    return "";
  }

  const int clampedScore = std::max(0, score);
  struct Threshold {
    const char *label;
    int numerator;
    int denominator;
  };
  constexpr Threshold thresholds[] = {
      {"MAX", 1, 1},   {"MAX -", 26, 27}, {"AAA", 24, 27},
      {"AA", 21, 27},  {"A", 18, 27},     {"B", 15, 27},
      {"C", 12, 27},   {"D", 9, 27},      {"E", 6, 27},
  };

  for (const auto &threshold : thresholds) {
    if (clampedScore >= targetScoreForFraction(
                            maxScore, threshold.numerator,
                            threshold.denominator)) {
      return threshold.label;
    }
  }
  return "F";
}

inline int deficitFromMax(int score, int maxScore) {
  return std::max(0, maxScore - score);
}

inline std::string displayLabelForScore(int score, int maxScore) {
  const std::string rank = labelForScore(score, maxScore);
  if (rank == "MAX -") {
    return "MAX-" + std::to_string(deficitFromMax(score, maxScore));
  }
  return rank;
}

} // namespace score_rank
