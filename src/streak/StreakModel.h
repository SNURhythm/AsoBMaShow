#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace streak {
inline constexpr int sharedDailyBonusLimit = 10;
inline constexpr int baseFreezePrice = 20;
inline constexpr int freezePriceStep = 5;
inline constexpr int maxFreezePrice = 50;
using DayId = int; // Local civil days since 1970-01-01; never elapsed 24h periods.
struct Level {
  std::string source;
  std::string level;
  std::string label;
  bool operator==(const Level &) const = default;
};
struct Assignment {
  std::string id;
  std::string name;
  std::vector<Level> levels;
  int eligibleLamp = -2; // -2 any, -1 no play, otherwise the stored clear rank.
  bool eligibleOrBelow = false;
  int targetLamp = 300; // -1 disables the lamp target.
  int scoreNumerator = 0; // 0 disables the score target.
  int scoreDenominator = 10000;
  bool customRate = false;
  int chartCount = 1;
  int points = 10;
};
struct Config {
  int dailyGoal = 100;
  int weeklyGoal = 500;
  int monthlyGoal = 2000;
  std::vector<Assignment> assignments;
};
struct ChartAchievement {
  std::string eligibleSince;
  int lamp = -1;
  int score = 0;
  int maxScore = 0;
};
struct Progress {
  Assignment requirement;
  int completedRounds = 0;
  int bonusRounds = 0;
  std::int64_t earnedPoints = 0;
  int charts = 0;
  std::set<std::string> usedCharts;
  std::map<std::string, ChartAchievement> chartProgress;
};
struct Day {
  Config config;
  bool played = false;
  bool frozen = false;
  std::int64_t points = 0;
  std::vector<Progress> assignments;
};
struct State {
  int version = 3;
  std::map<DayId, Config> configurations;
  std::map<DayId, Day> days;
  std::int64_t savings = 0;
  int freezes = 0;
  int freezePurchases = 0;
  int currentStreak = 0;
  int bestStreak = 0;
  std::set<std::string> milestones;
};
struct Play {
  DayId day = 0;
  std::string chart;
  std::vector<Level> levels;
  int previousLamp = -1;
  int lamp = 0;
  int score = 0;
  int maxScore = 0;
  bool manual = true;
  std::string createdAt; // Canonical UTC timestamp for delayed saved results.
};
struct Summary {
  DayId day = 0;
  std::int64_t weeklyPoints = 0;
  std::int64_t monthlyPoints = 0;
};
[[nodiscard]] std::string validate(const Config &config);
[[nodiscard]] DayId localDay(std::int64_t unixSeconds);
[[nodiscard]] DayId today();
[[nodiscard]] std::string dateLabel(DayId day);
[[nodiscard]] std::string lampLabel(int lamp);
[[nodiscard]] std::string scoreLabel(const Assignment &assignment);
[[nodiscard]] bool eligible(const Assignment &assignment, const Play &play);
[[nodiscard]] bool matches(const Assignment &assignment, const Play &play);
void advance(State &state, DayId day);
DayId configure(State &state, const Config &config, DayId day);
[[nodiscard]] int bonusPoints(const Day &day);
[[nodiscard]] int bonusLimit(const Day &day);
std::int64_t record(State &state, const Play &play, std::uint32_t randomChoice);
[[nodiscard]] std::int64_t freezePrice(const State &state);
bool buyFreeze(State &state, DayId day);
[[nodiscard]] Summary summary(const State &state);
} // namespace streak
