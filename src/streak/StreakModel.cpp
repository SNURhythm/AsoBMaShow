#include "StreakModel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace streak {
namespace {
constexpr std::array lamps{-1, 0, 100, 150, 200, 300, 400, 500, 600};
constexpr std::array rankNumerators{6, 9, 12, 15, 18, 21, 24, 26, 27};
constexpr std::array rankLabels{"E", "D", "C", "B", "A", "AA", "AAA", "MAX -", "MAX"};
using namespace std::chrono;
DayId weekStart(DayId day) {
  return day - static_cast<int>((weekday{sys_days{days{day}}}.iso_encoding() + 6) % 7);
}
std::string monthKey(DayId day) { return dateLabel(day).substr(0, 7); }
Day newDay(const Config &config) {
  Day day;
  day.config = config;
  for (const auto &a : config.assignments) day.assignments.push_back({.requirement = a});
  return day;
}
const Config &configurationFor(const State &s, DayId day) {
  auto it = s.configurations.upper_bound(day);
  if (it == s.configurations.begin()) throw std::invalid_argument("Streak has not started on this date.");
  return std::prev(it)->second;
}
void recompute(State &s, int returnedFreezes = 0, DayId returnedAfter = 0) {
  int current = 0;
  std::map<DayId, std::int64_t> weeks;
  std::map<std::string, std::int64_t> months;
  for (const auto &[id, d] : s.days) {
    if (d.points >= d.config.dailyGoal) {
      ++current;
      s.bestStreak = std::max(s.bestStreak, current);
      for (int n : {7, 14, 30, 50, 100, 365})
        if (current >= n) s.milestones.insert("streak:" + std::to_string(n));
    } else if (!d.played && !d.frozen && id != s.days.rbegin()->first) {
      // A returned freeze was owned before the recovered play. Move it to
      // the next absent day, rather than leaving a storage-delay-only gap.
      if (current > 0 && returnedFreezes > 0 && id > returnedAfter) {
        s.days.at(id).frozen = true;
        --s.freezes;
        --returnedFreezes;
      } else {
        current = 0;
      }
    }
    auto &week = weeks[weekStart(id)];
    auto &month = months[monthKey(id)];
    week += d.points;
    month += d.points;
    if (week >= d.config.weeklyGoal)
      s.milestones.insert("week:" + dateLabel(weekStart(id)));
    if (month >= d.config.monthlyGoal)
      s.milestones.insert("month:" + monthKey(id));
  }
  s.currentStreak = current;
}
void harder(Assignment &a, const Play &p, std::uint32_t choice) {
  std::vector<Assignment> options;
  if (a.scoreNumerator > 0 && a.scoreNumerator < a.scoreDenominator && p.maxScore > 0) {
    auto next = a;
    if (a.customRate) {
      next.scoreNumerator = std::min(a.scoreDenominator,
          a.scoreNumerator + std::max(1, a.scoreDenominator / 50));
    } else {
      for (int n : rankNumerators) {
        if (static_cast<std::int64_t>(n) * a.scoreDenominator >
            static_cast<std::int64_t>(a.scoreNumerator) * 27) {
          next.scoreNumerator = n;
          next.scoreDenominator = 27;
          break;
        }
      }
    }
    if (static_cast<std::int64_t>(p.score) * next.scoreDenominator >=
        static_cast<std::int64_t>(p.maxScore) * next.scoreNumerator)
      options.push_back(std::move(next));
  }
  if (a.targetLamp >= 0 && a.targetLamp < 600) {
    auto next = a;
    next.targetLamp = *std::upper_bound(lamps.begin(), lamps.end(), a.targetLamp);
    if (p.lamp >= next.targetLamp) options.push_back(std::move(next));
  }
  if (!options.empty()) a = std::move(options[choice % options.size()]);
}
} // namespace

