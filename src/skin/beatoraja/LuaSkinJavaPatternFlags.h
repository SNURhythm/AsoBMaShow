#pragma once

#include <string>
#include <string_view>

namespace skin::lua_skin_java_pattern_detail {

struct AdaptedPattern {
  std::string expression;
  bool unicodeCharacterClasses = false;
  bool unixLines = false;
};

// PCRE2 and Foundation already operate on Unicode strings, so Java's `u`
// flag needs no syntax.  `U` additionally selects Unicode predefined classes
// in PCRE2.  Java's `d` is PCRE2's default LF-only newline convention.
// Preserve the shared i/m/s/x flag grammar, including scoped and mid-pattern
// forms, without otherwise interpreting regex syntax.
inline AdaptedPattern adaptJavaEmbeddedFlags(std::string_view input) {
  AdaptedPattern adapted;
  adapted.expression.reserve(input.size());
  bool escaped = false;
  bool characterClass = false;
  bool quoted = false;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const char value = input[index];
    if (quoted) {
      adapted.expression.push_back(value);
      if (value == '\\' && index + 1 < input.size() &&
          input[index + 1] == 'E') {
        adapted.expression.push_back(input[++index]);
        quoted = false;
      }
      continue;
    }
    if (escaped) {
      adapted.expression.push_back(value);
      if (value == 'Q') {
        quoted = true;
      }
      escaped = false;
      continue;
    }
    if (value == '\\') {
      adapted.expression.push_back(value);
      escaped = true;
      continue;
    }
    if (value == '[') {
      characterClass = true;
      adapted.expression.push_back(value);
      continue;
    }
    if (value == ']' && characterClass) {
      characterClass = false;
      adapted.expression.push_back(value);
      continue;
    }
    if (characterClass || value != '(' || index + 2 >= input.size() ||
        input[index + 1] != '?') {
      adapted.expression.push_back(value);
      continue;
    }

    std::size_t cursor = index + 2;
    bool sawFlag = false;
    while (cursor < input.size() &&
           std::string_view("idmsuxU-").find(input[cursor]) !=
               std::string_view::npos) {
      sawFlag = true;
      ++cursor;
    }
    if (!sawFlag || cursor >= input.size() ||
        (input[cursor] != ')' && input[cursor] != ':')) {
      adapted.expression.push_back(value);
      continue;
    }

    std::string retained;
    bool disabling = false;
    for (std::size_t flag = index + 2; flag < cursor; ++flag) {
      const char setting = input[flag];
      if (setting == '-') {
        disabling = true;
        continue;
      }
      if (setting == 'U') {
        if (!disabling) {
          adapted.unicodeCharacterClasses = true;
        }
        continue;
      }
      if (setting == 'd') {
        adapted.unixLines = !disabling;
        continue;
      }
      if (setting == 'u') {
        continue;
      }
      if (disabling && retained.find('-') == std::string::npos) {
        retained.push_back('-');
      }
      retained.push_back(setting);
    }
    if (!retained.empty() && retained.back() == '-') {
      retained.pop_back();
    }
    if (!retained.empty()) {
      adapted.expression += "(?";
      adapted.expression += retained;
      adapted.expression.push_back(input[cursor]);
    } else if (input[cursor] == ':') {
      adapted.expression += "(?:";
    }
    index = cursor;
  }
  return adapted;
}

} // namespace skin::lua_skin_java_pattern_detail
