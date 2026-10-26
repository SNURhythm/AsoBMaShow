#pragma once

#include "StreakModel.h"
#include <charconv>
#include <cmath>
#include <map>
#include <string>
#include <stdexcept>

namespace streak {
using FieldErrors = std::map<std::string, std::string>;

inline int formInteger(const std::string &text, int maximum, const char *field,
                       const char *rangeError, FieldErrors &errors) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    errors[field] = "streak.ui.whole_numbers";
  else if (value < 1 || value > maximum) errors[field] = rangeError;
  return value;
}

inline FieldErrors readAssignmentForm(Assignment &assignment, const std::string &name,
                                     const std::string &count, const std::string &points,
                                     const std::string &rate) {
  FieldErrors errors;
  assignment.name = name;
  if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string::npos)
    errors["name"] = "streak.ui.invalid_name";
  else if (name.size() > 200) errors["name"] = "streak.ui.name_too_long";
  assignment.chartCount = formInteger(count, 10000, "count", "streak.ui.invalid_count", errors);
  assignment.points = formInteger(points, 1000000, "points", "streak.ui.invalid_points", errors);
  if (assignment.levels.empty() || assignment.levels.size() > 200)
    errors["levels"] = "streak.ui.invalid_levels";
  else for (const auto &level : assignment.levels)
    if (level.source.empty()) errors["levels"] = "streak.ui.invalid_level_source";
  if (assignment.customRate) {
    try {
      std::size_t end = 0;
      const double value = std::stod(rate, &end);
      if (end != rate.size() || !std::isfinite(value) || value < 0.01 || value > 100)
        errors["rate"] = "streak.ui.invalid_rate";
      else {
        assignment.scoreNumerator = static_cast<int>(std::lround(value * 100));
        assignment.scoreDenominator = 10000;
      }
    } catch (const std::exception &) { errors["rate"] = "streak.ui.invalid_rate"; }
  }
  if (!assignment.customRate && assignment.targetLamp < 0 && assignment.scoreNumerator == 0) {
    errors["lamp"] = "streak.ui.target_required";
    errors["score"] = "streak.ui.target_required";
  }
  return errors;
}

inline FieldErrors readRoutineForm(Config &config, const std::string &daily,
                                  const std::string &weekly, const std::string &monthly) {
  FieldErrors errors;
  config.dailyGoal = formInteger(daily, 1000000, "daily", "streak.ui.invalid_daily", errors);
  config.weeklyGoal = formInteger(weekly, 100000000, "weekly", "streak.ui.invalid_period", errors);
  config.monthlyGoal = formInteger(monthly, 100000000, "monthly", "streak.ui.invalid_period", errors);
  return errors;
}

inline bool canBuyFreeze(const State &state) {
  return state.freezes < 2 && state.savings >= freezePrice(state);
}
} // namespace streak
