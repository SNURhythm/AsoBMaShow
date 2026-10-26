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
  CHECK(record(s, play(20000, "four"), 0) == 60);
  CHECK(s.currentStreak == 1 && s.savings == 20);
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
  CHECK(bank.savings == 200 && bank.currentStreak == 1);
  CHECK(buyFreeze(bank, 21000) && buyFreeze(bank, 21000));
  CHECK(!buyFreeze(bank, 21000));
  CHECK(bank.savings == 0 && bank.days.at(21000).points == 300);
  advance(bank, 21002);
  CHECK(bank.freezes == 1 && bank.days.at(21001).frozen);
  CHECK(bank.currentStreak == 1);
  record(bank, play(21001, "late"), 0);
  CHECK(bank.freezes == 2 && !bank.days.at(21001).frozen);
  CHECK(bank.currentStreak == 2);
  auto effective = configure(bank, config(200, 100), 21002);
  CHECK(effective == 21002);
  record(bank, play(21002, "b"), 0);
  CHECK(configure(bank, config(500, 100), 21002) == 21003);
  CHECK(bank.days.at(21002).config.dailyGoal == 200);
  advance(bank, 21003);
  CHECK(bank.days.at(21003).config.dailyGoal == 500);
  advance(bank, 21000); // A clock rollback cannot reroll today's assignments.
  CHECK(bank.days.rbegin()->first == 21003);

  State recovered;
  configure(recovered, config(100, 200), 21500);
  record(recovered, play(21500, "start"), 0);
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
  std::cout << "streak model tests passed\n";
}
