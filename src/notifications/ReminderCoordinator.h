#pragma once
#include "LocalNotifications.h"
#include "ReminderPlan.h"
#include "../context.h"
#include <chrono>
#include <ctime>
#include <nlohmann/json.hpp>

namespace reminders {
// Owned by the application thread. Polling is cheap; repository reads happen
// only when a profile, score revision or civil date changes, or on a lifecycle
// transition (which also picks up routine edits and purchased freezes).
class Coordinator {
public:
  void update(ApplicationContext &context, bool force = false) {
    if constexpr (!supported) return;
    const auto ticks = SDL_GetTicks();
    if (!force && ticks < nextPoll_) return;
    nextPoll_ = ticks + 1000;
    refreshPermission();
    const auto tap = takeTapProfile();
    if (!tap.empty()) context.pendingKomboNotificationProfile = tap;
    if (!context.profileReady()) {
      if (!key_.empty() || force) replacePlan("[]");
      key_.clear(); cachedState_.reset();
      return;
    }
    const auto profile = context.profileManager.activeProfile().id;
    if (!context.pendingKomboNotificationProfile.empty() &&
        context.pendingKomboNotificationProfile != profile)
      context.pendingKomboNotificationProfile.clear();
    Preferences prefs;
    std::string language;
    {
      const std::lock_guard lock(context.applicationUiStateMutex);
      prefs = context.applicationUiState.reminders;
      language = context.applicationUiState.language;
    }
    const auto now = std::time(nullptr);
    const auto today = kombo::localDay(now);
    auto revision = context.scoreRepository.GetRevision();
    if (force || profile != cachedProfile_ || today != cachedDay_ || revision != cachedRevision_ || !cachedState_) {
      std::string error;
      cachedState_ = context.scoreRepository.LoadKombo(error);
      cachedProfile_ = profile; cachedDay_ = today;
      cachedRevision_ = context.scoreRepository.GetRevision();
      revision = cachedRevision_;
      if (!cachedState_) { replacePlan("[]"); key_.clear(); return; }
    }
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char zone[80]{};
    std::strftime(zone, sizeof(zone), "%Z%z", &local);
    const auto status = permission();
    const auto key = nlohmann::json::array({profile, revision, today, prefs.daily, prefs.comeback,
        prefs.hour, prefs.minute, language, zone, static_cast<int>(status)}).dump();
    if (!force && key == key_) return;
    key_ = key;
    auto requests = nlohmann::json::array();
    if (status == Permission::Granted) {
      for (const auto &entry : makePlan(*cachedState_, prefs, today, local.tm_hour * 60 + local.tm_min)) {
        const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{entry.day}}};
        const bool comeback = entry.kind == Kind::Comeback;
        requests.push_back({{"year", int(date.year())}, {"month", unsigned(date.month())},
            {"day", unsigned(date.day())}, {"hour", prefs.hour}, {"minute", prefs.minute},
            {"profile", profile}, {"title", i18n::tr("kombo.reminders.title")},
            {"body", i18n::tr(comeback ? "kombo.reminders.comeback_body" : "kombo.reminders.daily_body")}});
      }
    }
    // JNI NewStringUTF accepts modified UTF-8; ASCII JSON safely carries even
    // supplementary Unicode characters through escapes.
    replacePlan(requests.dump(-1, ' ', true));
  }
private:
  std::uint64_t nextPoll_ = 0, cachedRevision_ = 0;
  kombo::DayId cachedDay_ = 0;
  std::string key_, cachedProfile_;
  std::optional<kombo::State> cachedState_;
};
}
