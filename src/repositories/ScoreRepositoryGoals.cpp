#include "ScoreRepositoryInternal.h"
#include "../goals/ChartGoalStore.h"
#include "../LongNoteModeUtils.h"
#include "SqliteRAII.h"
#include <SDL3/SDL.h>

namespace {
int goalMode(const bms_parser::ChartMeta &chart, int selected) {
  return scoreLongNoteModeForClearLamp(chart, long_note_mode::normalizeSelectedValue(selected));
}
bms_parser::ChartMeta goalScoreIdentity(const bms_parser::ChartMeta &chart) {
  auto identity = chart;
  // Stored goals contain hashes but not paths; use the same evidence before saving.
  identity.BmsPath.clear();
  return identity;
}
chart_goals::GoalProgress project(sqlite3 *db, chart_goals::Goal goal,
                                  const std::optional<std::string> &excludeAttempt = {}) {
  const auto identity = goalScoreIdentity(goal.chart);
  auto bestScore = score_repository_detail::LoadBestScoreOnConnection(
      db, identity, {}, excludeAttempt, goal.longNoteMode);
  auto bestClear = score_repository_detail::LoadBestClearScoreOnConnection(
      db, identity, {}, excludeAttempt, goal.longNoteMode, {}, true);
  auto progress = chart_goals::evaluate(goal.target, std::move(bestScore), std::move(bestClear));
  return {std::move(goal), std::move(progress)};
}
}
std::vector<chart_goals::Goal> score_repository_detail::RecordChartGoalAchievementsOnConnection(
    sqlite3 *db, std::string_view sha256, std::string_view createdAt, std::string_view attemptId) {
  if (sha256.empty()) return {};
  std::string error;
  SqliteTransactionHandle tx(db, "SAVEPOINT goal_achievement", error,
      "RELEASE goal_achievement", "ROLLBACK TO goal_achievement; RELEASE goal_achievement");
  if (!tx.active()) { SDL_Log("Cannot record goal achievement: %s", error.c_str()); return {}; }
  try {
    auto goals = chart_goals::loadForChart(db, sha256);
    std::vector<chart_goals::Goal> achieved;
    auto timestamp = chart_goals::nowUnixMillis();
    if (!goals.empty() && !createdAt.empty()) {
      SqliteStatementHandle time;
      if (prepareSqliteStatement(db, "SELECT CAST(strftime('%s', ?1) AS INTEGER)*1000 + CAST(substr(strftime('%f', ?1),4,3) AS INTEGER)", time) != SQLITE_OK ||
          !bindSqliteText(time, 1, std::string(createdAt)) || sqlite3_step(time) != SQLITE_ROW)
        throw std::runtime_error("Cannot read goal achievement time.");
      if (sqlite3_column_type(time, 0) != SQLITE_NULL) timestamp = sqlite3_column_int64(time, 0);
    }
    for (auto &goal : goals) {
      if (goal.achievedAtUnixMillis != 0 || !project(db, goal).progress.achieved) continue;
      // An undated legacy achievement is not a new completion by this play.
      if (!attemptId.empty()) {
        if (project(db, goal, std::string(attemptId)).progress.achieved) continue;
      } else if (goal.setAtUnixMillis == 0) continue;
      goal.achievedAtUnixMillis = std::max(goal.setAtUnixMillis, timestamp);
      chart_goals::save(db, goal);
      achieved.push_back(std::move(goal));
    }
    if (!tx.commit(error)) throw std::runtime_error(error);
    return achieved;
  } catch (const std::exception &failure) {
    SDL_Log("Cannot record goal achievement: %s", failure.what());
    return {};
  }
}
std::vector<chart_goals::Goal> ScoreRepository::TakeChartGoalAchievements(std::string_view attemptId) {
  std::lock_guard lock(impl_->sessionMutex);
  if (attemptId.empty() || attemptId != impl_->chartGoalAchievementAttempt) return {};
  impl_->chartGoalAchievementAttempt.clear();
  return std::exchange(impl_->chartGoalAchievements, {});
}
std::optional<chart_goals::Index> ScoreRepository::LoadChartGoalIndex(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle tx(impl_->sessionDatabase, "BEGIN", error);
  if (!tx.active()) return {};
  try {
    auto result = chart_goals::loadIndex(impl_->sessionDatabase);
    for (const auto &[hash, mask] : result.modes)
      for (const auto &goal : chart_goals::loadForChart(impl_->sessionDatabase, hash))
        if (project(impl_->sessionDatabase, goal).progress.achieved)
          result.achievedModes[hash] |= 1u << goal.longNoteMode;
    if (!tx.commit(error)) return {};
    return result;
  }
  catch (const std::exception &failure) { error = failure.what(); return {}; }
}
std::optional<chart_goals::GoalProgress> ScoreRepository::PreviewChartGoal(
    const chart_goals::Goal &goal, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle tx(impl_->sessionDatabase, "BEGIN", error);
  if (!tx.active()) return {};
  try {
    auto normalized = goal;
    normalized.longNoteMode = goalMode(goal.chart, goal.longNoteMode);
    // A new editor has no target yet; its current records are still useful.
    chart_goals::GoalProgress result{.goal=std::move(normalized)};
    const auto identity = goalScoreIdentity(result.goal.chart);
    result.progress.bestScore = score_repository_detail::LoadBestScoreOnConnection(
        impl_->sessionDatabase, identity, {}, {}, result.goal.longNoteMode);
    result.progress.bestClear = score_repository_detail::LoadBestClearScoreOnConnection(
        impl_->sessionDatabase, identity, {}, {}, result.goal.longNoteMode, {}, true);
    if (!tx.commit(error)) return {};
    return result;
  } catch (const std::exception &failure) { error = failure.what(); return {}; }
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
  SqliteTransactionHandle tx(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!tx.active()) return false;
  try {
    auto normalized = goal;
    normalized.chart.SHA256 = chart_goals::normalizedHash(goal.chart.SHA256);
    normalized.longNoteMode = goalMode(goal.chart, goal.longNoteMode);
    const auto previous = chart_goals::load(impl_->sessionDatabase, normalized.chart.SHA256, normalized.longNoteMode);
    const bool unchanged = previous && previous->target == normalized.target;
    normalized.setAtUnixMillis = unchanged ? previous->setAtUnixMillis : chart_goals::nowUnixMillis();
    normalized.achievedAtUnixMillis = unchanged ? previous->achievedAtUnixMillis : 0;
    if (!unchanged && project(impl_->sessionDatabase, normalized).progress.achieved) {
      error = "goal_already_achieved";
      return false;
    }
    chart_goals::save(impl_->sessionDatabase, normalized);
    if (!tx.commit(error)) return false;
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
