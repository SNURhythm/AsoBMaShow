#include "kombo/KomboModel.h"
#include "kombo/KomboForm.h"
#include "kombo/KomboDashboardPresentation.h"
#include "kombo/KomboRoundMotion.h"
#include "kombo/KomboIgnitionMotion.h"
#include "kombo/KomboResultPresentation.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace kombo;
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
          .lamp = 600, .score = 180, .maxScore = 200};
}
int main() {
  {
    State fixed;
    configure(fixed, config(100, 60), 44000);
    CHECK(record(fixed, play(44000, "fixed-reward"), 0) == 10);
    CHECK(fixed.days.at(44000).assignments.size() == 2);
  }

  {
    auto assignment = config().assignments.front();
    auto attempt = play(39000, "assist");
    assignment.eligibleLamp = 100;
    assignment.eligibleOrBelow = true;
    attempt.previousLamp = 150;
    CHECK(eligible(assignment, attempt));
    assignment.eligibleLamp = 50;
    attempt.previousLamp = 100;
    CHECK(!eligible(assignment, attempt));
    assignment.eligibleOrBelow = false;
    attempt.previousLamp = 150;
    CHECK(eligible(assignment, attempt));
    CHECK(lampLabel(50) == "LIGHT ASSIST EASY" && lampLabel(150) == lampLabel(50));
    assignment.eligibleLamp = -2;
    assignment.targetLamp = 100;
    attempt.lamp = 150;
    CHECK(!matches(assignment, attempt));
    assignment.targetLamp = 50;
    attempt.lamp = 100;
    CHECK(matches(assignment, attempt));
    for (const auto [initial, harderLamp] : {std::pair{0, 50}, {50, 100}, {100, 200}}) {
      State state;
      auto settings = config(1, 1);
      settings.assignments[0].targetLamp = initial;
      configure(state, settings, 39000);
      attempt.lamp = initial;
      CHECK(record(state, attempt, 0) == 10);
      CHECK(state.days.at(39000).assignments[0].requirement.targetLamp == harderLamp);
    }
    State retained;
    auto settings = config();
    settings.assignments[0].targetLamp = 200;
    configure(retained, settings, 39000);
    attempt.lamp = 100;
    CHECK(record(retained, attempt, 0) == 0);
    attempt.lamp = 150;
    CHECK(record(retained, attempt, 0) == 0);
    CHECK(retained.days.at(39000).assignments[0].chartProgress.at("assist").lamp == 100);
  }
  {
    State goals;
    Config routine;
    prepareRoutine(routine);
    CHECK(routine.assignments.size() == 1 && isGoalsAssignment(routine.assignments[0]));
    CHECK(readAssignmentForm(routine.assignments[0], "Goals", "2", "").empty());
    CHECK(readAssignmentForm(routine.assignments[0], "Goals", "0", "").contains("count"));
    CHECK(readAssignmentForm(routine.assignments[0], "Goals", "2", "").empty());
    routine.dailyGoal = 10;
    configure(goals, routine, 44000);
    auto attempt = play(44000, "same-chart");
    attempt.levels.clear();
    CHECK(record(goals, attempt, 0) == 0);
    attempt.achievedGoals = {"chart:LN:first-target"};
    CHECK(record(goals, attempt, 0) == 0);
    CHECK(goals.days.at(44000).assignments[0].charts == 1);
    CHECK(record(goals, attempt, 0) == 0); // A retried event never counts twice.
    routine.assignments[0].chartCount = 3;
    CHECK(configure(goals, routine, 44000) == 44001);
    CHECK(goals.days.at(44000).assignments[0].requirement.chartCount == 2);
    attempt.achievedGoals = {"chart:CN:first-target"};
    attempt.manual = false;
    CHECK(record(goals, attempt, 0) == 0);
    attempt.manual = true;
    PlayProgress receipt;
    CHECK(record(goals, attempt, 0, &receipt) == 10);
    CHECK(receipt.reachedDailyGoal && receipt.assignments[0].completedRound());
    CHECK(goals.savings == 0);
    attempt.achievedGoals = {"chart:LN:harder-target"};
    CHECK(record(goals, attempt, 0) == 1);
    for (int i = 0; i < 12; ++i) {
      attempt.achievedGoals = {"extra:" + std::to_string(i)};
      record(goals, attempt, 0);
    }
    CHECK(goals.days.at(44000).points == 20 && goals.savings == 10);
    CHECK(!canResetExtraChallenge(goals.days.at(44000).assignments[0]));
    bool rejected = false;
    try { adjustAssignment(goals, routine.assignments[0], 44000); }
    catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);
    advance(goals, 44001);
    CHECK(goals.days.at(44001).assignments[0].requirement.chartCount == 3);
    // Multiple achievements from one pre-goal result each earn the full reward.
    attempt.day = 44001;
    attempt.achievedGoals = {"a", "b", "c", "d", "e", "f"};
    CHECK(record(goals, attempt, 0) == 20 && goals.savings == 10);
  }
  {
    State crossing;
    auto routine=config(20);
    auto second=routine.assignments.front(); second.id="b"; second.chartCount=2;
    routine.assignments.push_back(second);
    configure(crossing,routine,45000);
    CHECK(record(crossing,play(45000,"first"),0)==10);
    PlayProgress receipt;
    CHECK(record(crossing,play(45000,"second"),0,&receipt)==20);
    CHECK(crossing.days.at(45000).points==30 && crossing.savings==0);
    CHECK(receipt.reachedDailyGoal && receipt.assignments.size()==2);
    CHECK(record(crossing,play(45000,"third"),0)==2);
    CHECK(crossing.days.at(45000).points==32 && crossing.savings==2);
  }
  State activity;
  const auto end = localDay(1735689600); // The seven slots must cross a year boundary.
  activity.configurations[end - 4] = config();
  activity.days[end - 4] = {.config=config(), .played=true};
  activity.days[end - 3] = {.config=config(), .played=true, .points=100};
  activity.days[end - 2] = {.config=config(), .frozen=true};
  activity.days[end] = {.config=config()};
  const auto week = recentActivity(activity, end);
  CHECK(week.size() == 7 && week.front().day == end - 6 && week.back().day == end);
  CHECK(week[0].kind == ActivityKind::NotStarted && week[1].kind == ActivityKind::NotStarted);
  CHECK(week[2].kind == ActivityKind::Played && week[3].kind == ActivityKind::GoalReached);
  CHECK(week[4].kind == ActivityKind::Frozen && week[5].kind == ActivityKind::NoPlay);
  CHECK(week[6].kind == ActivityKind::NoPlay);
  CHECK(nextMilestone(activity)->target == 7 && nextMilestone(activity)->remaining == 7);
  activity.currentKombo = 5;
  CHECK(nextMilestone(activity)->remaining == 2);
  activity.milestones.insert("kombo:7"); activity.currentKombo = 0;
  CHECK(nextMilestone(activity)->target == 14 && nextMilestone(activity)->remaining == 14);
  for (int target : komboMilestones) activity.milestones.insert("kombo:" + std::to_string(target));
  CHECK(!nextMilestone(activity));
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
  stickyPlay.lamp = 300;
  stickyPlay.previousScore = 21; stickyPlay.previousMaxScore = 27;
  CHECK(record(scoreSticky, stickyPlay, 0) == 0);
  stickyPlay.previousScore = 24; stickyPlay.lamp = 400;
  CHECK(record(scoreSticky, stickyPlay, 0) == 10);
  State moving;
  configure(moving, config(10, 60, 3), 41000);
  PlayProgress receipt;
  record(moving, play(41000, "first"), 0, &receipt);
  CHECK(receipt.assignments.size() == 1 && receipt.assignments[0].beforeCharts == 0 &&
        receipt.assignments[0].afterCharts == 1 && !receipt.assignments[0].completedRound());
  record(moving, play(41000, "second"), 0);
  record(moving, play(41000, "third"), 0, &receipt);
  CHECK(receipt.assignments.size() == 1 && receipt.assignments[0].beforeCharts == 2 &&
        receipt.assignments[0].beforeQuota == 3 && receipt.assignments[0].afterQuota == 1 &&
        receipt.assignments[0].completedRound());
  CHECK(receipt.reachedDailyGoal);
  PlayProgress bonusReceipt;
  record(moving, play(41000, "bonus-after-goal"), 0, &bonusReceipt);
  CHECK(!bonusReceipt.reachedDailyGoal && !bonusReceipt.assignments.empty());
  CHECK(ignitionFrame(100, 100).scale == .68F);
  CHECK(ignitionFrame(100, 190).scale < .5F);
  CHECK(ignitionFrame(100, 400).scale > 1 && ignitionFrame(100, 400).glow == 1);
  CHECK(ignitionFrame(100, 500).active && ignitionFrame(100, 500).burst > 0);
  CHECK(!ignitionFrame(100, 1000).active && ignitionFrame(100, 1000).scale == 1);
  CHECK(!ignitionFrame(100, 10000).active && !ignitionFrame({}, 400).active);
  CHECK(!ignitionFrame(100, 400, true).active && ignitionFrame(100, 400, true).glow == 0);
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
  const std::vector<Level> tableLevels{{"table", "2", "sl2"}, {"table", "10", "sl10"},
                                       {"table", "EX", "slEX"}, {"table", "?", "sl?"}};
  Assignment rangeAssignment;
  rangeAssignment.levels = {{"other", "10", "Other 10"}, tableLevels[1]};
  CHECK(!addLevelRange(rangeAssignment, tableLevels, 2, 0) && rangeAssignment.levels.size() == 2);
  CHECK(addLevelRange(rangeAssignment, tableLevels, 0, 2));
  CHECK((rangeAssignment.levels == std::vector<Level>{{"other", "10", "Other 10"},
        tableLevels[1], tableLevels[0], tableLevels[2]}));
  CHECK(addLevelRange(rangeAssignment, tableLevels, 3, 3));
  CHECK(rangeAssignment.levels.back() == tableLevels[3] && rangeAssignment.levels.size() == 5);
  const auto selected = rangeAssignment.levels;
  CHECK(addLevelRange(rangeAssignment, tableLevels, 0, 3) && rangeAssignment.levels == selected);
  CHECK(!addLevelRange(rangeAssignment, tableLevels, -1, 2) && rangeAssignment.levels == selected);
  CHECK(!addLevelRange(rangeAssignment, tableLevels, 0, 4) && rangeAssignment.levels == selected);
  CHECK(!addLevelRange(rangeAssignment, {}, 0, 0) && rangeAssignment.levels == selected);
  Assignment full;
  for (int i = 0; i < 199; ++i) full.levels.push_back({"other", std::to_string(i), ""});
  const auto beforeRange = full.levels;
  CHECK(!addLevelRange(full, tableLevels, 0, 1) && full.levels == beforeRange);
  CHECK(addLevelRange(full, tableLevels, 0, 0) && full.levels.size() == 200);
  CHECK(addLevelRange(full, tableLevels, 0, 0) && full.levels.size() == 200);
  auto errors = readAssignmentForm(form, "", "1.5", "invalid hidden value");
  CHECK(errors.contains("name") && errors.contains("count"));
  CHECK(!errors.contains("rate"));
  CHECK(readAssignmentForm(form, "Practice", "1", "invalid hidden value").empty());
  form.levels.clear();
  form.targetLamp = -1;
  form.scoreNumerator = 0;
  errors = readAssignmentForm(form, "Practice", "1", "80");
  CHECK(errors.contains("levels") && errors.contains("lamp") && errors.contains("score"));
  form.levels = {{"table", "0", ""}};
  form.customRate = true;
  for (const auto *invalid : {"", "nan", "inf", "80oops", "0.001", "100.01"})
    CHECK(readAssignmentForm(form, "Practice", "1", invalid).contains("rate"));
  CHECK(readAssignmentForm(form, "Practice", "1", "0.01").empty());
  CHECK(form.scoreNumerator == 1 && form.scoreDenominator == 10000);
  CHECK(readAssignmentForm(form, "Practice", "1", "100").empty());
  CHECK(scoreValueLabel(form) == "100.00% EX");
  form.customRate = false;
  form.scoreNumerator = 21; form.scoreDenominator = 27;
  CHECK(scoreValueLabel(form) == "AA");
  auto routineForm = config();
  errors = readRoutineForm(routineForm, "0", "100000001", "oops");
  CHECK(errors.size() == 3 && errors.contains("daily") && errors.contains("weekly") && errors.contains("monthly"));
  CHECK(readRoutineForm(routineForm, "1000000", "100000000", "100000000").empty());
  Config emptyRoutine;
  CHECK(readRoutineForm(emptyRoutine, "20", "150", "650").empty());
  State emptyState;
  configure(emptyState, emptyRoutine, 41000);
  CHECK(emptyState.days.at(41000).assignments.size() == 1);
  CHECK(isGoalsAssignment(emptyState.days.at(41000).assignments.front().requirement));
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
  configure(s, config(15, 60, 2), 20000);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  CHECK(record(s, play(20000, "two"), 0) == 10);
  CHECK(s.days.at(20000).assignments[0].completedRounds == 1);
  CHECK(record(s, play(20000, "one"), 0) == 0);
  record(s, play(20000, "three"), 0);
  CHECK(record(s, play(20000, "four"), 0) == 10);
  CHECK(s.currentKombo == 1 && s.savings == 0);
  CHECK(s.days.at(20000).assignments[0].requirement.chartCount == 1);
  CHECK(record(s, play(20000, "five"), 0) == 1);
  CHECK(s.currentKombo == 1);
  advance(s, 20001);
  CHECK(s.days.at(20001).assignments[0].requirement.chartCount == 2);
  auto ineligible = play(20001, "outside"); ineligible.levels.clear();
  CHECK(record(s, ineligible, 0) == 0);
  advance(s, 20002);
  CHECK(s.currentKombo == 1 && s.freezes == 0);
  advance(s, 20003);
  CHECK(s.currentKombo == 0 && s.bestKombo == 1);
  // A delayed durable save belongs to its actual day and can repair that gap.
  record(s, play(20002, "late"), 0);
  CHECK(s.currentKombo == 1);

  State bank;
  configure(bank, config(10, 300), 21000);
  record(bank, play(21000, "a"), 0);
  CHECK(bank.savings == 0 && bank.currentKombo == 1);
  bank.savings = 45; // Isolate inventory and calendar behavior from earning.
  CHECK(buyFreeze(bank, 21000) && buyFreeze(bank, 21000));
  CHECK(!buyFreeze(bank, 21000));
  CHECK(bank.savings == 0 && bank.days.at(21000).points == 10);
  advance(bank, 21002);
  CHECK(bank.freezes == 1 && bank.days.at(21001).frozen);
  CHECK(bank.currentKombo == 1);
  record(bank, play(21001, "late"), 0);
  CHECK(bank.freezes == 2 && !bank.days.at(21001).frozen);
  CHECK(bank.currentKombo == 2);
  auto effective = configure(bank, config(200, 100), 21002);
  CHECK(effective == 21003);
  record(bank, play(21002, "b"), 0);
  CHECK(configure(bank, config(500, 100), 21002) == 21003);
  CHECK(bank.days.at(21002).config.dailyGoal == 10);
  advance(bank, 21003);
  CHECK(bank.days.at(21003).config.dailyGoal == 500);
  advance(bank, 21000); // A clock rollback cannot reroll today's assignments.
  CHECK(bank.days.rbegin()->first == 21003);

  State recovered;
  configure(recovered, config(10, 200), 21500);
  record(recovered, play(21500, "start"), 0);
  recovered.savings = 20;
  CHECK(buyFreeze(recovered, 21500));
  advance(recovered, 21503);
  CHECK(recovered.currentKombo == 0 && recovered.freezes == 0);
  auto recoveredPlay = play(21501, "late"); recoveredPlay.levels.clear();
  record(recovered, recoveredPlay, 0);
  CHECK(recovered.currentKombo == 1 && recovered.freezes == 0);
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
  CHECK(record(ranks, p, 1) == 10);
  CHECK(ranks.days.at(22000).assignments[0].requirement.scoreNumerator == 26);
  p.chart = "cleared-before"; p.previousLamp = 300; p.score = 100;
  CHECK(record(ranks, p, 1) == 0);
  State automatic;
  configure(automatic, config(), 22000);
  p = play(22000, "auto"); p.manual = false;
  record(automatic, p, 0);
  CHECK(!automatic.days.at(22000).played);

  State milestones;
  configure(milestones, config(10, 100), 23000);
  for (int d = 23000; d < 23007; ++d) record(milestones, play(d, "same-next-day"), 0);
  CHECK(milestones.currentKombo == 7 && milestones.milestones.contains("kombo:7"));
  advance(milestones, 23009);
  CHECK(milestones.currentKombo == 0 && milestones.milestones.contains("kombo:7"));
  CHECK(!validate(config(0)).empty());
  auto invalid = config(); invalid.assignments[0].scoreDenominator = 0;
  CHECK(!validate(invalid).empty());
  // Even edits before any play wait until tomorrow; the last save wins.
  State edits;
  CHECK(configure(edits, config(100, 60), 24000) == 24000);
  CHECK(configure(edits, config(200, 90), 24000) == 24001);
  CHECK(edits.days.at(24000).config.dailyGoal == 100);
  CHECK(configure(edits, config(300, 120), 24000) == 24001);
  CHECK(record(edits, play(24000, "today"), 0) == 10);
  advance(edits, 24001);
  CHECK(edits.days.at(24001).config.dailyGoal == 300);
  CHECK(edits.days.at(24001).assignments[0].requirement.points == 10);

  // Overlapping completions on the goal-reaching play cannot mint savings.
  State bonus;
  auto overlapping = config(15, 80);
  auto second = overlapping.assignments[0]; second.id = "b";
  overlapping.assignments.push_back(second);
  configure(bonus, overlapping, 25000);
  CHECK(record(bonus, play(25000, "goal"), 0) == 20);
  CHECK(bonus.savings == 0);
  for (int n = 0; n < 100; ++n) {
    auto extra = play(25000, "extra" + std::to_string(n)); extra.lamp = 600;
    record(bonus, extra, 0);
  }
  CHECK(bonus.savings == 10 && bonus.days.at(25000).points == 30);
  for (const auto &progress : bonus.days.at(25000).assignments)
    CHECK(progress.charts == 0);
  advance(bonus, 25001);
  CHECK(bonus.days.at(25001).assignments[0].bonusRounds == 0);
  CHECK(record(bonus, play(25001, "goal"), 0) == 20 && bonus.savings == 10);

  State prices;
  configure(prices, config(10, 100), 26000);
  record(prices, play(26000, "start"), 0);
  prices.savings = 1000;
  CHECK(freezePrice(prices) == 20 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 25 && buyFreeze(prices, 26000));
  CHECK(freezePrice(prices) == 30 && !buyFreeze(prices, 26000));
  CHECK(prices.savings == 955 && freezePrice(prices) == 30);
  advance(prices, 26003); // Both freezes used; no break yet.
  CHECK(prices.currentKombo == 1 && prices.freezes == 0 && freezePrice(prices) == 30);
  advance(prices, 26004);
  CHECK(prices.currentKombo == 0 && freezePrice(prices) == 20);
  CHECK(buyFreeze(prices, 26004) && freezePrice(prices) == 25);
  record(prices, play(26004, "restart"), 0); // Replaying old gaps must not reset price again.
  CHECK(freezePrice(prices) == 25);
  advance(prices, 26007); // Another kombo, another actual break.
  CHECK(prices.currentKombo == 0 && freezePrice(prices) == 20);
  prices.savings = 19;
  CHECK(!buyFreeze(prices, 26007) && freezePrice(prices) == 20);

  // Late attendance can move the historical gap without breaking a new kombo.
  State historical;
  configure(historical, config(10, 100), 27000);
  record(historical, play(27000, "start"), 0);
  advance(historical, 27003);
  historical.savings = 100;
  CHECK(buyFreeze(historical, 27003) && freezePrice(historical) == 25);
  auto lateAttendance = play(27001, "late-attendance"); lateAttendance.levels.clear();
  record(historical, lateAttendance, 0);
  CHECK(historical.currentKombo == 0 && freezePrice(historical) == 25);

  // A shared budget bounds income even with many overlapping assignments.
  State shared;
  auto many = config(1, 1);
  for (const auto &id : {"b", "c"}) {
    auto a = many.assignments[0]; a.id = id; many.assignments.push_back(a);
  }
  configure(shared, many, 28000);
  CHECK(record(shared, play(28000, "goal"), 0) == 30);
  for (int n = 0; n < 8; ++n) record(shared, play(28000, "bonus" + std::to_string(n)), 0);
  CHECK(bonusPoints(shared.days.at(28000)) == 10 && shared.savings == 10);
  CHECK(shared.days.at(28000).points == 40);
  const auto used = shared.days.at(28000).assignments[0].usedCharts.size();
  CHECK(record(shared, play(28000, "after-cap"), 0) == 0);
  CHECK(shared.days.at(28000).assignments[0].usedCharts.size() == used);
  advance(shared, 28001);
  CHECK(bonusPoints(shared.days.at(28001)) == 0);

  // Bonus mode raises a condition once even if the goal play did not meet it.
  for (const auto &progress : shared.days.at(28000).assignments) {
    CHECK(progress.requirement.chartCount == 1);
    CHECK(isGoalsAssignment(progress.requirement) || progress.requirement.targetLamp == 400);
  }
  State capable;
  auto target = config(1, 1);
  target.assignments[0].scoreNumerator = 24;
  target.assignments[0].scoreDenominator = 27;
  configure(capable, target, 29000);
  auto strong = play(29000, "strong"); strong.score = 194; strong.lamp = 400;
  record(capable, strong, 0);
  CHECK(capable.days.at(29000).assignments[0].requirement.scoreNumerator == 26);
  // A custom target advances by 2% without a performance-based promotion gate.
  State custom;
  target.assignments[0].customRate = true;
  target.assignments[0].scoreNumerator = 9000;
  target.assignments[0].scoreDenominator = 10000;
  configure(custom, target, 29100);
  auto justEnough = play(29100, "custom"); justEnough.score = 181;
  record(custom, justEnough, 0);
  CHECK(custom.days.at(29100).assignments[0].requirement.scoreNumerator == 9200);
  State lamp;
  configure(lamp, config(1, 1), 29200);
  strong.day = 29200;
  record(lamp, strong, 1);
  CHECK(lamp.days.at(29200).assignments[0].requirement.targetLamp == 400);

  // One random condition is raised when every assignment enters bonus mode.
  for (unsigned choice = 0; choice < 3; ++choice) {
    State once;
    auto base = config(1, 1, 3);
    base.assignments[0].scoreNumerator = 18;
    base.assignments[0].scoreDenominator = 27;
    configure(once, base, 42000);
    auto goalPlay = play(42000, "g0");
    goalPlay.lamp = 300; goalPlay.score = 18; goalPlay.maxScore = 27;
    goalPlay.nextLevels = {{{"table", "0", "sl0"}, {"table", "1", "sl1"}}};
    for (int i = 0; i < 3; ++i) { goalPlay.chart = "g" + std::to_string(i); record(once, goalPlay, choice); }
    auto &p = once.days.at(42000).assignments[0];
    CHECK(p.bonusDifficultyApplied && p.requirement.chartCount == 1);
    CHECK(p.requirement.scoreNumerator == (choice == 0 ? 21 : 18));
    CHECK(p.requirement.targetLamp == (choice == 1 ? 400 : 300));
    CHECK(p.requirement.levels[0].level == (choice == 2 ? "1" : "0"));
    auto bonusPlay = goalPlay;
    bonusPlay.lamp = 600; bonusPlay.score = 27;
    bonusPlay.levels = p.requirement.levels;
    for (int i = 0; i < 2; ++i) {
      bonusPlay.chart = "b" + std::to_string(i);
      CHECK(record(once, bonusPlay, choice + 1) == 1);
    }
    CHECK(p.requirement.scoreNumerator == (choice == 0 ? 21 : 18));
    CHECK(p.requirement.targetLamp == (choice == 1 ? 400 : 300));
    CHECK(p.requirement.levels[0].level == (choice == 2 ? "1" : "0"));
    const auto earned = once.days.at(42000).points;
    resetExtraChallenge(once, "a", 42000);
    CHECK(p.requirement.targetLamp == 300 && p.requirement.scoreNumerator == 18);
    CHECK(p.requirement.levels[0].level == "0" && p.requirement.chartCount == 2);
    CHECK(p.bonusChartCount == 2 && p.charts == 0 && once.days.at(42000).points == earned);
    auto easyPlay = goalPlay;
    easyPlay.chart = "easy0";
    PlayProgress partialReceipt;
    CHECK(record(once, easyPlay, 0, &partialReceipt) == 0 && p.charts == 1);
    CHECK(partialReceipt.assignments[0].afterQuota == 2 && partialReceipt.assignments[0].afterCharts == 1);
    CHECK(record(once, easyPlay, 0) == 0 && p.charts == 1); // Distinct charts only.
    easyPlay.chart = "easy1";
    CHECK(record(once, easyPlay, 0) == 1 && p.charts == 0);
    CHECK(p.requirement.scoreNumerator == 18 && p.requirement.targetLamp == 300);
    CHECK(p.requirement.chartCount == 2 && p.requirement.levels[0].level == "0");
    advance(once, 42001);
    const auto &tomorrow = once.days.at(42001).assignments[0];
    CHECK(!tomorrow.bonusDifficultyApplied && tomorrow.bonusChartCount == 1);
    CHECK(tomorrow.requirement.chartCount == 3 && tomorrow.requirement.targetLamp == 300);
    CHECK(tomorrow.requirement.scoreNumerator == 18 && tomorrow.requirement.levels[0].level == "0");
  }

  // Today's availability edits cannot replace the immutable reset target.
  State editedBonus;
  configure(editedBonus, config(1, 1), 43000);
  record(editedBonus, play(43000, "goal"), 0);
  auto edited = editedBonus.days.at(43000).assignments[0].requirement;
  edited.levels = {{"other", "7", "Other 7"}};
  adjustAssignment(editedBonus, edited, 43000);
  resetExtraChallenge(editedBonus, "a", 43000);
  const auto &restored = editedBonus.days.at(43000).assignments[0];
  CHECK(restored.requirement.targetLamp == 300 && restored.requirement.levels[0].level == "0");
  CHECK(restored.requirement.chartCount == 2 && !canResetExtraChallenge(restored));

  // A chart in an overlapping table level keeps its NO PLAY eligibility.
  State overlap;
  auto range = config(1, 1);
  range.assignments[0].levels.push_back({"table", "1", "sl1"});
  range.assignments[0].eligibleLamp = -1;
  range.assignments[0].targetLamp = 400;
  configure(overlap, range, 44000);
  auto retained = play(44000, "retained"); retained.levels = {{"table", "1", "sl1"}}; retained.lamp = 300;
  CHECK(record(overlap, retained, 0) == 0);
  auto removed = retained; removed.chart = "removed"; removed.levels[0].level = "0";
  CHECK(record(overlap, removed, 0) == 0);
  auto promoteRange = play(44000, "goal"); promoteRange.lamp = 400;
  promoteRange.nextLevels = {{{"table","0","sl0"},{"table","1","sl1"}},
                            {{"table","1","sl1"},{"table","2","sl2"}}};
  CHECK(record(overlap, promoteRange, 1) == 10);
  CHECK(overlap.days.at(44000).assignments[0].chartProgress.contains("retained"));
  CHECK(!overlap.days.at(44000).assignments[0].chartProgress.contains("removed"));
  retained.previousLamp = 300; retained.lamp = 400;
  CHECK(record(overlap, retained, 0) == 1);

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
  CHECK(record(fixed, play(32000, "three"), 0) == 10);
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
  CHECK(record(unfinished, play(33000, "goal"), 0) == 10);
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
  CHECK(record(raised, play(37000, "goal"), 0) == 10);
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
  CHECK(record(adjusted, fresh, 0) == 10);
  adjustAssignment(adjusted, replacement, 39000);
  CHECK(record(adjusted, fresh, 0) == 0);
  CHECK(adjusted.days.at(39000).points == 10);
  replacement.points = 100;
  bool rejectedReward = false;
  try { adjustAssignment(adjusted, replacement, 39000); }
  catch (const std::invalid_argument &) { rejectedReward = true; }
  CHECK(rejectedReward);
  advance(adjusted, 39001);
  CHECK(adjusted.days.at(39001).assignments[0].requirement.chartCount == 3);

  std::cout << "kombo model tests passed\n";
}
