#include "KomboModel.h"
#include "../ClearLampOrder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace kombo {
namespace {
constexpr std::array lamps{-1, 0, 50, 100, 200, 300, 400, 500, 600};
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
  prepareRoutine(day.config);
  for (const auto &a : day.config.assignments) day.assignments.push_back({.requirement = a, .originalRequirement = a});
  return day;
}
const Config &configurationFor(const State &s, DayId day) {
  auto it = s.configurations.upper_bound(day);
  if (it == s.configurations.begin()) throw std::invalid_argument("Kombo has not started on this date.");
  return std::prev(it)->second;
}
void recompute(State &s, int returnedFreezes = 0, DayId returnedAfter = 0) {
  int current = 0;
  std::map<DayId, std::int64_t> weeks;
  std::map<std::string, std::int64_t> months;
  for (const auto &[id, d] : s.days) {
    if (d.points >= d.config.dailyGoal) {
      ++current;
      s.bestKombo = std::max(s.bestKombo, current);
      for (int n : {7, 14, 30, 50, 100, 365})
        if (current >= n) s.milestones.insert("kombo:" + std::to_string(n));
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
  s.currentKombo = current;
}
void retainMatchingAchievements(Progress &progress) {
  std::erase_if(progress.chartProgress, [&](const auto &entry) {
    if (progress.usedCharts.contains(entry.first)) return false;
    return std::none_of(entry.second.levels.begin(), entry.second.levels.end(), [&](const Level &level) {
      return std::any_of(progress.requirement.levels.begin(), progress.requirement.levels.end(), [&](const Level &selected) {
        return level.source == selected.source && level.level == selected.level;
      });
    });
  });
}
void harder(Assignment &a, const Play &p, std::uint32_t choice) {
  if (isGoalsAssignment(a)) return;
  std::vector<Assignment> options;
  if (a.scoreNumerator > 0 && a.scoreNumerator < a.scoreDenominator) {
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
    options.push_back(std::move(next));
  }
  if (a.targetLamp >= 0 && a.targetLamp < 600) {
    auto next = a;
    next.targetLamp = *std::upper_bound(lamps.begin(), lamps.end(), a.targetLamp,
        [](int left, int right) { return clearLampOrder(left) < clearLampOrder(right); });
    options.push_back(std::move(next));
  }
  auto next = a;
  bool movedLevel = false;
  next.levels.clear();
  for (const auto &level : a.levels) {
    const auto step = std::find_if(p.nextLevels.begin(), p.nextLevels.end(), [&](const LevelStep &step) {
      return step.from.source == level.source && step.from.level == level.level &&
             step.to.source == level.source && step.to.level != level.level;
    });
    const auto &replacement = step == p.nextLevels.end() ? level : step->to;
    movedLevel |= step != p.nextLevels.end();
    if (std::none_of(next.levels.begin(), next.levels.end(), [&](const Level &existing) {
          return existing.source == replacement.source && existing.level == replacement.level;
        })) next.levels.push_back(replacement);
  }
  if (movedLevel) options.push_back(std::move(next));
  if (!options.empty()) a = std::move(options[choice % options.size()]);
}
} // namespace

bool isGoalsAssignment(const Assignment &a) { return a.id == goalsAssignmentId; }
Assignment goalsAssignment() {
  return {.id=goalsAssignmentId, .name="Goals", .targetLamp=-1};
}
void prepareRoutine(Config &config) {
  for (auto &a : config.assignments) a.points = regularRoundPoints;
  if (std::none_of(config.assignments.begin(), config.assignments.end(), isGoalsAssignment))
    config.assignments.push_back(goalsAssignment());
}
std::string validate(const Config &config) {
  if (config.dailyGoal < 1 || config.dailyGoal > 1000000 ||
      config.weeklyGoal < 1 || config.weeklyGoal > 100000000 ||
      config.monthlyGoal < 1 || config.monthlyGoal > 100000000)
    return "Goals must be positive (daily up to 1,000,000; weekly/monthly up to 100,000,000).";
  if (std::count_if(config.assignments.begin(), config.assignments.end(),
          [](const auto &a) { return !isGoalsAssignment(a); }) > 100)
    return "Use at most 100 table assignments.";
  std::set<std::string> ids;
  for (const auto &a : config.assignments) {
    if (a.id.empty() || !ids.insert(a.id).second || a.name.empty() || a.name.size() > 200)
      return "Each assignment needs a unique identity and a name (up to 200 bytes).";
    if (a.chartCount < 1 || a.chartCount > 10000 || a.points < 1 || a.points > 1000000)
      return "Use 1–10,000 completions per round and a valid historical reward.";
    if (isGoalsAssignment(a)) {
      if (!a.levels.empty() || a.targetLamp != -1 || a.scoreNumerator != 0)
        return "The Goals assignment counts personal goal achievements only.";
      continue;
    }
    if (a.levels.empty() || a.levels.size() > 200)
      return "Choose at least one difficulty-table level (up to 200).";
    for (const auto &level : a.levels)
      if (level.source.empty()) return "A selected difficulty table has no source URL.";
    if (a.eligibleScoreRank != -2 && a.eligibleScoreRank != -1 && a.eligibleScoreRank != 0 &&
        std::find(rankNumerators.begin(), rankNumerators.end(), a.eligibleScoreRank) == rankNumerators.end())
      return "Choose a supported previous score rank.";
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
  switch (normalizeClearLamp(lamp)) {
  case -2: return "Any";
  case -1: return "NO PLAY";
  case 0: return "FAILED";
  case 100: return "ASSIST EASY";
  case 50: return "LIGHT ASSIST EASY";
  case 200: return "EASY";
  case 300: return "CLEAR";
  case 400: return "HARD";
  case 500: return "EX HARD";
  case 600: return "FULL COMBO";
  default: return "Unknown";
  }
}
int scoreRank(int score, int maxScore) {
  if (maxScore <= 0) return -1;
  int rank = 0;
  for (int threshold : rankNumerators)
    if (static_cast<std::int64_t>(score) * 27 >= static_cast<std::int64_t>(maxScore) * threshold)
      rank = threshold;
  return rank;
}
std::string scoreRankLabel(int rank) {
  if (rank == -1) return "NO PLAY";
  if (rank == 0) return "F";
  for (std::size_t i = 0; i < rankNumerators.size(); ++i)
    if (rank == rankNumerators[i]) return rankLabels[i];
  return "Unknown";
}
std::string scoreValueLabel(const Assignment &a) {
  if (a.scoreNumerator == 0) return {};
  if (!a.customRate) {
    for (std::size_t i = 0; i < rankNumerators.size(); ++i)
      if (a.scoreNumerator * 27 == rankNumerators[i] * a.scoreDenominator)
        return std::string(rankLabels[i]);
  }
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << (100.0 * a.scoreNumerator / a.scoreDenominator) << "% EX";
  return text.str();
}
bool eligible(const Assignment &a, const Play &p) {
  if (isGoalsAssignment(a)) return false; // Credit comes only from committed goal events.
  const bool levelMatches = std::any_of(a.levels.begin(), a.levels.end(), [&](const Level &level) {
    return std::any_of(p.levels.begin(), p.levels.end(), [&](const Level &candidate) {
      return level.source == candidate.source && level.level == candidate.level;
    });
  });
  if (!levelMatches || !p.manual || p.chart.empty()) return false;
  if (a.eligibleLamp != -2 && (a.eligibleOrBelow ? clearLampOrder(p.previousLamp) > clearLampOrder(a.eligibleLamp) : normalizeClearLamp(p.previousLamp) != normalizeClearLamp(a.eligibleLamp))) return false;
  if (a.eligibleScoreRank != -2) {
    const int rank = scoreRank(p.previousScore, p.previousMaxScore);
    if (a.eligibleScoreOrBelow ? rank > a.eligibleScoreRank : rank != a.eligibleScoreRank) return false;
  }
  return true;
}
namespace {
bool meetsTargets(const Assignment &a, int lamp, int score, int maxScore) {
  if (a.targetLamp >= 0 && clearLampOrder(lamp) < clearLampOrder(a.targetLamp)) return false;
  return a.scoreNumerator == 0 || (maxScore > 0 &&
      static_cast<std::int64_t>(score) * a.scoreDenominator >=
      static_cast<std::int64_t>(maxScore) * a.scoreNumerator);
}
} // namespace
bool matches(const Assignment &a, const Play &p) {
  return eligible(a, p) && meetsTargets(a, p.lamp, p.score, p.maxScore);
}
void advance(State &s, DayId day) {
  if (s.configurations.empty()) return;
  if (!s.days.empty() && day <= s.days.rbegin()->first) return;
  const DayId first = s.days.empty() ? s.configurations.begin()->first : s.days.rbegin()->first + 1;
  if (day < first) return;
  if (day - first > 36600) throw std::invalid_argument("System date is too far from the last kombo day.");
  for (DayId id = first; id <= day; ++id) {
    if (!s.days.empty()) {
      auto &previous = s.days.rbegin()->second;
      if (!previous.played && !previous.frozen && s.currentKombo > 0 && s.freezes > 0) {
        --s.freezes;
        previous.frozen = true;
      }
    }
    s.days.emplace(id, newDay(configurationFor(s, id)));
    // Only closing a new day can break the current kombo and reset prices.
    // Late saves may move an old gap during recompute, but cannot break a new
    // kombo or undo purchases made after the original break.
    const int before = s.currentKombo;
    recompute(s);
    if (before > 0 && s.currentKombo == 0) s.freezePurchases = 0;
  }
}
DayId configure(State &s, const Config &config, DayId day) {
  auto routine = config;
  prepareRoutine(routine);
  if (const auto error = validate(routine); !error.empty()) throw std::invalid_argument(error);
  advance(s, day);
  if (!s.days.empty()) day = std::max(day, s.days.rbegin()->first);
  const DayId effective = day + (s.configurations.empty() ? 0 : 1);
  s.configurations[effective] = routine;
  if (effective == day) s.days[day] = newDay(routine);
  recompute(s);
  return effective;
}
void adjustAssignment(State &s, const Assignment &replacement, DayId dayId) {
  if (isGoalsAssignment(replacement))
    throw std::invalid_argument("Change the Goals quota in tomorrow's routine.");
  advance(s, dayId);
  if (s.days.empty() || s.days.rbegin()->first != dayId)
    throw std::invalid_argument("Only today's assignments can be adjusted.");
  auto &day = s.days.at(dayId);
  auto found = std::find_if(day.assignments.begin(), day.assignments.end(),
      [&](const Progress &p) { return p.requirement.id == replacement.id; });
  if (found == day.assignments.end()) throw std::invalid_argument("Assignment no longer exists.");
  if (replacement.points != found->requirement.points)
    throw std::invalid_argument("Today's reward stays unchanged.");
  auto config = day.config;
  const auto index = static_cast<std::size_t>(found - day.assignments.begin());
  config.assignments[index] = replacement;
  if (const auto error = validate(config); !error.empty()) throw std::invalid_argument(error);
  day.config = std::move(config);
  found->requirement = replacement;
  if (day.points >= day.config.dailyGoal) found->requirement.chartCount = found->bonusChartCount;
  // Keep the daily chart ledger, including partial achievements. Editing never
  // awards points; even a reduced quota needs another distinct qualifying play.
}
bool canResetExtraChallenge(const Progress &progress) {
  if (isGoalsAssignment(progress.requirement)) return false;
  const auto &a = progress.requirement;
  const auto &b = progress.originalRequirement;
  return progress.bonusChartCount == 1 && progress.bonusDifficultyApplied &&
      (a.targetLamp != b.targetLamp ||
       static_cast<std::int64_t>(a.scoreNumerator) * b.scoreDenominator !=
           static_cast<std::int64_t>(b.scoreNumerator) * a.scoreDenominator || a.levels != b.levels);
}
void resetExtraChallenge(State &s, const std::string &assignmentId, DayId dayId) {
  if (s.days.empty() || s.days.rbegin()->first != dayId)
    throw std::invalid_argument("Only today's extra challenges can be reset.");
  auto &day = s.days.at(dayId);
  if (day.points < day.config.dailyGoal || bonusPoints(day) >= bonusLimit(day))
    throw std::invalid_argument("No extra challenge is available today.");
  auto found = std::find_if(day.assignments.begin(), day.assignments.end(),
      [&](const Progress &p) { return p.requirement.id == assignmentId; });
  if (found == day.assignments.end()) throw std::invalid_argument("Assignment no longer exists.");
  if (found->bonusChartCount == 2) return; // Repeated confirmation must not discard partial progress.
  if (!canResetExtraChallenge(*found)) throw std::invalid_argument("This challenge is already at its original difficulty.");
  found->requirement = found->originalRequirement;
  found->bonusDifficultyApplied = true;
  found->bonusChartCount = found->requirement.chartCount = 2;
  found->charts = 0;
  retainMatchingAchievements(*found);
}
int bonusLimit(const Day &day) {
  return day.assignments.empty() ? 0 : sharedDailyBonusLimit;
}
int bonusPoints(const Day &day) {
  int points = 0;
  for (const auto &progress : day.assignments) points += progress.bonusRounds;
  return points;
}
std::int64_t record(State &s, const Play &p, std::uint32_t randomChoice, PlayProgress *receipt) {
  if (receipt) *receipt = {};
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
  if (receipt) {
    receipt->day = p.day;
    for (const auto &progress : day.assignments)
      receipt->assignments.push_back({.id=progress.requirement.id,
          .beforeCharts=progress.charts,
          .beforeQuota=progress.requirement.chartCount,
          .beforeRound=progress.completedRounds + 1});
  }
  int remainingBonus = std::max(0, bonusLimit(day) - bonusPoints(day));
  std::vector<Progress *> ordered;
  for (auto &progress : day.assignments) ordered.push_back(&progress);
  std::sort(ordered.begin(), ordered.end(), [](const auto *a, const auto *b) {
    return a->requirement.id < b->requirement.id;
  });
  // Settle each completion in stable ID order so later completions from this
  // play use bonus rewards immediately after the daily target is reached.
  const auto completeRound = [&](Progress &progress) {
    const bool extra = day.points >= day.config.dailyGoal;
    const auto reward = extra ? 1 : std::min<std::int64_t>(
        regularRoundPoints, day.config.dailyGoal - day.points);
    ++progress.completedRounds;
    progress.charts = 0;
    progress.earnedPoints += reward;
    day.points += reward;
    if (extra) { ++progress.bonusRounds; ++s.savings; --remainingBonus; }
  };
  for (auto *item : ordered) {
    if (day.points >= day.config.dailyGoal && remainingBonus == 0) break;
    auto &progress = *item;
    if (isGoalsAssignment(progress.requirement)) {
      for (const auto &goal : p.achievedGoals) {
        if (day.points >= day.config.dailyGoal && remainingBonus == 0) break;
        // A preceding completion can reach the target within this result.
        // Switch before consuming another identity; old partial counts do not
        // carry into the one-goal bonus rounds.
        if (day.points >= day.config.dailyGoal &&
            progress.requirement.chartCount != progress.bonusChartCount) {
          progress.requirement.chartCount = progress.bonusChartCount;
          progress.charts = 0;
        }
        if (goal.empty() || !progress.usedCharts.insert(goal).second) continue;
        progress.chartProgress.emplace(goal, ChartAchievement{});
        if (++progress.charts < progress.requirement.chartCount) continue;
        completeRound(progress);
      }
      continue;
    }
    if (progress.usedCharts.contains(p.chart)) continue;
    auto chart = progress.chartProgress.find(p.chart);
    if (chart == progress.chartProgress.end()) {
      if (!eligible(progress.requirement, p)) continue;
      chart = progress.chartProgress.emplace(p.chart, ChartAchievement{}).first;
      chart->second.eligibleSince = p.createdAt;
      chart->second.levels = p.levels;
    }
    // Entry existence preserves the full eligibility decision for today.
    // Retain actual achievements so raised targets still compare real values.
    auto &achievement = chart->second;
    if (!p.createdAt.empty() && !achievement.eligibleSince.empty() &&
        p.createdAt < achievement.eligibleSince) {
      if (!eligible(progress.requirement, p)) continue;
      achievement.eligibleSince = p.createdAt;
    }
    achievement.lamp = bestClearLamp(achievement.lamp, p.lamp);
    if (p.maxScore > 0 && p.score >= 0 &&
        (achievement.maxScore == 0 ||
         static_cast<std::int64_t>(p.score) * achievement.maxScore >
             static_cast<std::int64_t>(achievement.score) * p.maxScore)) {
      achievement.score = std::min(p.score, p.maxScore);
      achievement.maxScore = p.maxScore;
    }
    if (!meetsTargets(progress.requirement, achievement.lamp, achievement.score, achievement.maxScore)) continue;
    progress.usedCharts.insert(p.chart);
    if (++progress.charts < progress.requirement.chartCount) continue;
    completeRound(progress);
  }
  // Every assignment sees the same final daily total, independent of list order.
  if (!bonus && day.points >= day.config.dailyGoal) {
    for (auto &progress : day.assignments) {
      progress.requirement.chartCount = progress.bonusChartCount;
      progress.charts = 0;
      if (!progress.bonusDifficultyApplied) {
        const auto previousLevels = progress.requirement.levels;
        harder(progress.requirement, p, randomChoice++);
        progress.bonusDifficultyApplied = true;
        // Sticky eligibility remains valid within a challenge, not across a new
        // level range. Already counted charts stay unavailable for the whole day.
        if (progress.requirement.levels != previousLevels) retainMatchingAchievements(progress);
      }
    }
  }
  recompute(s, returnedFreezes, p.day);
  if (receipt) {
    receipt->reachedDailyGoal = !bonus && day.points >= day.config.dailyGoal;
    for (std::size_t i = 0; i < day.assignments.size(); ++i) {
      auto &change = receipt->assignments[i];
      const auto &progress = day.assignments[i];
      change.afterCharts = progress.charts;
      change.afterQuota = progress.requirement.chartCount;
      change.afterRound = progress.completedRounds + 1;
    }
    std::erase_if(receipt->assignments, [](const auto &change) {
      return !change.completedRound() && change.afterCharts <= change.beforeCharts;
    });
  }
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
} // namespace kombo
