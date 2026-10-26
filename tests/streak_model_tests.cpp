#include "streak/StreakModel.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace streak;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

Config config(int goal = 100, int reward = 60, int count = 1) {
  Config c;
  c.dailyGoal = goal;
  c.assignments = {{.id = "a", .name = "Explore", .levels = {{"table", "0", "sl0"}},
                    .chartCount = count, .points = reward}};
  return c;
}
Play play(DayId day, std::string chart) {
  return {.day = day, .chart = std::move(chart), .levels = {{"table", "0", ""}},
          .lamp = 300, .score = 180, .maxScore = 200};
}
int main() {
  State s;
  configure(s, config(100, 60, 2), 20000);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  CHECK(record(s, play(20000, "two"), 0) == 60);
  CHECK(s.days.at(20000).assignments[0].completedRounds == 1);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  record(s, play(20000, "three"), 0);
  CHECK(record(s, play(20000, "four"), 0) == 40);
  CHECK(s.currentStreak == 1 && s.savings == 0);
  CHECK(s.days.at(20000).assignments[0].requirement.chartCount == 3);
  CHECK(record(s, play(20000, "five"), 0) == 0);
  CHECK(s.currentStreak == 1);
  advance(s, 20001);
  CHECK(s.days.at(20001).assignments[0].requirement.chartCount == 2);
  auto ineligible = play(20001, "outside"); ineligible.levels.clear();
  CHECK(record(s, ineligible, 0) == 0);
  advance(s, 20002);
  CHECK(s.currentStreak == 1 && s.freezes == 0);
  advance(s, 20003);
  CHECK(s.currentStreak == 0 && s.bestStreak == 1);
  // A delayed durable save belongs to its actual day and can repair that gap.
  record(s, play(20002, "late"), 0);
  CHECK(s.currentStreak == 1);

  State bank;
  configure(bank, config(100, 300), 21000);
  record(bank, play(21000, "a"), 0);
  CHECK(bank.savings == 0 && bank.currentStreak == 1);
  bank.savings = 50; // Isolate inventory and calendar behavior from earning.
  CHECK(buyFreeze(bank, 21000) && buyFreeze(bank, 21000));
  CHECK(!buyFreeze(bank, 21000));
  CHECK(bank.savings == 0 && bank.days.at(21000).points == 100);
  advance(bank, 21002);
  CHECK(bank.freezes == 1 && bank.days.at(21001).frozen);
  CHECK(bank.currentStreak == 1);
  record(bank, play(21001, "late"), 0);
  CHECK(bank.freezes == 2 && !bank.days.at(21001).frozen);
  CHECK(bank.currentStreak == 2);
  auto effective = configure(bank, config(200, 100), 21002);
  CHECK(effective == 21003);
  record(bank, play(21002, "b"), 0);
  CHECK(configure(bank, config(500, 100), 21002) == 21003);
  CHECK(bank.days.at(21002).config.dailyGoal == 100);
  advance(bank, 21003);
  CHECK(bank.days.at(21003).config.dailyGoal == 500);
  advance(bank, 21000); // A clock rollback cannot reroll today's assignments.
  CHECK(bank.days.rbegin()->first == 21003);

  State recovered;
  configure(recovered, config(100, 200), 21500);
  record(recovered, play(21500, "start"), 0);
  recovered.savings = 20;
  CHECK(buyFreeze(recovered, 21500));
  advance(recovered, 21503);
  CHECK(recovered.currentStreak == 0 && recovered.freezes == 0);
  auto recoveredPlay = play(21501, "late"); recoveredPlay.levels.clear();
  record(recovered, recoveredPlay, 0);
  CHECK(recovered.currentStreak == 1 && recovered.freezes == 0);
  CHECK(recovered.days.at(21502).frozen);

  State ranks;
  auto c = config(1, 1);
  c.assignments[0].eligibleLamp = 0;
  c.assignments[0].targetLamp = -1;
  c.assignments[0].scoreNumerator = 24;
  c.assignments[0].scoreDenominator = 27;
  configure(ranks, c, 22000);
  auto p = play(22000, "rank"); p.previousLamp = 0; p.maxScore = 100; p.score = 88;
  CHECK(record(ranks, p, 1) == 0);
  p.score = 89;
  CHECK(record(ranks, p, 1) == 1);
  CHECK(ranks.days.at(22000).assignments[0].requirement.scoreNumerator == 26);
  p.chart = "cleared-before"; p.previousLamp = 300; p.score = 100;
  CHECK(record(ranks, p, 1) == 0);
  State automatic;
  configure(automatic, config(), 22000);
  p = play(22000, "auto"); p.manual = false;
  record(automatic, p, 0);
  CHECK(!automatic.days.at(22000).played);

  State milestones;
  configure(milestones, config(100, 100), 23000);
  for (int d = 23000; d < 23007; ++d) record(milestones, play(d, "same-next-day"), 0);
  CHECK(milestones.currentStreak == 7 && milestones.milestones.contains("streak:7"));
  advance(milestones, 23009);
  CHECK(milestones.currentStreak == 0 && milestones.milestones.contains("streak:7"));
  CHECK(!validate(config(0)).empty());
  auto invalid = config(); invalid.assignments[0].scoreDenominator = 0;
  CHECK(!validate(invalid).empty());
  // Even edits before any play wait until tomorrow; the last save wins.
  State edits;
  CHECK(configure(edits, config(100, 60), 24000) == 24000);
  CHECK(configure(edits, config(200, 90), 24000) == 24001);
  CHECK(edits.days.at(24000).config.dailyGoal == 100);
  CHECK(configure(edits, config(300, 120), 24000) == 24001);
  CHECK(record(edits, play(24000, "today"), 0) == 60);
  advance(edits, 24001);
  CHECK(edits.days.at(24001).config.dailyGoal == 300);
  CHECK(edits.days.at(24001).assignments[0].requirement.points == 120);

  // Overlapping completions on the goal-reaching play cannot mint savings.
  State bonus;
  auto overlapping = config(100, 80);
  auto second = overlapping.assignments[0]; second.id = "b";
  overlapping.assignments.push_back(second);
  configure(bonus, overlapping, 25000);
  CHECK(record(bonus, play(25000, "goal"), 0) == 100);
  CHECK(bonus.savings == 0);
  for (int n = 0; n < 100; ++n) {
    auto extra = play(25000, "extra" + std::to_string(n)); extra.lamp = 600;
    record(bonus, extra, 0);
  }
  CHECK(bonus.savings == 10 && bonus.days.at(25000).points == 110);
  for (const auto &progress : bonus.days.at(25000).assignments)
    CHECK(progress.bonusRounds == 5 && progress.charts == 0);
  advance(bonus, 25001);
  CHECK(bonus.days.at(25001).assignments[0].bonusRounds == 0);
  CHECK(record(bonus, play(25001, "goal"), 0) == 100 && bonus.savings == 10);

  State prices;
  configure(prices, config(100, 100), 26000);
  record(prices, play(26000, "start"), 0);
  prices.savings = 1000;
  CHECK(freezePrice(prices) == 20 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 30 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 40 && !buyFreeze(prices, 26000));
  CHECK(prices.savings == 950 && freezePrice(prices) == 40);
  advance(prices, 26003); // Both freezes used; no break yet.
  CHECK(prices.currentStreak == 1 && prices.freezes == 0 && freezePrice(prices) == 40);
  advance(prices, 26004);
  CHECK(prices.currentStreak == 0 && freezePrice(prices) == 20);
  CHECK(buyFreeze(prices, 26004) && freezePrice(prices) == 30);
  record(prices, play(26004, "restart"), 0); // Replaying old gaps must not reset price again.
  CHECK(freezePrice(prices) == 30);
  advance(prices, 26007); // Another streak, another actual break.
  CHECK(prices.currentStreak == 0 && freezePrice(prices) == 20);
  prices.savings = 19;
  CHECK(!buyFreeze(prices, 26007) && freezePrice(prices) == 20);

  // Late attendance can move the historical gap without breaking a new streak.
  State historical;
  configure(historical, config(100, 100), 27000);
  record(historical, play(27000, "start"), 0);
  advance(historical, 27003);
  historical.savings = 100;
  CHECK(buyFreeze(historical, 27003) && freezePrice(historical) == 30);
  auto lateAttendance = play(27001, "late-attendance"); lateAttendance.levels.clear();
  record(historical, lateAttendance, 0);
  CHECK(historical.currentStreak == 0 && freezePrice(historical) == 30);

  std::cout << "streak model tests passed\n";
}
