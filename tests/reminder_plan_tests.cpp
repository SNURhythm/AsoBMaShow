#include "notifications/ReminderPlan.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
  using namespace reminders;
  kombo::State state;
  Preferences prefs;
  CHECK(makePlan(state, prefs, 20000, 0).empty());
  prefs.daily = prefs.comeback = true;
  CHECK(makePlan(state, prefs, 20000, 0).empty());
  state.configurations[20000] = {};
  state.days[20000] = {};
  auto plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 56 && plan.front().day == 20000);
  CHECK(makePlan(state, prefs, 20000, 1200).front().day == 20001);
  state.currentKombo = 3;
  plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 4 && plan[0].kind == Kind::Daily);
  CHECK(plan[1].day == 20003 && plan[2].day == 20008 && plan[3].day == 20015);
  state.freezes = 2;
  plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 6 && plan[3].day == 20005);
  state.days[20000].played = true;
  plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 7 && plan[4].day == 20006);
  state.days[20000].points = 20;
  plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 6 && plan.front().day == 20001);
  prefs.daily = false;
  plan = makePlan(state, prefs, 20000, 0);
  CHECK(plan.size() == 3 && plan.front().day == 20006);
  state.currentKombo = 0;
  CHECK(makePlan(state, prefs, 20000, 0).empty());
  // A late result can refund a freeze after the player has replenished the
  // inventory. Two limits purchases, not the balance used by advance().
  kombo::State refunded;
  kombo::Config routine;
  routine.dailyGoal = 1;
  routine.assignments = {{.id="practice", .name="Practice", .levels={{"table", "0", "sl0"}}, .points=1}};
  const kombo::DayId start = 20000;
  kombo::configure(refunded, routine, start);
  kombo::record(refunded, {.day=start, .chart="first", .levels={{"table", "0", "sl0"}}, .lamp=300}, 0);
  refunded.savings = 100;
  CHECK(kombo::buyFreeze(refunded, start) && kombo::buyFreeze(refunded, start));
  kombo::advance(refunded, start + 2);
  CHECK(refunded.days.at(start + 1).frozen && refunded.freezes == 1);
  CHECK(kombo::buyFreeze(refunded, start + 2));
  kombo::record(refunded, {.day=start + 1, .chart="delayed", .levels={{"table", "0", "sl0"}}, .lamp=300}, 0);
  CHECK(refunded.freezes == 3 && refunded.currentKombo == 2);
  prefs.daily = true;
  const auto refundedPlan = makePlan(refunded, prefs, start + 2, 0);
  int refundedLoss = start + 2;
  while (refunded.currentKombo > 0) kombo::advance(refunded, ++refundedLoss);
  const auto dailyCount = std::count_if(refundedPlan.begin(), refundedPlan.end(),
      [](const auto &entry) { return entry.kind == Kind::Daily; });
  CHECK(dailyCount == refundedLoss - (start + 2));
  CHECK(refundedPlan.size() == dailyCount + 3);
  CHECK(refundedPlan[dailyCount].day == refundedLoss + 2);
  CHECK(refundedPlan[dailyCount + 1].day == refundedLoss + 7);
  CHECK(refundedPlan[dailyCount + 2].day == refundedLoss + 14);
  // Compare forecasting against the real model across a year boundary. A
  // partial play protects the day but does not increment the combo.
  prefs.daily = false;
  for (int freezes = 0; freezes <= 2; ++freezes) {
    for (bool played : {false, true}) {
      kombo::State actual;
      const kombo::DayId day = 20453; // 2025-12-31
      actual.configurations[day - 1] = {};
      actual.days[day - 1] = {.played = true, .points = 20};
      actual.days[day] = {.played = played};
      actual.currentKombo = 1;
      actual.freezes = freezes;
      const auto forecast = makePlan(actual, prefs, day, 0);
      CHECK(forecast.size() == 3);
      int lost = day;
      while (actual.currentKombo > 0) kombo::advance(actual, ++lost);
      CHECK(forecast[0].day == lost + 2 && forecast[1].day == lost + 7 && forecast[2].day == lost + 14);
      CHECK(makePlan(actual, prefs, lost, 0).empty()); // Return cancels the old lost-combo series.
    }
  }
  kombo::State rollback;
  const kombo::DayId monday = 20753;
  rollback.configurations[monday - 1] = {};
  rollback.days[monday - 1] = {.played = true, .points = 20};
  rollback.days[monday] = {};
  rollback.currentKombo = 1;
  rollback.freezes = 2;
  kombo::advance(rollback, monday + 1); // Monday's freeze is already consumed.
  CHECK(rollback.days[monday].frozen && rollback.freezes == 1);
  auto backInMonday = makePlan(rollback, prefs, monday, 0);
  int actualLoss = monday + 1;
  while (rollback.currentKombo > 0) kombo::advance(rollback, ++actualLoss);
  CHECK(backInMonday.front().day == actualLoss + 2);
  CHECK(backInMonday.back().day == actualLoss + 14);
  prefs.daily = true;
  rollback.currentKombo = 1;
  rollback.freezes = 100;
  rollback.days[monday + 100] = {.played = true, .points = 20};
  const auto farRollback = makePlan(rollback, prefs, monday, 0);
  CHECK(farRollback.size() == 56);
  CHECK(farRollback.back().kind == Kind::Comeback);
  CHECK(farRollback.back().day == monday + 216);
  prefs.hour = 24;
  CHECK(!prefs.valid());
}
