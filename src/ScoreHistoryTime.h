#pragma once

#include <cstdint>
#include <ctime>
#include <string>

// Legacy score timestamps are UTC with second precision. A known attempt ID
// supplies the ordering within that second; a time-only fallback is exclusive.
inline std::string scoreHistoryTime(std::int64_t unixMillis) {
  if (unixMillis <= 0) return {};
  const auto seconds = static_cast<std::time_t>(unixMillis / 1000);
  std::tm utc{};
#ifdef _WIN32
  if (gmtime_s(&utc, &seconds) != 0) return {};
#else
  if (gmtime_r(&seconds, &utc) == nullptr) return {};
#endif
  char text[32]{};
  if (std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &utc) == 0) return {};
  return text;
}
