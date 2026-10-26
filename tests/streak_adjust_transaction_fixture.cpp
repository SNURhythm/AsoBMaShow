#include <cassert>
#include <mutex>
#include <stdexcept>
#include <string>
static int civilDay=1, savedDay=0, commits=0;
namespace profile_database_activity { struct WriteGuard {}; }
namespace streak {
using DayId=int;
struct Assignment {};
struct State { int day=0; };
DayId today() { return civilDay; }
State load(void *) { return {}; }
void save(void *, const State &state) { savedDay=state.day; }
void adjustAssignment(State &state, const Assignment &, DayId day) { state.day=day; }
}
struct SqliteTransactionHandle {
  SqliteTransactionHandle(void *, const char *, std::string &) { civilDay=2; }
  bool active() const { return true; }
  bool commit(std::string &) { ++commits; return true; }
};
struct ScoreRepository {
  struct Impl { std::mutex sessionMutex; void *sessionDatabase=nullptr; } impl;
  Impl *impl_=&impl;
  bool EnsureSessionDatabaseLocked() { return true; }
  bool AdjustStreakAssignment(const streak::Assignment &, streak::DayId, std::string &);
};
PRODUCTION_METHOD
int main() {
  ScoreRepository repository;
  std::string error;
  assert(!repository.AdjustStreakAssignment({}, 1, error));
  assert(!error.empty() && savedDay==0 && commits==0);
  assert(repository.AdjustStreakAssignment({}, 2, error));
  assert(error.empty() && savedDay==2 && commits==1);
}