std::string validate(const Config &config) {
  if (config.dailyGoal < 1 || config.dailyGoal > 1000000 ||
      config.weeklyGoal < 1 || config.weeklyGoal > 100000000 ||
      config.monthlyGoal < 1 || config.monthlyGoal > 100000000)
    return "Goals must be positive (daily up to 1,000,000; weekly/monthly up to 100,000,000).";
  if (config.assignments.size() > 100) return "Use at most 100 assignments.";
  std::set<std::string> ids;
  for (const auto &a : config.assignments) {
    if (a.id.empty() || !ids.insert(a.id).second || a.name.empty() || a.name.size() > 200)
      return "Each assignment needs a unique identity and a name (up to 200 bytes).";
    if (a.levels.empty() || a.levels.size() > 200)
      return "Choose at least one difficulty-table level (up to 200).";
    for (const auto &level : a.levels)
      if (level.source.empty()) return "A selected difficulty table has no source URL.";
    if (a.chartCount < 1 || a.chartCount > 10000 || a.points < 1 || a.points > 1000000)
      return "Use 1–10,000 charts and 1–1,000,000 points per round.";
    if ((a.eligibleLamp != -2 && std::find(lamps.begin(), lamps.end(), a.eligibleLamp) == lamps.end()) ||
        std::find(lamps.begin(), lamps.end(), a.targetLamp) == lamps.end())
      return "Choose a supported clear lamp.";
    if (a.scoreDenominator < 1 || a.scoreDenominator > 10000 ||
        a.scoreNumerator < 0 || a.scoreNumerator > a.scoreDenominator)
      return "EX rate must be between 0 and 100%.";
    if (a.targetLamp < 0 && a.scoreNumerator == 0)
      return "Choose a target lamp or score.";
  }
  return {};
}

DayId localDay(std::int64_t unixSeconds) {
  const auto time = static_cast<std::time_t>(unixSeconds);
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &time) != 0) throw std::invalid_argument("Invalid play date.");
#else
  if (!localtime_r(&time, &local)) throw std::invalid_argument("Invalid play date.");
