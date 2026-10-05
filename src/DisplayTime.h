#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace display_time {

enum class Precision { Minutes, Seconds, Milliseconds };

// Stored SQL timestamps are UTC even without a suffix. Profile metadata uses
// ISO 8601 UTC. Parse independently of the device timezone and DST rules.
inline std::optional<std::int64_t> parseUtcTimestamp(std::string_view value) {
  if (!value.empty() && value.back() == 'Z') value.remove_suffix(1);
  if ((value.size() != 19 && value.size() != 23) || value[4] != '-' ||
      value[7] != '-' || (value[10] != ' ' && value[10] != 'T') ||
      value[13] != ':' || value[16] != ':' ||
      (value.size() == 23 && value[19] != '.')) return std::nullopt;
  const auto digits = [&](std::size_t offset, std::size_t count) -> int {
    const auto part = value.substr(offset, count);
    for (const char digit : part) {
      if (digit < '0' || digit > '9') return -1;
    }
    int number = 0;
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), number);
    return parsed.ec == std::errc{} ? number : -1;
  };
  const int year = digits(0, 4);
  const int month = digits(5, 2);
  const int day = digits(8, 2);
  const int hour = digits(11, 2);
  const int minute = digits(14, 2);
  const int second = digits(17, 2);
  const int millis = value.size() == 23 ? digits(20, 3) : 0;
  const auto date = std::chrono::year{year} /
                    std::chrono::month{static_cast<unsigned>(month)} /
                    std::chrono::day{static_cast<unsigned>(day)};
  if (year < 1 || month < 1 || day < 1 || !date.ok() || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
      millis < 0) return std::nullopt;
  const auto instant = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                       std::chrono::minutes{minute} + std::chrono::seconds{second} +
                       std::chrono::milliseconds{millis};
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             instant.time_since_epoch()).count();
}

inline std::string formatUnixMillis(
    std::int64_t unixMillis, Precision precision = Precision::Seconds) {
  const auto instant = std::chrono::milliseconds{unixMillis};
  const auto wholeSeconds = std::chrono::floor<std::chrono::seconds>(instant);
  const auto seconds = static_cast<std::time_t>(wholeSeconds.count());
  if (static_cast<std::int64_t>(seconds) != wholeSeconds.count()) return {};
  std::tm local{};
#ifdef _WIN32
  if (localtime_s(&local, &seconds) != 0) return {};
#else
  if (localtime_r(&seconds, &local) == nullptr) return {};
#endif
  char text[32]{};
  const auto *format = precision == Precision::Minutes ? "%Y-%m-%d %H:%M"
                                                     : "%Y-%m-%d %H:%M:%S";
  if (std::strftime(text, sizeof(text), format, &local) == 0) return {};
  std::string result(text);
  if (precision == Precision::Milliseconds) {
    const auto fraction = std::chrono::duration_cast<std::chrono::milliseconds>(
                              instant - wholeSeconds).count();
    const auto digits = std::to_string(fraction);
    result += '.';
    result.append(3 - digits.size(), '0');
    result += digits;
  }
  return result;
}

// Labels such as AUTO PLAY and unknown legacy values are not timestamps.
inline std::string formatStoredTimestamp(std::string_view value) {
  const auto instant = parseUtcTimestamp(value);
  if (!instant) return std::string(value);
  const auto formatted = formatUnixMillis(
      *instant, value.size() > 19 && value[19] == '.'
                    ? Precision::Milliseconds : Precision::Seconds);
  return formatted.empty() ? std::string(value) : formatted;
}

} // namespace display_time
