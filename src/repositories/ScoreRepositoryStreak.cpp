#include "ScoreRepositoryInternal.h"
#include "SqliteRAII.h"
#include "../streak/StreakStore.h"
#include <stdexcept>

std::optional<streak::State> ScoreRepository::LoadStreak(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return {};
  try {
    auto state = streak::load(impl_->sessionDatabase);
    const auto last = state.days.empty() ? 0 : state.days.rbegin()->first;
    streak::advance(state, streak::today());
    if (!state.days.empty() && last != state.days.rbegin()->first)
      streak::save(impl_->sessionDatabase, state);
    if (!transaction.commit(error)) return {};
    return state;
  } catch (const std::exception &failure) { error = failure.what(); return {}; }
}
std::optional<streak::PlayProgress> ScoreRepository::TakeStreakProgress(std::string_view attemptId) {
  std::lock_guard lock(impl_->sessionMutex);
  if (!impl_->streakProgress) return {};
  if (impl_->streakProgress->day != streak::today()) { impl_->streakProgress.reset(); return {}; }
  if (!attemptId.empty() && impl_->streakProgress->attemptId != attemptId) return {};
  auto result = std::move(impl_->streakProgress);
  impl_->streakProgress.reset();
  return result;
}
bool ScoreRepository::ConfigureStreak(const streak::Config &config,
                                     streak::DayId &effectiveDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = streak::load(impl_->sessionDatabase);
    effectiveDay = streak::configure(state, config, streak::today());
    streak::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::AdjustStreakAssignment(const streak::Assignment &assignment,
                                            streak::DayId expectedDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = streak::load(impl_->sessionDatabase);
    const auto currentDay = streak::today();
    if (currentDay != expectedDay)
      throw std::runtime_error("The day changed. Return to today's dashboard and try again.");
    streak::adjustAssignment(state, assignment, currentDay);
    streak::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::ResetStreakExtraChallenge(const std::string &assignmentId,
                                            streak::DayId expectedDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = streak::load(impl_->sessionDatabase);
    const auto currentDay = streak::today();
    if (currentDay != expectedDay)
      throw std::runtime_error("The day changed. Return to today's dashboard and try again.");
    streak::resetExtraChallenge(state, assignmentId, currentDay);
    streak::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::BuyStreakFreeze(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = streak::load(impl_->sessionDatabase);
    if (!streak::buyFreeze(state, streak::today())) {
      error = "Not enough bonus points, or two freezes are already stored.";
      return false;
    }
    streak::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