#endif
  return static_cast<DayId>(std::chrono::sys_days{
      std::chrono::year{local.tm_year + 1900} / (local.tm_mon + 1) / local.tm_mday}
      .time_since_epoch().count());
}
DayId today() { return localDay(std::time(nullptr)); }
std::string dateLabel(DayId day) {
  const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{day}}};
  std::ostringstream text;
  text << static_cast<int>(date.year()) << '-' << std::setfill('0') << std::setw(2)
       << static_cast<unsigned>(date.month()) << '-' << std::setw(2) << static_cast<unsigned>(date.day());
  return text.str();
}
std::string lampLabel(int lamp) {
  switch (lamp) {
  case -2: return "Any";
  case -1: return "NO PLAY";
  case 0: return "FAILED";
  case 100: return "ASSIST EASY";
  case 150: return "LIGHT ASSIST EASY";
  case 200: return "EASY";
  case 300: return "CLEAR";
  case 400: return "HARD";
  case 500: return "EX HARD";
  case 600: return "FULL COMBO";
  default: return "Unknown";
  }
}
std::string scoreLabel(const Assignment &a) {
  if (a.scoreNumerator == 0) return "Any score";
  if (!a.customRate) {
    for (std::size_t i = 0; i < rankNumerators.size(); ++i)
      if (a.scoreNumerator * 27 == rankNumerators[i] * a.scoreDenominator)
        return std::string(rankLabels[i]) + " or above";
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << (100.0 * a.scoreNumerator / a.scoreDenominator) << "% EX or above";
  return text.str();
}
bool matches(const Assignment &a, const Play &p) {
  const bool levelMatches = std::any_of(a.levels.begin(), a.levels.end(), [&](const Level &level) {
    return std::any_of(p.levels.begin(), p.levels.end(), [&](const Level &candidate) {
      return level.source == candidate.source && level.level == candidate.level;
    });
  });
  if (!levelMatches || !p.manual || p.chart.empty()) return false;
  if (a.eligibleLamp != -2 && (a.eligibleOrBelow ? p.previousLamp > a.eligibleLamp : p.previousLamp != a.eligibleLamp)) return false;
  if (a.targetLamp >= 0 && p.lamp < a.targetLamp) return false;
  return a.scoreNumerator == 0 || (p.maxScore > 0 &&
      static_cast<std::int64_t>(p.score) * a.scoreDenominator >=
      static_cast<std::int64_t>(p.maxScore) * a.scoreNumerator);
}
void advance(State &s, DayId day) {
  if (s.configurations.empty()) return;
  if (!s.days.empty() && day <= s.days.rbegin()->first) return;
  const DayId first = s.days.empty() ? s.configurations.begin()->first : s.days.rbegin()->first + 1;
  if (day < first) return;
  if (day - first > 36600) throw std::invalid_argument("System date is too far from the last streak day.");
  for (DayId id = first; id <= day; ++id) {
    if (!s.days.empty()) {
      auto &previous = s.days.rbegin()->second;
      if (!previous.played && !previous.frozen && s.currentStreak > 0 && s.freezes > 0) {
        --s.freezes;
        previous.frozen = true;
      }
    }
    s.days.emplace(id, newDay(configurationFor(s, id)));
    // Only closing a new day can break the current streak and reset prices.
    // Late saves may move an old gap during recompute, but cannot break a new
    // streak or undo purchases made after the original break.
    const int before = s.currentStreak;
    recompute(s);
    if (before > 0 && s.currentStreak == 0) s.freezePurchases = 0;
  }
}
DayId configure(State &s, const Config &config, DayId day) {
  if (const auto error = validate(config); !error.empty()) throw std::invalid_argument(error);
  advance(s, day);
  if (!s.days.empty()) day = std::max(day, s.days.rbegin()->first);
  const DayId effective = day + (s.configurations.empty() ? 0 : 1);
  s.configurations[effective] = config;
  if (effective == day) s.days[day] = newDay(config);
  recompute(s);
  return effective;
}
int bonusLimit(const Day &day) {
  return day.assignments.empty() ? 0 : sharedDailyBonusLimit;
}
int bonusPoints(const Day &day) {
  int points = 0;
  for (const auto &progress : day.assignments) points += progress.bonusRounds;
  return points;
}
std::int64_t record(State &s, const Play &p, std::uint32_t randomChoice) {
  if (!p.manual || p.chart.empty() || s.configurations.empty() || p.day < s.configurations.begin()->first) return 0;
  advance(s, p.day);
  auto found = s.days.find(p.day);
  if (found == s.days.end()) return 0;
  auto &day = found->second;
  day.played = true;
  const int returnedFreezes = day.frozen ? 1 : 0;
  if (day.frozen) { ++s.freezes; day.frozen = false; }
  const auto before = day.points;
  const bool bonus = before >= day.config.dailyGoal;
  if (bonus) {
    // Existing saved bonus rounds may still have the older multi-chart quota.
    // Their partial progress grants no automatic reward; the next new chart
    // completes one bonus round under the current rule.
    for (auto &progress : day.assignments) {
      progress.requirement.chartCount = 1;
      progress.charts = 0;
    }
  }
  int remainingBonus = std::max(0, bonusLimit(day) - bonusPoints(day));
  std::vector<Progress *> ordered;
  for (auto &progress : day.assignments) ordered.push_back(&progress);
  std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) {
    return a->requirement.id < b->requirement.id;
  });
  std::vector<Progress *> completed;
  for (auto *item : ordered) {
    if (bonus && remainingBonus == 0) break;
    auto &progress = *item;
    if (!matches(progress.requirement, p) || progress.usedCharts.contains(p.chart)) continue;
    progress.usedCharts.insert(p.chart);
    if (++progress.charts < progress.requirement.chartCount) continue;
    ++progress.completedRounds;
    progress.charts = 0;
    completed.push_back(&progress);
    if (bonus) --remainingBonus;
  }
  // A play that reaches the goal earns no bonus, including simultaneous
  // completions. Allocate its remaining goal budget in stable identity order.
  for (auto *progress : completed) {
    const auto reward = bonus ? 1 : std::min<std::int64_t>(
        progress->requirement.points, day.config.dailyGoal - day.points);
    progress->earnedPoints += reward;
    day.points += reward;
    if (bonus) { ++progress->bonusRounds; ++s.savings; }
  }
  // Every assignment sees the same final daily total, independent of list order.
  if (day.points >= day.config.dailyGoal) {
    for (auto &progress : day.assignments) {
      progress.requirement.chartCount = 1;
      progress.charts = 0;
    }
    for (auto *progress : completed) harder(progress->requirement, p, randomChoice++);
  }
  recompute(s, returnedFreezes, p.day);
  return day.points - before;
}
std::int64_t freezePrice(const State &s) {
  return std::min<std::int64_t>(maxFreezePrice,
      baseFreezePrice + static_cast<std::int64_t>(freezePriceStep) * s.freezePurchases);
}
bool buyFreeze(State &s, DayId day) {
  advance(s, day);
  if (s.days.empty() || s.freezes >= 2 || s.freezePurchases == std::numeric_limits<int>::max()) return false;
  const auto price = freezePrice(s);
  if (s.savings < price) return false;
  s.savings -= price;
  ++s.freezes;
  ++s.freezePurchases;
  return true;
}
Summary summary(const State &s) {
  Summary result;
  if (s.days.empty()) return result;
  result.day = s.days.rbegin()->first;
  for (const auto &[id, day] : s.days) {
    if (weekStart(id) == weekStart(result.day)) result.weeklyPoints += day.points;
    if (monthKey(id) == monthKey(result.day)) result.monthlyPoints += day.points;
  }
  return result;
}
} // namespace streak
