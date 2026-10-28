#include "ScoreRepositoryInternal.h"
#include "SqliteRAII.h"
#include "../kombo/KomboStore.h"
#include <stdexcept>

std::optional<kombo::State> ScoreRepository::LoadKombo(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return {}; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return {};
  try {
    auto state = kombo::load(impl_->sessionDatabase);
    const auto last = state.days.empty() ? 0 : state.days.rbegin()->first;
    kombo::advance(state, kombo::today());
    if (!state.days.empty() && last != state.days.rbegin()->first)
      kombo::save(impl_->sessionDatabase, state);
    if (!transaction.commit(error)) return {};
    return state;
  } catch (const std::exception &failure) { error = failure.what(); return {}; }
}
std::optional<kombo::PlayProgress> ScoreRepository::TakeKomboProgress(std::string_view attemptId) {
  std::lock_guard lock(impl_->sessionMutex);
  if (!impl_->komboProgress) return {};
  if (impl_->komboProgress->day != kombo::today()) { impl_->komboProgress.reset(); return {}; }
  if (!attemptId.empty() && impl_->komboProgress->attemptId != attemptId) return {};
  auto result = std::move(impl_->komboProgress);
  impl_->komboProgress.reset();
  return result;
}
bool ScoreRepository::ConfigureKombo(const kombo::Config &config,
                                     kombo::DayId &effectiveDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = kombo::load(impl_->sessionDatabase);
    effectiveDay = kombo::configure(state, config, kombo::today());
    kombo::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::AdjustKomboAssignment(const kombo::Assignment &assignment,
                                            kombo::DayId expectedDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = kombo::load(impl_->sessionDatabase);
    const auto currentDay = kombo::today();
    if (currentDay != expectedDay)
      throw std::runtime_error("The day changed. Return to today's dashboard and try again.");
    kombo::adjustAssignment(state, assignment, currentDay);
    kombo::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::ResetKomboExtraChallenge(const std::string &assignmentId,
                                            kombo::DayId expectedDay, std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = kombo::load(impl_->sessionDatabase);
    const auto currentDay = kombo::today();
    if (currentDay != expectedDay)
      throw std::runtime_error("The day changed. Return to today's dashboard and try again.");
    kombo::resetExtraChallenge(state, assignmentId, currentDay);
    kombo::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
bool ScoreRepository::BuyKomboFreeze(std::string &error) {
  profile_database_activity::WriteGuard guard;
  std::lock_guard lock(impl_->sessionMutex);
  error.clear();
  if (!EnsureSessionDatabaseLocked()) { error = "Score storage is unavailable."; return false; }
  SqliteTransactionHandle transaction(impl_->sessionDatabase, "BEGIN IMMEDIATE", error);
  if (!transaction.active()) return false;
  try {
    auto state = kombo::load(impl_->sessionDatabase);
    if (!kombo::buyFreeze(state, kombo::today())) {
      error = "Not enough bonus points, or two freezes are already stored.";
      return false;
    }
    kombo::save(impl_->sessionDatabase, state);
    return transaction.commit(error);
  } catch (const std::exception &failure) { error = failure.what(); return false; }
}
