#pragma once

#include <string>
#include <string_view>

inline std::string makeResultShareText(std::string_view title,
                                       std::string_view subtitle,
                                       std::string_view lamp,
                                       std::string_view rank) {
  std::string text;
  for (const auto part : {title, subtitle, lamp, rank}) {
    bool space = !text.empty();
    for (const char c : part) {
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        space = !text.empty();
      } else {
        if (space) text += ' ';
        text += c;
        space = false;
      }
    }
  }
  return text + "\n#AsoBMaShow";
}
