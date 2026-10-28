#pragma once

#include "KomboModel.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <string>
#include <stdexcept>

namespace kombo {
using FieldErrors = std::map<std::string, std::string>;

inline bool addLevelRange(Assignment &assignment, const std::vector<Level> &levels,
                          int first, int last) {
  if (first < 0 || last < first || static_cast<std::size_t>(first) >= levels.size() ||
      static_cast<std::size_t>(last) >= levels.size()) return false;
  auto selected = assignment.levels;
  for (int i = first; i <= last; ++i) {
    const auto &level = levels[i];
    const bool duplicate = std::any_of(selected.begin(), selected.end(), [&](const auto &existing) {
      return existing.source == level.source && existing.level == level.level;
    });
    if (!duplicate) {
      if (selected.size() >= 200) return false;
      selected.push_back(level);
    }
  }
  assignment.levels = std::move(selected);
  return true;
}

inline int formInteger(const std::string &text, int maximum, const char *field,
                       const char *rangeError, FieldErrors &errors) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    errors[field] = "kombo.ui.whole_numbers";
  else if (value < 1 || value > maximum) errors[field] = rangeError;
  return value;
}

inline FieldErrors readAssignmentForm(Assignment &assignment, const std::string &name,
                                     const std::string &count, const std::string &points,
                                     const std::string &rate) {
  FieldErrors errors;
  assignment.name = name;
  if (name.empty() || name.find_first_not_of(" \t\r\n") == std::string::npos)
    errors["name"] = "kombo.ui.invalid_name";
  else if (name.size() > 200) errors["name"] = "kombo.ui.name_too_long";
  assignment.chartCount = formInteger(count, 10000, "count", "kombo.ui.invalid_count", errors);
  assignment.points = formInteger(points, 1000000, "points", "kombo.ui.invalid_points", errors);
  if (assignment.levels.empty() || assignment.levels.size() > 200)
    errors["levels"] = "kombo.ui.invalid_levels";
  else for (const auto &level : assignment.levels)
    if (level.source.empty()) errors["levels"] = "kombo.ui.invalid_level_source";
  if (assignment.customRate) {
    try {
      std::size_t end = 0;
      const double value = std::stod(rate, &end);
      if (end != rate.size() || !std::isfinite(value) || value < 0.01 || value > 100)
        errors["rate"] = "kombo.ui.invalid_rate";
      else {
        assignment.scoreNumerator = static_cast<int>(std::lround(value * 100));
        assignment.scoreDenominator = 10000;
      }
    } catch (const std::exception &) { errors["rate"] = "kombo.ui.invalid_rate"; }
  }
  if (!assignment.customRate && assignment.targetLamp < 0 && assignment.scoreNumerator == 0) {
    errors["lamp"] = "kombo.ui.target_required";
    errors["score"] = "kombo.ui.target_required";
  }
  return errors;
}

inline FieldErrors readRoutineForm(Config &config, const std::string &daily,
                                  const std::string &weekly, const std::string &monthly) {
  FieldErrors errors;
  if (config.assignments.empty()) errors["assignments"] = "kombo.ui.assignment_required";
  config.dailyGoal = formInteger(daily, 1000000, "daily", "kombo.ui.invalid_daily", errors);
  config.weeklyGoal = formInteger(weekly, 100000000, "weekly", "kombo.ui.invalid_period", errors);
  config.monthlyGoal = formInteger(monthly, 100000000, "monthly", "kombo.ui.invalid_period", errors);
  return errors;
}

inline bool canBuyFreeze(const State &state) {
  return state.freezes < 2 && state.savings >= freezePrice(state);
}
} // namespace kombo
