#pragma once

#include <string>

namespace gameplay_timing {

inline std::string formatJudgementTimingMilliseconds(long long diffMicros) {
  const auto unsignedDiff = static_cast<unsigned long long>(diffMicros);
  const unsigned long long magnitudeMicros =
      diffMicros < 0 ? 0ULL - unsignedDiff : unsignedDiff;
  const unsigned long long hundredths =
      magnitudeMicros / 10ULL + (magnitudeMicros % 10ULL != 0ULL ? 1ULL : 0ULL);
  const unsigned long long wholeMilliseconds = hundredths / 100ULL;
  const unsigned long long fractionalHundredths = hundredths % 100ULL;

  return std::to_string(wholeMilliseconds) + "." +
         (fractionalHundredths < 10ULL ? "0" : "") +
         std::to_string(fractionalHundredths) + "ms";
}

} // namespace gameplay_timing
