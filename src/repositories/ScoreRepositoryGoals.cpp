#include "ScoreRepositoryInternal.h"
#include "../goals/ChartGoalStore.h"
#include "../LongNoteModeUtils.h"
#include "SqliteRAII.h"

namespace {
int goalMode(const bms_parser::ChartMeta &chart, int selected) {
  return scoreLongNoteModeForClearLamp(chart, long_note_mode::normalizeSelectedValue(selected));
}
chart_goals::GoalProgress project(sqlite3 *db, chart_goals::Goal goal) {
  auto bestScore = score_repository_detail::LoadBestScoreOnConnection(
      db, goal.chart, {}, {}, goal.longNoteMode);
  auto bestClear = score_repository_detail::LoadBestClearScoreOnConnection(
      db, goal.chart, {}, {}, goal.longNoteMode, {}, true);
  auto progress = chart_goals::evaluate(goal.target, std::move(bestScore), std::move(bestClear));
  return {std::move(goal), std::move(progress)};
}
}
std::optional<chart_goals::ProgressPage> ScoreRepository::LoadChartGoals(int offset, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle tx(impl_->sessionDatabase, "BEGIN", error);
  if (!tx.active()) return {};
  try {
    auto stored = chart_goals::loadPage(impl_->sessionDatabase, offset);
    chart_goals::ProgressPage result{.hasMore = stored.hasMore};
    for (auto &goal : stored.goals) result.goals.push_back(project(impl_->sessionDatabase, std::move(goal)));
    if (!tx.commit(error)) return {};
    return result;
  } catch (const std::exception &failure) { error = failure.what(); return {}; }
}
std::optional<chart_goals::GoalProgress> ScoreRepository::LoadChartGoal(
    const bms_parser::ChartMeta &chart, int selectedMode, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle tx(impl_->sessionDatabase, "BEGIN", error);
  if (!tx.active()) return {};
  try {
    auto goal = chart_goals::load(impl_->sessionDatabase, chart.SHA256, goalMode(chart, selectedMode));
    std::optional<chart_goals::GoalProgress> result;
    if (goal) result = project(impl_->sessionDatabase, std::move(*goal));
    if (!tx.commit(error)) return {};
    return result;
  } catch (const std::exception &failure) { error = failure.what(); return {}; }
}
bool ScoreRepository::SaveChartGoal(const chart_goals::Goal &goal, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  try {
    auto normalized = goal;
    normalized.chart.SHA256 = chart_goals::normalizedHash(goal.chart.SHA256);
    normalized.longNoteMode = goalMode(goal.chart, goal.longNoteMode);
    chart_goals::save(impl_->sessionDatabase, normalized);
    score_repository_detail::IncrementRevision();
    return true;
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::RemoveChartGoal(const bms_parser::ChartMeta &chart, int selectedMode, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  try {
    chart_goals::remove(impl_->sessionDatabase, chart.SHA256, goalMode(chart, selectedMode));
    score_repository_detail::IncrementRevision();
    return true;
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
