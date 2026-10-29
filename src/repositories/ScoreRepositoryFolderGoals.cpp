#include "ScoreRepositoryInternal.h"
#include "../goals/ChartGoal.h"
#include <algorithm>
#include <stdexcept>

folder_goals::Projector &ScoreRepository::Impl::folderProjector() {
  if (!folderGoalProjector) {
    sqlite3 *raw=nullptr;
    const auto text=chartDatabasePath.u8string();
    const int rc=sqlite3_open_v2(reinterpret_cast<const char *>(text.c_str()),&raw,SQLITE_OPEN_READONLY,nullptr);
    folderGoalCharts.reset(raw);
    if (rc!=SQLITE_OK) { folderGoalCharts.reset(); throw std::runtime_error("Chart library is unavailable."); }
    folderGoalProjector=std::make_unique<folder_goals::Projector>(raw);
  }
  folderGoalProjector->refresh();
  return *folderGoalProjector;
}
std::vector<folder_goals::GoalProgress> ScoreRepository::Impl::folderGoalsBeforePlay(std::string_view hash) {
  auto goals=folder_goals::load(sessionDatabase);
  std::vector<folder_goals::GoalProgress> out;
  if (goals.empty()) return out;
  auto &projection=folderProjector();
  std::optional<folder_goals::Evidence> evidence;
  for (const auto &g:goals) {
    if (g.achievedAt || !projection.contains(g.source,hash)) continue;
    if (!evidence) evidence=folder_goals::loadEvidence(sessionDatabase);
    out.push_back({g,projection.project(g,*evidence)});
  }
  return out;
}
std::vector<folder_goals::Goal> ScoreRepository::Impl::folderGoalsAfterPlay(
    const std::vector<folder_goals::GoalProgress> &before, std::string_view createdAt) {
  std::vector<folder_goals::Goal> out;
  if (before.empty()) return out;
  const auto evidence=folder_goals::loadEvidence(sessionDatabase);
  SqliteStatementHandle time;
  if (prepareSqliteStatement(sessionDatabase,
      "SELECT CAST(strftime('%s',?1) AS INTEGER)*1000+CAST(substr(strftime('%f',?1),4,3) AS INTEGER)",time)!=SQLITE_OK ||
      !bindSqliteText(time,1,std::string(createdAt)) || sqlite3_step(time)!=SQLITE_ROW || sqlite3_column_type(time,0)==SQLITE_NULL)
    throw std::runtime_error("Cannot read folder goal achievement time.");
  const auto timestamp=sqlite3_column_int64(time,0);
  // Use the same membership snapshot on both sides of this score write.
  for (const auto &entry:before) {
    if (!entry.progress.achieved && !folderGoalProjector->project(entry.goal,evidence).achieved) continue;
    auto goal=entry.goal;
    goal.achievedAt=std::max<std::int64_t>(goal.setAt,timestamp);
    folder_goals::save(sessionDatabase,goal);
    if (!entry.progress.achieved) out.push_back(std::move(goal));
  }
  return out;
}
std::optional<folder_goals::GoalProgress> ScoreRepository::PreviewFolderGoal(
    const folder_goals::Goal &goal,std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error="Score storage is unavailable."; return {}; }
  try {
    folder_goals::validate(goal);
    SqliteTransactionHandle tx(impl_->sessionDatabase,"BEGIN",error);
    if (!tx.active()) return {};
    auto progress=impl_->folderProjector().project(goal,folder_goals::loadEvidence(impl_->sessionDatabase));
    if (!tx.commit(error)) return {};
    return folder_goals::GoalProgress{goal,progress};
  } catch (const std::exception &e) { error=e.what(); return {}; }
}
std::optional<std::vector<folder_goals::GoalProgress>> ScoreRepository::LoadFolderGoals(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error="Score storage is unavailable."; return {}; }
  try {
    SqliteTransactionHandle tx(impl_->sessionDatabase,"BEGIN IMMEDIATE",error);
    if (!tx.active()) return {};
    auto goals=folder_goals::load(impl_->sessionDatabase);
    std::vector<folder_goals::GoalProgress> out;
    if (!goals.empty()) {
      auto &projection=impl_->folderProjector();
      const auto evidence=folder_goals::loadEvidence(impl_->sessionDatabase);
      for (auto &g:goals) {
        auto progress=projection.project(g,evidence);
        if (progress.achieved && !g.achievedAt) {
          g.achievedAt=std::max(g.setAt,chart_goals::nowUnixMillis());
          folder_goals::save(impl_->sessionDatabase,g); // History only, never play credit.
        }
        out.push_back({std::move(g),progress});
      }
    }
    if (!tx.commit(error)) return {};
    return out;
  } catch (const std::exception &e) { error=e.what(); return {}; }
}
bool ScoreRepository::SaveFolderGoal(const folder_goals::Goal &input,std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error="Score storage is unavailable."; return false; }
  try {
    auto goal=input;
    folder_goals::validate(goal);
    SqliteTransactionHandle tx(impl_->sessionDatabase,"BEGIN IMMEDIATE",error);
    if (!tx.active()) return false;
    const auto goals=folder_goals::load(impl_->sessionDatabase);
    auto previous=std::find_if(goals.begin(),goals.end(),[&](const auto &g){ return folder_goals::key(g)==folder_goals::key(goal); });
    const bool unchanged=previous!=goals.end() && previous->target==goal.target;
    auto progress=impl_->folderProjector().project(goal,folder_goals::loadEvidence(impl_->sessionDatabase));
    if (!progress.sourceAvailable || !progress.available) throw std::runtime_error("This folder has no available charts.");
    if (!unchanged && progress.achieved) throw std::runtime_error("Choose a target above your current progress.");
    goal.revision=previous==goals.end() ? 1 : previous->revision+(unchanged ? 0 : 1);
    goal.setAt=unchanged ? previous->setAt : std::max<std::int64_t>(chart_goals::nowUnixMillis(),previous==goals.end() ? 0 : previous->setAt+1);
    goal.achievedAt=unchanged ? previous->achievedAt : 0;
    folder_goals::save(impl_->sessionDatabase,goal);
    if (!tx.commit(error)) return false;
    score_repository_detail::IncrementRevision();
    return true;
  } catch (const std::exception &e) { error=e.what(); return false; }
}
bool ScoreRepository::RemoveFolderGoal(const folder_goals::Goal &goal,std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error="Score storage is unavailable."; return false; }
  try {
    SqliteTransactionHandle tx(impl_->sessionDatabase,"BEGIN IMMEDIATE",error);
    if (!tx.active()) return false;
    (void)folder_goals::load(impl_->sessionDatabase); // Do not silently mutate corrupt/future data.
    folder_goals::remove(impl_->sessionDatabase,goal);
    if (!tx.commit(error)) return false;
    score_repository_detail::IncrementRevision(); return true;
  } catch (const std::exception &e) { error=e.what(); return false; }
}
std::vector<folder_goals::Goal> ScoreRepository::TakeFolderGoalAchievements(std::string_view attemptId) {
  std::lock_guard lock(impl_->sessionMutex);
  if (attemptId.empty() || impl_->folderGoalAchievementAttempt!=attemptId) return {};
  impl_->folderGoalAchievementAttempt.clear();
  return std::exchange(impl_->folderGoalAchievements,{});
}
