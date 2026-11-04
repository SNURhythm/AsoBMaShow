#pragma once
#include <array>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace recap {
struct Range { std::int64_t start = 0, end = 0; };
enum class Kind { Lamp, Score, BadPoints };
struct Upgrade {
  std::string hash, title;
  int lnMode = 0;
  std::optional<int> before;
  int after = 0;
};
struct Snapshot {
  Range range;
  std::array<std::vector<Upgrade>, 3> sections;
  std::string error;
};
inline std::tm localTime(std::int64_t seconds) {
  const auto time = static_cast<std::time_t>(seconds);
  std::tm result{};
#ifdef _WIN32
  localtime_s(&result, &time);
#else
  localtime_r(&time, &result);
#endif
  return result;
}
inline std::string localDate(std::int64_t seconds) {
  auto time = localTime(seconds);
  char buffer[32]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &time);
  return buffer;
}
inline std::optional<std::int64_t> parseLocalDate(const std::string &value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') return {};
  for (std::size_t i=0; i<value.size(); ++i)
    if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return {};
  int year, month, day;
  if (std::sscanf(value.c_str(), "%d-%d-%d", &year, &month, &day) != 3 ||
      year < 1970 || year > 9998 || month < 1 || month > 12 || day < 1 || day > 31) return {};
  std::tm time{};
  time.tm_year = year - 1900; time.tm_mon = month - 1; time.tm_mday = day; time.tm_isdst = -1;
  const auto seconds = std::mktime(&time);
  if (seconds == -1 || localDate(seconds) != value) return {};
  return seconds;
}
inline std::int64_t shiftDays(std::int64_t seconds, int days) {
  auto time = localTime(seconds);
  time.tm_mday += days; time.tm_isdst = -1;
  return std::mktime(&time);
}
inline Range calendarRange(std::int64_t now, bool weekly, int offset = 0) {
  auto time = localTime(now);
  time.tm_hour = time.tm_min = time.tm_sec = 0;
  time.tm_mday += weekly ? -(time.tm_wday + 6) % 7 + offset * 7 : offset;
  time.tm_isdst = -1;
  const auto start = std::mktime(&time);
  return {start, shiftDays(start, weekly ? 7 : 1)};
}
} // namespace recap
