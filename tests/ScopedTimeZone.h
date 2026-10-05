#pragma once

#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

// Process-local only: each test executable owns its timezone changes.
class ScopedTimeZone {
public:
  explicit ScopedTimeZone(const char *zone) {
    if (const auto *previous = std::getenv("TZ")) previous_ = previous;
    set(zone);
  }
  ~ScopedTimeZone() { set(previous_ ? previous_->c_str() : nullptr); }
private:
  static void set(const char *zone) {
#ifdef _WIN32
    _putenv_s("TZ", zone ? zone : "");
    _tzset();
#else
    if (zone) setenv("TZ", zone, 1);
    else unsetenv("TZ");
    tzset();
#endif
  }
  std::optional<std::string> previous_;
};
