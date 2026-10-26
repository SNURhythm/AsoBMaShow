#include "streak/StreakModel.h"
#include "streak/StreakForm.h"
#include "streak/StreakRoundMotion.h"
#include "streak/StreakResultPresentation.h"
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
  auto scoreFilter = config().assignments.front();
  auto priorScore = play(40000, "previous-score");
  scoreFilter.eligibleScoreRank = 21;
  priorScore.previousScore = 21; priorScore.previousMaxScore = 27;
  CHECK(eligible(scoreFilter, priorScore));
  priorScore.previousScore = 24;
  CHECK(!eligible(scoreFilter, priorScore));
  scoreFilter.eligibleScoreOrBelow = true;
  CHECK(!eligible(scoreFilter, priorScore));
  priorScore.previousScore = 20;
  CHECK(eligible(scoreFilter, priorScore));
  priorScore.previousMaxScore = 0;
  CHECK(eligible(scoreFilter, priorScore));
  scoreFilter.eligibleScoreRank = -1; scoreFilter.eligibleScoreOrBelow = false;
  CHECK(eligible(scoreFilter, priorScore));
  priorScore.previousMaxScore = 27; priorScore.previousScore = 0;
  CHECK(!eligible(scoreFilter, priorScore));
  scoreFilter.eligibleScoreRank = 0;
  CHECK(eligible(scoreFilter, priorScore));
  for (int rank : {6, 9, 12, 15, 18, 21, 24, 26, 27}) {
    CHECK(scoreRank(rank, 27) == rank);
    CHECK(scoreRank(rank - 1, 27) < rank);
  }

  scoreFilter.eligibleLamp = 200;
  priorScore.previousLamp = 300;
  CHECK(!eligible(scoreFilter, priorScore)); // Both eligibility conditions must match.
  auto stickyConfig = config();
  stickyConfig.assignments[0].eligibleScoreRank = 21;
  stickyConfig.assignments[0].targetLamp = 400;
  State scoreSticky;
  configure(scoreSticky, stickyConfig, 40000);
  auto stickyPlay = play(40000, "score-sticky");
  stickyPlay.previousScore = 21; stickyPlay.previousMaxScore = 27;
  CHECK(record(scoreSticky, stickyPlay, 0) == 0);
  stickyPlay.previousScore = 24; stickyPlay.lamp = 400;
  CHECK(record(scoreSticky, stickyPlay, 0) == 60);
  State moving;
  configure(moving, config(60, 60, 3), 41000);
  PlayProgress receipt;
  record(moving, play(41000, "first"), 0, &receipt);
  CHECK(receipt.assignments.size() == 1 && receipt.assignments[0].beforeCharts == 0 &&
        receipt.assignments[0].afterCharts == 1 && !receipt.assignments[0].completedRound());
  record(moving, play(41000, "second"), 0);
  record(moving, play(41000, "third"), 0, &receipt);
  CHECK(receipt.assignments.size() == 1 && receipt.assignments[0].beforeCharts == 2 &&
        receipt.assignments[0].beforeQuota == 3 && receipt.assignments[0].afterQuota == 1 &&
        receipt.assignments[0].completedRound());
  RoundMotion round(receipt.assignments[0], 100);
  CHECK(std::abs(round.frame(100).fraction - 2.0F/3) < .001F);
  CHECK(round.frame(950).fraction == 1 && round.frame(950).completed && round.frame(950).round == 1);
  CHECK(round.frame(1649).completed && round.frame(1649).quota == 3);
  CHECK(round.frame(1650).round == 2 && round.frame(1650).fraction == 0 && round.frame(1650).promotion == 0);
  CHECK(!round.frame(2100).active && round.frame(2100).quota == 1 && round.frame(2100).promotion == 1);
  RoundMotion rebuilt(receipt.assignments[0], 100);
  CHECK(rebuilt.frame(1800).promotion == round.frame(1800).promotion);
  RoundMotion reduced(receipt.assignments[0], 100, true);
  CHECK(!reduced.frame(100).active && reduced.frame(100).round == 2);
  RoundPromotionCue cue;
  cue.start(receipt, 100);
  CHECK(!cue.consume(1649) && cue.consume(1650) && !cue.consume(1651));
  cue.start(receipt, 100);
  CHECK(!cue.consume(10000));
  // The receipt stays visible when this play reaches the goal or exhausts the bonus.
  CHECK(dashboardAssignmentVisible(true, false, true, receipt.assignments[0]));
  CHECK(dashboardAssignmentVisible(true, false, false, receipt.assignments[0]));
  CHECK(!dashboardAssignmentVisible(true, false, true, {}));
  CHECK(!dashboardAssignmentVisible(true, true, false, {}));
  CHECK(dashboardAssignmentVisible(true, true, true, {}));
  cue.start(receipt, 100);
  cue = {}; // Hiding the presentation cancels its cue, including after reopening.
  CHECK(!cue.consume(1650) && !cue.consume(1800));
  record(moving, play(41000, "third"), 0, &receipt);
  CHECK(receipt.assignments.empty());
  auto form = config().assignments.front();
  auto errors = readAssignmentForm(form, "", "1.5", "0", "invalid hidden value");
  CHECK(errors.contains("name") && errors.contains("count") && errors.contains("points"));
  CHECK(!errors.contains("rate"));
  CHECK(readAssignmentForm(form, "Practice", "1", "60", "invalid hidden value").empty());
  form.levels.clear();
  form.targetLamp = -1;
  form.scoreNumerator = 0;
  errors = readAssignmentForm(form, "Practice", "1", "60", "80");
  CHECK(errors.contains("levels") && errors.contains("lamp") && errors.contains("score"));
  form.levels = {{"table", "0", ""}};
  form.customRate = true;
  for (const auto *invalid : {"", "nan", "inf", "80oops", "0.001", "100.01"})
    CHECK(readAssignmentForm(form, "Practice", "1", "60", invalid).contains("rate"));
  CHECK(readAssignmentForm(form, "Practice", "1", "60", "0.01").empty());
  CHECK(form.scoreNumerator == 1 && form.scoreDenominator == 10000);
  CHECK(readAssignmentForm(form, "Practice", "1", "60", "100").empty());
  CHECK(scoreValueLabel(form) == "100.00% EX");
  form.customRate = false;
  form.scoreNumerator = 21; form.scoreDenominator = 27;
  CHECK(scoreValueLabel(form) == "AA");
  auto routineForm = config();
  errors = readRoutineForm(routineForm, "0", "100000001", "oops");
  CHECK(errors.size() == 3 && errors.contains("daily") && errors.contains("weekly") && errors.contains("monthly"));
  CHECK(readRoutineForm(routineForm, "1000000", "100000000", "100000000").empty());
  State wallet;
  wallet.savings = freezePrice(wallet) - 1;
  CHECK(!canBuyFreeze(wallet));
  ++wallet.savings;
  CHECK(canBuyFreeze(wallet));
  wallet.freezes = 2;
  CHECK(!canBuyFreeze(wallet));

  // Result skins retain design coordinates; the overlay must retain point sizes.
  const auto phone = resultOverlayMetrics(1080, 2337, 3, 1170.0F/1080, 0, 0, 0, 0);
  CHECK(std::abs(phone.width / phone.unit - 366) < 1);
  CHECK(std::abs(phone.unit - 1080.0F/390) < .01F);
  const auto desktop = resultOverlayMetrics(1920, 1080, 2, 2560.0F/1920, 0, 0, 0, 0);
  CHECK(std::abs(desktop.width / desktop.unit - 440) < 1);
  ResultPresentation drawer;
  ResultEligibility eligibility{.saved=false, .manual=true, .currentDay=true, .activeRoutine=true};
  CHECK(!drawer.arm(eligibility, 0));
  eligibility.saved = true;
  CHECK(drawer.arm(eligibility, 100));
  CHECK(drawer.active() && !drawer.open(549) && drawer.open(550));
  CHECK(!drawer.open(551) && drawer.visible());
  CHECK(drawer.fraction(970) == 1 && !drawer.animating(970));
  drawer.dismiss();
  CHECK(!drawer.active() && !drawer.arm(eligibility, 1000));
  for (int excluded = 0; excluded < 6; ++excluded) {
    auto e = eligibility;
    if (excluded == 0) e.manual = false;
    if (excluded == 1) e.practice = true;
    if (excluded == 2) e.replay = true;
    if (excluded == 3) e.course = true;
    if (excluded == 4) e.currentDay = false;
    if (excluded == 5) e.activeRoutine = false;
    CHECK(!showsResultProgress(e));
  }
  ResultPresentation earlyDismiss;
  CHECK(earlyDismiss.arm(eligibility, 0));
  earlyDismiss.dismiss();
  CHECK(!earlyDismiss.open(9999));

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
  CHECK(s.days.at(20000).assignments[0].requirement.chartCount == 1);
  CHECK(record(s, play(20000, "five"), 0) == 1);
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
  bank.savings = 45; // Isolate inventory and calendar behavior from earning.
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
  CHECK(ranks.days.at(22000).assignments[0].requirement.scoreNumerator == 24);
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
    CHECK(progress.charts == 0);
  advance(bonus, 25001);
  CHECK(bonus.days.at(25001).assignments[0].bonusRounds == 0);
  CHECK(record(bonus, play(25001, "goal"), 0) == 100 && bonus.savings == 10);

  State prices;
  configure(prices, config(100, 100), 26000);
  record(prices, play(26000, "start"), 0);
  prices.savings = 1000;
  CHECK(freezePrice(prices) == 20 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 25 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 30 && !buyFreeze(prices, 26000));
  CHECK(prices.savings == 955 && freezePrice(prices) == 30);
  advance(prices, 26003); // Both freezes used; no break yet.
  CHECK(prices.currentStreak == 1 && prices.freezes == 0 && freezePrice(prices) == 30);
  advance(prices, 26004);
  CHECK(prices.currentStreak == 0 && freezePrice(prices) == 20);
  CHECK(buyFreeze(prices, 26004) && freezePrice(prices) == 25);
  record(prices, play(26004, "restart"), 0); // Replaying old gaps must not reset price again.
  CHECK(freezePrice(prices) == 25);
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
  CHECK(buyFreeze(historical, 27003) && freezePrice(historical) == 25);
  auto lateAttendance = play(27001, "late-attendance"); lateAttendance.levels.clear();
  record(historical, lateAttendance, 0);
  CHECK(historical.currentStreak == 0 && freezePrice(historical) == 25);

  // A shared budget bounds income even with many overlapping assignments.
  State shared;
  auto many = config(1, 1);
  for (const auto &id : {"b", "c"}) {
    auto a = many.assignments[0]; a.id = id; many.assignments.push_back(a);
  }
  configure(shared, many, 28000);
  CHECK(record(shared, play(28000, "goal"), 0) == 1);
  for (int n = 0; n < 8; ++n) record(shared, play(28000, "bonus" + std::to_string(n)), 0);
  CHECK(bonusPoints(shared.days.at(28000)) == 10 && shared.savings == 10);
  CHECK(shared.days.at(28000).points == 11);
  const auto used = shared.days.at(28000).assignments[0].usedCharts.size();
  CHECK(record(shared, play(28000, "after-cap"), 0) == 0);
  CHECK(shared.days.at(28000).assignments[0].usedCharts.size() == used);
  advance(shared, 28001);
  CHECK(bonusPoints(shared.days.at(28001)) == 0);

  // Extra rounds always use one chart; unmet lamp targets never escalate.
  for (const auto &progress : shared.days.at(28000).assignments) {
    CHECK(progress.requirement.chartCount == 1);
    CHECK(progress.requirement.targetLamp == 300);
  }
  State capable;
  auto target = config(1, 1);
  target.assignments[0].scoreNumerator = 24;
  target.assignments[0].scoreDenominator = 27;
  configure(capable, target, 29000);
  auto strong = play(29000, "strong"); strong.score = 194; strong.lamp = 400;
  record(capable, strong, 0);
  CHECK(capable.days.at(29000).assignments[0].requirement.scoreNumerator == 26);
  // A custom target may advance only when the actual result meets its next +2%.
  State custom;
  target.assignments[0].customRate = true;
  target.assignments[0].scoreNumerator = 9000;
  target.assignments[0].scoreDenominator = 10000;
  configure(custom, target, 29100);
  auto justEnough = play(29100, "custom"); justEnough.score = 181;
  record(custom, justEnough, 1);
  CHECK(custom.days.at(29100).assignments[0].requirement.scoreNumerator == 9000);
  State lamp;
  configure(lamp, config(1, 1), 29200);
  strong.day = 29200;
  record(lamp, strong, 1);
  CHECK(lamp.days.at(29200).assignments[0].requirement.targetLamp == 400);

  State ceiling;
  configure(ceiling, config(1, 1), 30000);
  ceiling.savings = 1000;
  for (int n = 0; n < 10; ++n) {
    CHECK(freezePrice(ceiling) == (n < 6 ? 20 + n * 5 : 50));
    CHECK(buyFreeze(ceiling, 30000));
    ceiling.freezes = 0; // Isolate successive purchases from inventory capacity.
  }
  CHECK(freezePrice(ceiling) == 50);

  // One assignment can earn the whole shared budget without a separate cap.
  State single;
  configure(single, config(1, 1), 31000);
  record(single, play(31000, "goal"), 0);
  for (int n = 0; n < 10; ++n) record(single, play(31000, "bonus" + std::to_string(n)), 0);
  CHECK(bonusPoints(single.days.at(31000)) == 10);
  CHECK(bonusLimit(single.days.at(31000)) == 10);
  CHECK(bonusPoints(single.days.at(31000)) >= bonusLimit(single.days.at(31000)));
  CHECK(bonusLimit(shared.days.at(28000)) == 10);
  CHECK(bonusLimit(Day{}) == 0);

  // Normal quotas still apply before the goal; every extra round uses one chart.
  State fixed;
  configure(fixed, config(5, 5, 3), 32000);
  CHECK(record(fixed, play(32000, "one"), 0) == 0);
  CHECK(record(fixed, play(32000, "two"), 0) == 0);
  CHECK(record(fixed, play(32000, "three"), 0) == 5);
  CHECK(fixed.savings == 0);
  CHECK(record(fixed, play(32000, "one"), 0) == 0);
  CHECK(record(fixed, play(32000, "four"), 0) == 1);
  CHECK(fixed.days.at(32000).assignments[0].requirement.chartCount == 1);
  advance(fixed, 32001);
  CHECK(fixed.days.at(32001).assignments[0].requirement.chartCount == 3);
  CHECK(record(fixed, play(32001, "one"), 0) == 0);

  // An unfinished assignment also switches to single-chart bonus rounds.
  State unfinished;
  auto mixed = config(1, 1);
  auto longer = mixed.assignments[0]; longer.id = "long"; longer.chartCount = 4;
  mixed.assignments.push_back(longer);
  configure(unfinished, mixed, 33000);
  CHECK(record(unfinished, play(33000, "goal"), 0) == 1);
  CHECK(unfinished.days.at(33000).assignments[1].charts == 0);
  CHECK(unfinished.days.at(33000).assignments[1].requirement.chartCount == 1);
  CHECK(record(unfinished, play(33000, "extra"), 0) == 2);

  // Eligibility survives lamp promotion and table membership changes today.
  State sticky;
  auto promote = config(100, 10);
  promote.assignments[0].eligibleLamp = 200;
  promote.assignments[0].targetLamp = 400;
  configure(sticky, promote, 34000);
  auto retry = play(34000, "promote"); retry.previousLamp = 200; retry.lamp = 300;
  CHECK(record(sticky, retry, 0) == 0);
  retry.previousLamp = 300; retry.lamp = 400; retry.levels.clear();
  CHECK(record(sticky, retry, 0) == 10);
  CHECK(record(sticky, retry, 0) == 0);
  retry.day = 34001;
  CHECK(record(sticky, retry, 0) == 0);

  // Ineligible charts can qualify later, but earlier target achievements do
  // not count retroactively before full eligibility has been established.
  State newlyEligible;
  promote.assignments[0].scoreNumerator = 9;
  promote.assignments[0].scoreDenominator = 10;
  configure(newlyEligible, promote, 35000);
  retry = play(35000, "new"); retry.previousLamp = 0; retry.lamp = 200; retry.score = 200;
  CHECK(record(newlyEligible, retry, 0) == 0);
  retry.previousLamp = 200; retry.lamp = 400; retry.score = 100;
  CHECK(record(newlyEligible, retry, 0) == 0);
  retry.previousLamp = 400; retry.lamp = 0; retry.score = 180;
  CHECK(record(newlyEligible, retry, 0) == 10);
  CHECK(record(newlyEligible, retry, 0) == 0);

  State tableEntry;
  configure(tableEntry, config(100, 10), 36000);
  retry = play(36000, "table-change"); retry.levels.clear();
  CHECK(record(tableEntry, retry, 0) == 0);
  retry.levels = {{"table", "0", ""}}; retry.lamp = 0;
  CHECK(record(tableEntry, retry, 0) == 0);
  retry.levels.clear(); retry.lamp = 300;
  CHECK(record(tableEntry, retry, 0) == 10);

  // Actual achievements must meet an escalated target, not just the old one.
  State raised;
  auto targets = config(1, 1);
  targets.assignments[0].scoreNumerator = 18;
  targets.assignments[0].scoreDenominator = 27;
  configure(raised, targets, 37000);
  retry = play(37000, "partial"); retry.lamp = 200; retry.score = 140;
  CHECK(record(raised, retry, 0) == 0);
  CHECK(record(raised, play(37000, "goal"), 0) == 1);
  CHECK(raised.days.at(37000).assignments[0].requirement.scoreNumerator == 21);
  retry.lamp = 300; retry.score = 120;
  CHECK(record(raised, retry, 0) == 0);
  retry.lamp = 0; retry.score = 80; retry.maxScore = 100;
  CHECK(record(raised, retry, 0) == 1);
  CHECK(record(raised, retry, 0) == 0);
  retry.day = 37001;
  CHECK(record(raised, retry, 0) == 0);

  // A delayed earlier ineligible result cannot borrow later eligibility.
  State delayed;
  configure(delayed, promote, 38000);
  retry = play(38000, "delayed"); retry.previousLamp = 200;
  retry.lamp = 400; retry.score = 100; retry.createdAt = "2026-10-26 12:00:00.000";
  CHECK(record(delayed, retry, 0) == 0);
  retry.previousLamp = 0; retry.lamp = 200; retry.score = 200;
  retry.createdAt = "2026-10-26 11:00:00.000";
  CHECK(record(delayed, retry, 0) == 0);
  retry.previousLamp = 200; // An earlier eligible attempt can contribute.
  CHECK(record(delayed, retry, 0) == 10);

  // A stuck assignment can be adjusted today without resetting its ledger.
  State adjusted;
  configure(adjusted, config(100, 30, 3), 39000);
  record(adjusted, play(39000, "counted"), 0);
  auto replacement = adjusted.days.at(39000).assignments[0].requirement;
  replacement.levels = {{"table", "1", "sl1"}};
  replacement.chartCount = 1;
  adjustAssignment(adjusted, replacement, 39000);
  CHECK(adjusted.days.at(39000).points == 0);
  CHECK(adjusted.days.at(39000).assignments[0].charts == 1);
  CHECK(record(adjusted, play(39000, "counted"), 0) == 0);
  auto fresh = play(39000, "new"); fresh.levels[0].level = "1";
  CHECK(record(adjusted, fresh, 0) == 30);
  adjustAssignment(adjusted, replacement, 39000);
  CHECK(record(adjusted, fresh, 0) == 0);
  CHECK(adjusted.days.at(39000).points == 30);
  replacement.points = 100;
  bool rejectedReward = false;
  try { adjustAssignment(adjusted, replacement, 39000); }
  catch (const std::invalid_argument &) { rejectedReward = true; }
  CHECK(rejectedReward);
  advance(adjusted, 39001);
  CHECK(adjusted.days.at(39001).assignments[0].requirement.chartCount == 3);

  std::cout << "streak model tests passed\n";
}
