#pragma once

namespace reminders {
struct Preferences {
  bool daily = false;
  bool comeback = false;
  int hour = 20;
  int minute = 0;
  [[nodiscard]] bool valid() const { return hour >= 0 && hour < 24 && minute >= 0 && minute < 60; }
  bool operator==(const Preferences &) const = default;
};
} // namespace reminders
