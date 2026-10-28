#pragma once
#include "../kombo/KomboModel.h"
#include "ReminderPreferences.h"
#include <algorithm>
#include <vector>

namespace reminders {
enum class Kind { Daily, Comeback };
struct Entry {
  kombo::DayId day;
  Kind kind;
  bool operator==(const Entry &) const = default;
};
// Input state has been advanced to today by the score repository. Dates are
// civil days, not elapsed 24-hour intervals, so DST cannot move a reminder day.
inline std::vector<Entry> makePlan(const kombo::State &state, const Preferences &prefs,
                                   kombo::DayId today, int minuteOfDay) {
  std::vector<Entry> result;
  if (!prefs.valid() || state.configurations.empty() || state.days.empty() ||
      state.configurations.begin()->first > today) return result;
  const auto current = state.days.find(today);
  if (current == state.days.end()) return result;
  const auto &day = current->second;
  const bool completed = day.points >= day.config.dailyGoal;
  const bool forecastLoss = prefs.comeback && state.currentKombo > 0;
  // A clock/zone rollback does not undo recorded days or refund consumed
  // freezes. Forecast from the last retained day, just as advance() does,
  // rather than spending the remaining inventory again from today's date.
  const auto &[frontier, last] = *state.days.rbegin();
  // Late results can refund freezes beyond the two-freeze purchase limit.
  const auto lossDay = frontier + ((last.played || last.frozen) ? 1 : 0) +
      std::max(state.freezes, 0) + 1;
  if (prefs.daily) {
    const auto end = forecastLoss ? std::min(lossDay, today + 53) : today + 56;
    for (auto date = today; date < end; ++date) {
      if (date == today && (completed || minuteOfDay >= prefs.hour * 60 + prefs.minute)) continue;
      const auto retained = state.days.find(date);
      if (retained != state.days.end() && retained->second.points >= retained->second.config.dailyGoal) continue;
      result.push_back({date, Kind::Daily});
    }
  }
  if (forecastLoss)
    for (int offset : {2, 7, 14}) result.push_back({lossDay + offset, Kind::Comeback});
  return result;
}
} // namespace reminders
