#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace assist_options {
inline constexpr const char *kOff = "OFF";
inline constexpr const char *kDrag = "DRAG";
inline constexpr const char *kBpmGuide = "BPM-GUIDE";

inline std::string normalize(std::string option) {
  option.erase(option.begin(), std::find_if_not(option.begin(), option.end(),
                                                [](unsigned char ch) {
                                                  return std::isspace(ch) != 0;
                                                }));
  option.erase(
      std::find_if_not(option.rbegin(), option.rend(),
                       [](unsigned char ch) { return std::isspace(ch) != 0; })
          .base(),
      option.end());
  std::transform(option.begin(), option.end(), option.begin(),
                 [](unsigned char ch) {
                   if (ch == '_' || ch == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(ch));
                 });
  if (option == "DRAG" || option == "DRAG-MODE") {
    return kDrag;
  }
  if (option == "BPM-GUIDE" || option == "BPMGUIDE") {
    return kBpmGuide;
  }
  return kOff;
}

inline bool isEnabled(const std::string &option) {
  return normalize(option) != kOff;
}

inline bool isDragMode(const std::string &option) {
  return normalize(option) == kDrag;
}

inline bool isBpmGuide(const std::string &option) {
  return normalize(option) == kBpmGuide;
}

// BMSPlayer only applies BPM Guide's light-assist result consequence when
// there is an authored BPM range.  The setting remains selected on a
// constant-tempo chart, but has no score/clear effect there.
inline bool bpmGuideAffectsClear(const std::string &option, double minimumBpm,
                                 double maximumBpm) noexcept {
  return isBpmGuide(option) && minimumBpm < maximumBpm;
}

// Score provenance records the effective attempt modifier. A BPM Guide
// setting on a constant-tempo chart is inert in BMSPlayer, so it must not
// turn that otherwise normal attempt into an assisted record.
inline std::string effectiveForChart(std::string option, double minimumBpm,
                                     double maximumBpm) {
  option = normalize(std::move(option));
  return bpmGuideAffectsClear(option, minimumBpm, maximumBpm) ? kBpmGuide
       : isBpmGuide(option)                                  ? kOff
                                                               : option;
}
} // namespace assist_options
