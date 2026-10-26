#include "streak/StreakForm.h"
#include "notifications/ReminderPreferences.h"
#include <cassert>
#include <functional>
#include <mutex>
#include <vector>
namespace reminders {
constexpr bool supported = true;
enum class Permission { Unknown, NotDetermined, Denied, Granted };
Permission authorization = Permission::NotDetermined;
int requests = 0, settings = 0;
Permission permission() { return authorization; }
void requestPermission() { ++requests; }
void openSettings() { ++settings; }
}
namespace i18n { const char *tr(const char *key) { return key; } }
namespace app_theme { enum class ActionCategory { Primary, Neutral }; }
void SDL_Log(const char *, const char *) {}
std::string comparison(const char *, const std::string &) { return "saved"; }
struct View { void setName(const char *) {} };
View *card(View *view) { return view; }
void label(View *, const char *, int) {}
std::vector<std::function<void()>> actions;
View *action(View *view, const char *, std::function<void()> callback, app_theme::ActionCategory) {
  actions.push_back(std::move(callback)); return view;
}
struct Repository {
  bool succeeds = true;
  bool ConfigureStreak(const streak::Config &, streak::DayId &effective, std::string &error) {
    effective = 1; error = "save failed"; return succeeds;
  }
};
struct Context {
  Repository scoreRepository;
  std::mutex applicationUiStateMutex;
  struct { reminders::Preferences reminders; } applicationUiState;
  reminders::Preferences persisted;
  bool saves = true;
  bool saveApplicationUiState(std::string *) {
    if (saves) persisted = applicationUiState.reminders;
    return saves;
  }
};
struct StreakScene {
  enum class Page { Dashboard, Routine, ReminderPrompt } page_ = Page::Routine;
  Context context;
  streak::State state_;
  streak::Config draft_;
  streak::FieldErrors fieldErrors_;
  std::string message_, daily_ = "1", weekly_ = "1", monthly_ = "1";
  StreakScene() { draft_.assignments.push_back({}); }
  void reload() { state_.configurations[1] = draft_; }
  void later(std::function<void()> callback) { callback(); }
  void saveRoutine();
  bool saveReminders(reminders::Preferences);
  void reminderPrompt(View *);
};
PRODUCTION_METHODS
int main() {
  StreakScene first;
  first.saveRoutine();
  assert(first.page_ == StreakScene::Page::ReminderPrompt);
  assert(!first.context.persisted.daily && !first.context.persisted.comeback);
  assert(reminders::requests == 0);
  first.page_ = StreakScene::Page::Routine;
  first.saveRoutine();
  assert(first.page_ == StreakScene::Page::Dashboard); // Later edits never ask again.
  StreakScene failed;
  failed.context.scoreRepository.succeeds = false;
  failed.saveRoutine();
  assert(failed.page_ == StreakScene::Page::Routine);
  StreakScene enabled;
  enabled.context.applicationUiState.reminders.daily = true;
  enabled.context.applicationUiState.reminders.comeback = true;
  enabled.saveRoutine();
  assert(enabled.page_ == StreakScene::Page::Dashboard);
  View body;
  StreakScene skipped;
  skipped.saveRoutine();
  skipped.reminderPrompt(&body);
  assert(actions.size() == 2);
  actions[1]();
  assert(skipped.page_ == StreakScene::Page::Dashboard);
  assert(!skipped.context.persisted.daily && !skipped.context.persisted.comeback);
  assert(reminders::requests == 0);

  actions.clear();
  StreakScene confirmed;
  confirmed.context.applicationUiState.reminders.hour = 18;
  confirmed.context.applicationUiState.reminders.minute = 35;
  confirmed.saveRoutine();
  confirmed.reminderPrompt(&body);
  confirmed.context.saves = false;
  actions[0]();
  assert(confirmed.page_ == StreakScene::Page::ReminderPrompt);
  assert(!confirmed.message_.empty());
  assert(!confirmed.context.applicationUiState.reminders.daily);
  assert(!confirmed.context.applicationUiState.reminders.comeback);
  assert(reminders::requests == 0);
  confirmed.context.saves = true;
  actions[0]();
  assert(confirmed.page_ == StreakScene::Page::Dashboard);
  assert(confirmed.context.persisted.daily && confirmed.context.persisted.comeback);
  assert(confirmed.context.persisted.hour == 18 && confirmed.context.persisted.minute == 35);
  assert(reminders::requests == 1);

  actions.clear();
  reminders::authorization = reminders::Permission::Denied;
  StreakScene denied;
  denied.saveRoutine();
  denied.reminderPrompt(&body);
  actions[0]();
  assert(denied.context.persisted.daily && denied.context.persisted.comeback);
  assert(reminders::settings == 1 && reminders::requests == 1);
}
