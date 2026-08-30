#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace skin::lua_skin_java_pattern_detail {

struct AdaptedPattern {
  std::string expression;
};

namespace detail {

struct JavaFlags {
  bool unixLines = false;
  bool caseInsensitive = false;
  bool comments = false;
  bool multiline = false;
  bool dotAll = false;
  bool unicodeCase = false;
  bool unicodeClasses = false;
};

inline bool engineCaseInsensitive(const JavaFlags &flags) noexcept {
  return flags.caseInsensitive &&
         (flags.unicodeCase || flags.unicodeClasses);
}

inline bool asciiCaseInsensitive(const JavaFlags &flags) noexcept {
  return flags.caseInsensitive && !engineCaseInsensitive(flags);
}

inline bool isAsciiLetter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

inline char oppositeAsciiCase(char value) noexcept {
  if (value >= 'a' && value <= 'z') {
    return static_cast<char>(value - 'a' + 'A');
  }
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

inline void appendEngineFlagDelta(std::string &output,
                                  const JavaFlags &before,
                                  const JavaFlags &after) {
  std::string enabled;
  std::string disabled;
  if (engineCaseInsensitive(before) != engineCaseInsensitive(after)) {
    (engineCaseInsensitive(after) ? enabled : disabled).push_back('i');
  }
  if (before.comments != after.comments) {
    (after.comments ? enabled : disabled).push_back('x');
  }
  if (enabled.empty() && disabled.empty()) {
    return;
  }
  output += "(?";
  output += enabled;
  if (!disabled.empty()) {
    output.push_back('-');
    output += disabled;
  }
  output.push_back(')');
}

inline void applyFlag(char flag, bool enabled, JavaFlags &flags) {
  switch (flag) {
  case 'd': flags.unixLines = enabled; break;
  case 'i': flags.caseInsensitive = enabled; break;
  case 'm': flags.multiline = enabled; break;
  case 's': flags.dotAll = enabled; break;
  case 'u': flags.unicodeCase = enabled; break;
  case 'x': flags.comments = enabled; break;
  case 'U':
    flags.unicodeClasses = enabled;
    if (enabled) flags.unicodeCase = true;
    break;
  default: throw std::invalid_argument("unsupported Java Pattern flag");
  }
}

inline JavaFlags withFlags(JavaFlags flags, std::string_view specification) {
  bool disabling = false;
  bool sawFlag = false;
  bool sawHyphen = false;
  for (const char value : specification) {
    if (value == '-') {
      if (sawHyphen) {
        throw std::invalid_argument("duplicate Java Pattern flag hyphen");
      }
      sawHyphen = true;
      disabling = true;
      continue;
    }
    sawFlag = true;
    applyFlag(value, !disabling, flags);
  }
  if (!sawFlag) throw std::invalid_argument("empty Java Pattern flag group");
  return flags;
}

inline std::string asciiWordClass(bool negated) {
  return negated ? "[^A-Za-z_0-9]" : "[A-Za-z_0-9]";
}

inline std::string unicodeWordClass(bool negated) {
  return negated
             ? "[^\\p{L}\\p{M}\\p{N}\\p{Pc}\\x{200C}\\x{200D}]"
             : "[\\p{L}\\p{M}\\p{N}\\p{Pc}\\x{200C}\\x{200D}]";
}

inline std::string asciiDigitClass(bool negated) {
  return negated ? "[^0-9]" : "[0-9]";
}

inline std::string unicodeDigitClass(bool negated) {
  return negated ? "[^\\p{Nd}]" : "[\\p{Nd}]";
}

inline std::string asciiSpaceClass(bool negated) {
  return negated ? "[^\\x{20}\\x{09}\\x{0A}\\x{0B}\\x{0C}\\x{0D}]"
                 : "[\\x{20}\\x{09}\\x{0A}\\x{0B}\\x{0C}\\x{0D}]";
}

inline std::string unicodeSpaceClass(bool negated) {
  return negated ? "[^\\p{Z}\\x{09}-\\x{0D}\\x{85}]"
                 : "[\\p{Z}\\x{09}-\\x{0D}\\x{85}]";
}

inline std::string predefinedClass(char value, const JavaFlags &flags) {
  const bool negated = value == 'D' || value == 'S' || value == 'W';
  switch (static_cast<char>(std::tolower(static_cast<unsigned char>(value)))) {
  case 'd': return flags.unicodeClasses ? unicodeDigitClass(negated)
                                        : asciiDigitClass(negated);
  case 's': return flags.unicodeClasses ? unicodeSpaceClass(negated)
                                        : asciiSpaceClass(negated);
  case 'w': return flags.unicodeClasses ? unicodeWordClass(negated)
                                        : asciiWordClass(negated);
  default: return {};
  }
}

struct PosixPropertyClasses {
  std::string positive;
  std::string negative;
};

inline PosixPropertyClasses makePropertyClasses(std::string fragment) {
  return {.positive = "[" + fragment + "]",
          .negative = "[^" + fragment + "]"};
}

inline std::optional<PosixPropertyClasses>
javaPosixProperty(std::string_view name, const JavaFlags &flags) {
  if (!flags.unicodeClasses) {
    if (name == "Lower") {
      return makePropertyClasses(asciiCaseInsensitive(flags) ? "A-Za-z"
                                                             : "a-z");
    }
    if (name == "Upper") {
      return makePropertyClasses(asciiCaseInsensitive(flags) ? "A-Za-z"
                                                             : "A-Z");
    }
    if (name == "ASCII") return makePropertyClasses("\\x00-\\x7F");
    if (name == "Alpha") return makePropertyClasses("A-Za-z");
    if (name == "Digit") return makePropertyClasses("0-9");
    if (name == "Alnum") return makePropertyClasses("A-Za-z0-9");
    if (name == "Punct") {
      return makePropertyClasses(
          "\\x21-\\x2F\\x3A-\\x40\\x5B-\\x60\\x7B-\\x7E");
    }
    if (name == "Graph") return makePropertyClasses("\\x21-\\x7E");
    if (name == "Print") return makePropertyClasses("\\x20-\\x7E");
    if (name == "Blank") return makePropertyClasses("\\x09\\x20");
    if (name == "Cntrl") {
      return makePropertyClasses("\\x00-\\x1F\\x7F");
    }
    if (name == "XDigit") return makePropertyClasses("0-9A-Fa-f");
    if (name == "Space") {
      return makePropertyClasses("\\x09-\\x0D\\x20");
    }
    return std::nullopt;
  }

  if (name == "Lower") return makePropertyClasses("\\p{Lower}");
  if (name == "Upper") return makePropertyClasses("\\p{Upper}");
  if (name == "ASCII") return makePropertyClasses("\\x00-\\x7F");
  if (name == "Alpha") return makePropertyClasses("\\p{Alphabetic}");
  if (name == "Digit") return makePropertyClasses("\\p{Nd}");
  if (name == "Alnum") {
    return makePropertyClasses("\\p{Alphabetic}\\p{Nd}");
  }
  if (name == "Punct") return makePropertyClasses("\\p{P}");
  if (name == "Graph") {
    return PosixPropertyClasses{
        .positive = "[^\\p{White_Space}\\p{Cc}\\p{Cs}\\p{Cn}]",
        .negative = "[\\p{White_Space}\\p{Cc}\\p{Cs}\\p{Cn}]"};
  }
  if (name == "Print") {
    constexpr std::string_view excluded =
        "\\p{Cc}\\p{Cs}\\p{Cn}\\p{Zl}\\p{Zp}"
        "\\x09-\\x0D\\x85";
    return PosixPropertyClasses{.positive = "[^" + std::string(excluded) + "]",
                                .negative = "[" + std::string(excluded) + "]"};
  }
  if (name == "Blank") return makePropertyClasses("\\p{Zs}\\x09");
  if (name == "Cntrl") return makePropertyClasses("\\p{Cc}");
  if (name == "XDigit") {
    return makePropertyClasses("\\p{Nd}\\p{Hex_Digit}");
  }
  if (name == "Space") return makePropertyClasses("\\p{White_Space}");
  return std::nullopt;
}

inline std::string wordBoundary(bool boundary) {
  // Beatoraja targets Java 17, where word boundaries use Unicode word
  // characters even when UNICODE_CHARACTER_CLASS is disabled. This differs
  // from the default ASCII-only Java \w class and from Java 19+ boundary
  // behavior, so spell the boundary out instead of delegating to PCRE2.
  const std::string word = unicodeWordClass(false);
  const std::string nonWord = unicodeWordClass(true);
  if (boundary) {
    return "(?:(?:(?<=\\A)|(?<=" + nonWord + "))(?=" + word +
           ")|(?<=" + word + ")(?:(?=\\z)|(?=" + nonWord + ")))";
  }
  return "(?:(?:(?<=\\A)|(?<=" + nonWord + "))(?:(?=\\z)|(?=" +
         nonWord + "))|(?<=" + word + ")(?=" + word + "))";
}

class JavaPatternAdapter {
public:
  explicit JavaPatternAdapter(std::string_view input) : input_(input) {}

  AdaptedPattern adapt() {
    requireBoundedNesting();
    std::size_t cursor = 0;
    JavaFlags flags;
    std::string expression = transformSequence(cursor, flags, false);
    if (cursor != input_.size()) {
      throw std::invalid_argument("unconsumed Java Pattern input");
    }
    if (expression.empty()) {
      expression = "(?:)";
    }
    return {.expression = std::move(expression)};
  }

private:
  static constexpr std::size_t maximumNestingDepth = 96;

  void requireBoundedNesting() const {
    std::size_t groupDepth = 0;
    std::size_t classDepth = 0;
    std::array<unsigned char, maximumNestingDepth + 1> classPrefix{};
    bool quoted = false;
    for (std::size_t cursor = 0; cursor < input_.size(); ++cursor) {
      const char value = input_[cursor];
      if (value == '\\' && cursor + 1 < input_.size()) {
        const char escaped = input_[cursor + 1];
        if (quoted) {
          if (escaped == 'E') quoted = false;
        } else if (classDepth == 0 && escaped == 'Q') {
          quoted = true;
        }
        ++cursor;
        continue;
      }
      if (quoted) continue;
      if (classDepth != 0) {
        unsigned char &prefix = classPrefix[classDepth];
        if (prefix == 0 && value == '^') {
          prefix = 1;
          continue;
        }
        if (prefix < 2 && value == ']') {
          prefix = 2;
          continue;
        }
        if (value == '[') {
          if (++classDepth > maximumNestingDepth) {
            throw std::invalid_argument("Java Pattern nesting is too deep");
          }
          classPrefix[classDepth] = 0;
          continue;
        }
        if (value == ']') {
          --classDepth;
          continue;
        }
        prefix = 2;
        continue;
      }
      if (value == '[') {
        classDepth = 1;
        classPrefix[classDepth] = 0;
        continue;
      }
      if (value == '(') {
        if (++groupDepth > maximumNestingDepth) {
          throw std::invalid_argument("Java Pattern nesting is too deep");
        }
      } else if (value == ')' && groupDepth != 0) {
        --groupDepth;
      }
    }
  }

  struct FlagGroup {
    std::string_view specification;
    char terminator = 0;
    std::size_t end = 0;
  };

  static bool isFlagLetter(char value) noexcept {
    return std::string_view("idmsuxU-").find(value) !=
           std::string_view::npos;
  }

  bool parseFlagGroup(std::size_t start, FlagGroup &group) const noexcept {
    if (start + 3 > input_.size() || input_[start] != '(' ||
        input_[start + 1] != '?') return false;
    std::size_t cursor = start + 2;
    while (cursor < input_.size() && isFlagLetter(input_[cursor])) ++cursor;
    if (cursor == start + 2 || cursor >= input_.size() ||
        (input_[cursor] != ')' && input_[cursor] != ':')) return false;
    group = {.specification = input_.substr(start + 2, cursor - start - 2),
             .terminator = input_[cursor], .end = cursor};
    return true;
  }

  static void appendLiteral(std::string &output, char value,
                            const JavaFlags &flags) {
    if (asciiCaseInsensitive(flags) && isAsciiLetter(value)) {
      output.push_back('[');
      output.push_back(value);
      output.push_back(oppositeAsciiCase(value));
      output.push_back(']');
      return;
    }
    if (flags.comments &&
        (value == '#' || std::isspace(static_cast<unsigned char>(value)))) {
      constexpr char hexadecimal[] = "0123456789ABCDEF";
      const auto byte = static_cast<unsigned char>(value);
      output += "\\x{";
      output.push_back(hexadecimal[byte >> 4]);
      output.push_back(hexadecimal[byte & 0x0f]);
      output.push_back('}');
      return;
    }
    if (std::string_view(R"(\.^$|?*+()[]{}-)").find(value) !=
        std::string_view::npos) output.push_back('\\');
    output.push_back(value);
  }

  std::size_t appendQuoted(std::size_t cursor, const JavaFlags &flags,
                           std::string &output) const {
    while (cursor < input_.size()) {
      if (input_[cursor] == '\\' && cursor + 1 < input_.size() &&
          input_[cursor + 1] == 'E') return cursor + 2;
      appendLiteral(output, input_[cursor], flags);
      ++cursor;
    }
    return cursor;
  }

  static std::string classFragment(char value, const JavaFlags &flags) {
    const auto complete = predefinedClass(value, flags);
    if (complete.empty()) return {};
    return complete.substr(1, complete.size() - 2);
  }

  static std::size_t findNestedClassEnd(std::string_view value,
                                        std::size_t start) {
    std::size_t cursor = start + 1;
    if (cursor < value.size() && value[cursor] == '^') ++cursor;
    if (cursor < value.size() && value[cursor] == ']') ++cursor;
    while (cursor < value.size()) {
      if (value[cursor] == '\\') {
        cursor += cursor + 1 < value.size() ? 2 : 1;
        continue;
      }
      if (value[cursor] == '[') {
        cursor = findNestedClassEnd(value, cursor) + 1;
        continue;
      }
      if (value[cursor] == ']') return cursor;
      ++cursor;
    }
    throw std::invalid_argument("unterminated Java character class");
  }

  static std::vector<std::string_view>
  splitIntersections(std::string_view content) {
    std::vector<std::string_view> terms;
    std::size_t start = 0;
    std::size_t depth = 0;
    bool escaped = false;
    for (std::size_t cursor = 0; cursor + 1 < content.size(); ++cursor) {
      if (escaped) { escaped = false; continue; }
      if (content[cursor] == '\\') { escaped = true; continue; }
      if (content[cursor] == '[') { ++depth; continue; }
      if (content[cursor] == ']' && depth > 0) { --depth; continue; }
      if (depth == 0 && content[cursor] == '&' &&
          content[cursor + 1] == '&' && cursor > start &&
          cursor + 2 < content.size()) {
        terms.push_back(content.substr(start, cursor - start));
        start = cursor + 2;
        ++cursor;
      }
    }
    if (!terms.empty()) terms.push_back(content.substr(start));
    return terms;
  }

  static std::string translateSimpleClass(std::string_view content,
                                          const JavaFlags &flags) {
    const bool negated = !content.empty() && content.front() == '^';
    std::size_t cursor = negated ? 1 : 0;
    std::string fragment;
    fragment.reserve(content.size() * 2);
    std::vector<std::string> alternatives;
    while (cursor < content.size()) {
      if (content[cursor] == '\\') {
        if (cursor + 1 >= content.size())
          throw std::invalid_argument("trailing Java class escape");
        const char escaped = content[cursor + 1];
        const std::string predefinedFragment = classFragment(escaped, flags);
        if (!predefinedFragment.empty()) {
          // Predefined Java classes are already reduced to a class fragment.
          // They can therefore participate in unions without changing group
          // or match width.
          fragment += predefinedFragment;
          cursor += 2;
          continue;
        }
        if ((escaped == 'p' || escaped == 'P') &&
            cursor + 2 < content.size() && content[cursor + 2] == '{') {
          const std::size_t close = content.find('}', cursor + 3);
          if (close == std::string_view::npos) {
            throw std::invalid_argument("unterminated Java property escape");
          }
          if (const auto property = javaPosixProperty(
                  content.substr(cursor + 3, close - cursor - 3), flags)) {
            const std::string &translatedProperty =
                escaped == 'p' ? property->positive : property->negative;
            if (translatedProperty.size() >= 2 &&
                translatedProperty.front() == '[' &&
                translatedProperty[1] != '^' &&
                translatedProperty.back() == ']') {
              fragment.append(translatedProperty, 1,
                              translatedProperty.size() - 2);
            } else {
              alternatives.push_back(translatedProperty);
            }
            cursor = close + 1;
            continue;
          }
        }
        fragment.push_back('\\');
        fragment.push_back(escaped);
        cursor += 2;
        if ((escaped == 'p' || escaped == 'P' || escaped == 'x') &&
            cursor < content.size() && content[cursor] == '{') {
          do { fragment.push_back(content[cursor]); }
          while (cursor < content.size() && content[cursor++] != '}');
        }
        continue;
      }
      if (flags.comments &&
          std::isspace(static_cast<unsigned char>(content[cursor]))) {
        ++cursor;
        continue;
      }
      if (asciiCaseInsensitive(flags) && cursor + 2 < content.size() &&
          isAsciiLetter(content[cursor]) && content[cursor + 1] == '-' &&
          isAsciiLetter(content[cursor + 2])) {
        const char first = content[cursor];
        const char last = content[cursor + 2];
        fragment.push_back(first);
        fragment.push_back('-');
        fragment.push_back(last);
        if ((first >= 'a' && first <= 'z' && last >= 'a' && last <= 'z') ||
            (first >= 'A' && first <= 'Z' && last >= 'A' && last <= 'Z')) {
          fragment.push_back(oppositeAsciiCase(first));
          fragment.push_back('-');
          fragment.push_back(oppositeAsciiCase(last));
        }
        cursor += 3;
        continue;
      }
      fragment.push_back(content[cursor]);
      if (asciiCaseInsensitive(flags) && isAsciiLetter(content[cursor]))
        fragment.push_back(oppositeAsciiCase(content[cursor]));
      ++cursor;
    }
    if (!fragment.empty() || alternatives.empty()) {
      alternatives.push_back("[" + fragment + "]");
    }
    std::string translated;
    if (alternatives.size() == 1) {
      translated = alternatives.front();
    } else {
      translated = "(?:";
      for (std::size_t index = 0; index < alternatives.size(); ++index) {
        if (index != 0) translated.push_back('|');
        translated += alternatives[index];
      }
      translated.push_back(')');
    }
    if (!negated) return translated;
    return "(?:(?!" + translated + ")[\\s\\S])";
  }

  static std::string translateClass(std::string_view content,
                                    const JavaFlags &flags) {
    const auto intersections = splitIntersections(content);
    if (!intersections.empty()) {
      std::string translated = "(?:";
      for (const auto term : intersections) {
        std::string predicate;
        if (term.size() >= 2 && term.front() == '[' &&
            findNestedClassEnd(term, 0) + 1 == term.size()) {
          predicate = translateClass(term.substr(1, term.size() - 2), flags);
        } else {
          predicate = translateSimpleClass(term, flags);
        }
        translated += "(?=";
        translated += predicate;
        translated.push_back(')');
      }
      translated += "[\\s\\S])";
      return translated;
    }

    const bool negated = !content.empty() && content.front() == '^';
    const std::size_t first = negated ? 1 : 0;
    std::vector<std::string> alternatives;
    std::string simple;
    for (std::size_t cursor = first; cursor < content.size();) {
      if (content[cursor] == '\\') {
        const std::size_t length = cursor + 1 < content.size() ? 2 : 1;
        simple.append(content.substr(cursor, length));
        cursor += length;
        continue;
      }
      if (content[cursor] != '[') {
        simple.push_back(content[cursor++]);
        continue;
      }
      if (!simple.empty()) {
        alternatives.push_back(translateSimpleClass(simple, flags));
        simple.clear();
      }
      const std::size_t end = findNestedClassEnd(content, cursor);
      alternatives.push_back(
          translateClass(content.substr(cursor + 1, end - cursor - 1), flags));
      cursor = end + 1;
    }
    if (!simple.empty() || alternatives.empty())
      alternatives.push_back(translateSimpleClass(simple, flags));
    if (alternatives.size() == 1 && !negated) return alternatives.front();
    std::string unionExpression = "(?:";
    for (std::size_t index = 0; index < alternatives.size(); ++index) {
      if (index != 0) unionExpression.push_back('|');
      unionExpression += alternatives[index];
    }
    unionExpression.push_back(')');
    if (!negated) return unionExpression;
    return "(?:(?!" + unionExpression + ")[\\s\\S])";
  }

  static void appendStartAnchor(std::string &output,
                                const JavaFlags &flags) {
    if (!flags.multiline) output += "\\A";
    else if (flags.unixLines) output += "(?:(?<=\\n)|\\A)";
    else output += "(?:(?<=\\n)|(?<=\\r)(?!\\n)|(?<=\\x{85})|"
                   "(?<=\\x{2028})|(?<=\\x{2029})|\\A)";
  }

  static void appendEndAnchor(std::string &output, const JavaFlags &flags) {
    if (!flags.multiline) {
      output += flags.unixLines
                    ? "(?=\\n?\\z)"
                    : "(?=(?:(?:\\r\\n)|[\\n\\r\\x{85}\\x{2028}"
                      "\\x{2029}])?\\z)";
    } else if (flags.unixLines) output += "(?=\\n|\\z)";
    else output += "(?=\\r\\n|[\\n\\r\\x{85}\\x{2028}\\x{2029}]|\\z)";
  }

  std::string transformEscape(std::size_t &cursor,
                              const JavaFlags &flags) const {
    if (cursor + 1 >= input_.size())
      throw std::invalid_argument("trailing Java Pattern escape");
    const char escaped = input_[cursor + 1];
    if (escaped == 'Q') {
      std::string quoted;
      cursor = appendQuoted(cursor + 2, flags, quoted);
      return quoted;
    }
    if ((escaped == 'p' || escaped == 'P') &&
        cursor + 2 < input_.size() && input_[cursor + 2] == '{') {
      const std::size_t close = input_.find('}', cursor + 3);
      if (close == std::string_view::npos) {
        throw std::invalid_argument("unterminated Java property escape");
      }
      if (const auto property = javaPosixProperty(
              input_.substr(cursor + 3, close - cursor - 3), flags)) {
        cursor = close + 1;
        return escaped == 'p' ? property->positive : property->negative;
      }
    }
    if (const auto predefined = predefinedClass(escaped, flags);
        !predefined.empty()) {
      cursor += 2;
      return predefined;
    }
    if (escaped == 'b' || escaped == 'B') {
      cursor += 2;
      return wordBoundary(escaped == 'b');
    }
    if (asciiCaseInsensitive(flags) && (escaped == 'x' || escaped == 'u')) {
      std::size_t first = cursor + 2;
      std::size_t last = first;
      if (escaped == 'x' && first < input_.size() && input_[first] == '{') {
        first += 1;
        last = input_.find('}', first);
        if (last == std::string_view::npos) {
          throw std::invalid_argument("unterminated Java hexadecimal escape");
        }
      } else {
        last = first + (escaped == 'u' ? 4 : 2);
        if (last > input_.size()) {
          throw std::invalid_argument("short Java hexadecimal escape");
        }
      }
      unsigned int codePoint = 0;
      bool valid = first < last;
      for (std::size_t digit = first; valid && digit < last; ++digit) {
        const char value = input_[digit];
        codePoint *= 16;
        if (value >= '0' && value <= '9') codePoint += value - '0';
        else if (value >= 'a' && value <= 'f') codePoint += value - 'a' + 10;
        else if (value >= 'A' && value <= 'F') codePoint += value - 'A' + 10;
        else valid = false;
      }
      if (valid && codePoint <= 0x7f &&
          isAsciiLetter(static_cast<char>(codePoint))) {
        std::string literal;
        appendLiteral(literal, static_cast<char>(codePoint), flags);
        cursor = last + (escaped == 'x' && cursor + 2 < input_.size() &&
                         input_[cursor + 2] == '{');
        return literal;
      }
    }
    if (escaped >= '1' && escaped <= '9') {
      std::size_t end = cursor + 2;
      std::size_t identity = static_cast<std::size_t>(escaped - '0');
      while (end < input_.size() && input_[end] >= '0' &&
             input_[end] <= '9') {
        identity = identity * 10 + static_cast<std::size_t>(input_[end] - '0');
        ++end;
      }
      std::string reference(input_.substr(cursor, end - cursor));
      cursor = end;
      if (asciiCaseInsensitive(flags) && identity < captureAsciiOnly_.size() &&
          captureAsciiOnly_[identity]) {
        return "(?i:" + reference + ")";
      }
      return reference;
    }
    if (escaped == 'k' && cursor + 2 < input_.size() &&
        input_[cursor + 2] == '<') {
      const std::size_t close = input_.find('>', cursor + 3);
      if (close == std::string_view::npos || close == cursor + 3) {
        throw std::invalid_argument("invalid Java named backreference");
      }
      const std::string name(input_.substr(cursor + 3, close - cursor - 3));
      std::string reference(input_.substr(cursor, close - cursor + 1));
      cursor = close + 1;
      const auto found = namedCaptureAsciiOnly_.find(name);
      if (asciiCaseInsensitive(flags) &&
          found != namedCaptureAsciiOnly_.end() && found->second) {
        return "(?i:" + reference + ")";
      }
      return reference;
    }
    std::string preserved;
    preserved.push_back('\\');
    preserved.push_back(escaped);
    cursor += 2;
    if ((escaped == 'p' || escaped == 'P' || escaped == 'x') &&
        cursor < input_.size() && input_[cursor] == '{') {
      do { preserved.push_back(input_[cursor]); }
      while (cursor < input_.size() && input_[cursor++] != '}');
    } else if (escaped == 'u') {
      const std::size_t digits = std::min<std::size_t>(4, input_.size() - cursor);
      preserved.append(input_.substr(cursor, digits));
      cursor += digits;
    }
    return preserved;
  }

  bool groupIsAsciiOnly(std::string_view content,
                        const JavaFlags &flags) const {
    for (std::size_t cursor = 0; cursor < content.size(); ++cursor) {
      const unsigned char value = static_cast<unsigned char>(content[cursor]);
      if (value >= 0x80 || content[cursor] == '.') return false;
      if (content[cursor] == '[') {
        const std::size_t end = findNestedClassEnd(content, cursor);
        const auto classContent = content.substr(cursor + 1,
                                                 end - cursor - 1);
        if ((!classContent.empty() && classContent.front() == '^') ||
            classContent.find("\\D") != std::string_view::npos ||
            classContent.find("\\S") != std::string_view::npos ||
            classContent.find("\\W") != std::string_view::npos ||
            classContent.find("\\p") != std::string_view::npos ||
            classContent.find("\\P") != std::string_view::npos ||
            (flags.unicodeClasses &&
             (classContent.find("\\d") != std::string_view::npos ||
              classContent.find("\\s") != std::string_view::npos ||
              classContent.find("\\w") != std::string_view::npos))) {
          return false;
        }
        cursor = end;
        continue;
      }
      if (content[cursor] != '\\') continue;
      if (cursor + 1 >= content.size()) return false;
      const char escaped = content[++cursor];
      if (escaped == 'D' || escaped == 'S' || escaped == 'W' ||
          escaped == 'p' || escaped == 'P' ||
          (flags.unicodeClasses &&
           (escaped == 'd' || escaped == 's' || escaped == 'w'))) {
        return false;
      }
      if (escaped == 'x' || escaped == 'u') {
        std::size_t first = cursor + 1;
        std::size_t last = first;
        if (escaped == 'x' && first < content.size() &&
            content[first] == '{') {
          first += 1;
          last = content.find('}', first);
          if (last == std::string_view::npos) return false;
        } else {
          last = first + (escaped == 'u' ? 4 : 2);
          if (last > content.size()) return false;
        }
        unsigned int codePoint = 0;
        for (std::size_t digit = first; digit < last; ++digit) {
          const char hex = content[digit];
          codePoint *= 16;
          if (hex >= '0' && hex <= '9') codePoint += hex - '0';
          else if (hex >= 'a' && hex <= 'f') codePoint += hex - 'a' + 10;
          else if (hex >= 'A' && hex <= 'F') codePoint += hex - 'A' + 10;
          else return false;
        }
        if (codePoint >= 0x80) return false;
        cursor = last - 1 + (escaped == 'x' && cursor + 1 < content.size() &&
                             content[cursor + 1] == '{');
      }
    }
    return true;
  }

  std::string groupOpener(std::size_t &cursor) const {
    const std::size_t start = cursor++;
    if (cursor >= input_.size() || input_[cursor] != '?') return "(";
    if (cursor + 1 >= input_.size())
      throw std::invalid_argument("unterminated Java group");
    const char kind = input_[cursor + 1];
    if (kind == ':' || kind == '=' || kind == '!' || kind == '>') {
      cursor += 2;
      return std::string(input_.substr(start, 3));
    }
    if (kind == '<') {
      if (cursor + 2 < input_.size() &&
          (input_[cursor + 2] == '=' || input_[cursor + 2] == '!')) {
        cursor += 3;
        return std::string(input_.substr(start, 4));
      }
      const std::size_t close = input_.find('>', cursor + 2);
      if (close == std::string_view::npos || close == cursor + 2)
        throw std::invalid_argument("invalid Java named group");
      cursor = close + 1;
      return std::string(input_.substr(start, cursor - start));
    }
    throw std::invalid_argument("unsupported Java group syntax");
  }

  std::string transformSequence(std::size_t &cursor, JavaFlags flags,
                                bool nested) const {
    std::string output;
    while (cursor < input_.size()) {
      const char value = input_[cursor];
      if (value == ')' && nested) { ++cursor; return output; }
      if (value == ')') throw std::invalid_argument("unmatched Java group close");
      if (flags.comments && value == '#') {
        while (cursor < input_.size() && input_[cursor] != '\n' &&
               input_[cursor] != '\r') output.push_back(input_[cursor++]);
        continue;
      }
      if (value == '\\') { output += transformEscape(cursor, flags); continue; }
      if (value == '[') {
        const std::size_t end = findNestedClassEnd(input_, cursor);
        output += translateClass(input_.substr(cursor + 1, end - cursor - 1),
                                 flags);
        cursor = end + 1;
        continue;
      }
      if (value == '(') {
        FlagGroup group;
        if (parseFlagGroup(cursor, group)) {
          const JavaFlags changed = withFlags(flags, group.specification);
          if (group.terminator == ')') {
            appendEngineFlagDelta(output, flags, changed);
            flags = changed;
            cursor = group.end + 1;
          } else {
            output += "(?:";
            appendEngineFlagDelta(output, flags, changed);
            cursor = group.end + 1;
            output += transformSequence(cursor, changed, true);
            output.push_back(')');
          }
          continue;
        }
        const bool ordinaryCapture = cursor + 1 >= input_.size() ||
                                     input_[cursor + 1] != '?';
        const bool namedCapture = cursor + 3 < input_.size() &&
                                  input_[cursor + 1] == '?' &&
                                  input_[cursor + 2] == '<' &&
                                  input_[cursor + 3] != '=' &&
                                  input_[cursor + 3] != '!';
        const std::string opener = groupOpener(cursor);
        std::size_t captureIdentity = 0;
        std::string captureName;
        if (ordinaryCapture || namedCapture) {
          captureIdentity = ++nextCaptureIdentity_;
          if (captureAsciiOnly_.size() <= captureIdentity)
            captureAsciiOnly_.resize(captureIdentity + 1, false);
          if (namedCapture)
            captureName = opener.substr(3, opener.size() - 4);
        }
        output += opener;
        const std::size_t contentStart = cursor;
        output += transformSequence(cursor, flags, true);
        output.push_back(')');
        if (captureIdentity != 0) {
          const bool asciiOnly = groupIsAsciiOnly(
              input_.substr(contentStart, cursor - contentStart - 1), flags);
          captureAsciiOnly_[captureIdentity] = asciiOnly;
          if (!captureName.empty()) namedCaptureAsciiOnly_[captureName] = asciiOnly;
        }
        continue;
      }
      if (value == '.') {
        output += flags.dotAll ? "[\\s\\S]"
                 : flags.unixLines ? "[^\\n]"
                                   : "[^\\n\\r\\x{85}\\x{2028}\\x{2029}]";
        ++cursor;
        continue;
      }
      if (value == '^') { appendStartAnchor(output, flags); ++cursor; continue; }
      if (value == '$') { appendEndAnchor(output, flags); ++cursor; continue; }
      if (asciiCaseInsensitive(flags) && isAsciiLetter(value))
        appendLiteral(output, value, flags);
      else output.push_back(value);
      ++cursor;
    }
    if (nested) throw std::invalid_argument("unterminated Java group");
    return output;
  }

  std::string_view input_;
  mutable std::size_t nextCaptureIdentity_ = 0;
  mutable std::vector<bool> captureAsciiOnly_{false};
  mutable std::map<std::string, bool> namedCaptureAsciiOnly_;
};

} // namespace detail

inline AdaptedPattern adaptJavaEmbeddedFlags(std::string_view input) {
  return detail::JavaPatternAdapter(input).adapt();
}

} // namespace skin::lua_skin_java_pattern_detail
