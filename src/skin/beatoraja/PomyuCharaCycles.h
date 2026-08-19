#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

namespace pomyu_chara_cycles_detail {

inline bool equalsIgnoreCase(std::string_view left,
                             std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const char lhs = left[index];
    const char rhs = right[index];
    const char foldedLeft = lhs >= 'A' && lhs <= 'Z'
                                 ? static_cast<char>(lhs - 'A' + 'a')
                                 : lhs;
    const char foldedRight = rhs >= 'A' && rhs <= 'Z'
                                  ? static_cast<char>(rhs - 'A' + 'a')
                                  : rhs;
    if (foldedLeft != foldedRight) {
      return false;
    }
  }
  return true;
}

inline std::vector<std::string_view> splitTabs(std::string_view value) {
  std::vector<std::string_view> output;
  std::size_t begin = 0;
  while (true) {
    const std::size_t end = value.find('\t', begin);
    output.push_back(value.substr(begin, end == std::string_view::npos
                                             ? value.size() - begin
                                             : end - begin));
    if (end == std::string_view::npos) {
      return output;
    }
    begin = end + 1;
  }
}

inline std::vector<std::string> parseFields(
    const std::vector<std::string_view> &fields) {
  std::vector<std::string> output;
  for (const std::string_view field : fields) {
    if (field.empty()) {
      continue;
    }
    if (field.starts_with('/')) {
      break;
    }
    const std::size_t comment = field.find("//");
    output.emplace_back(field.substr(0, comment));
    if (comment != std::string_view::npos) {
      break;
    }
  }
  return output;
}

inline std::optional<int> parseDecimal(std::string_view value) noexcept {
  std::string digits;
  digits.reserve(value.size());
  for (const char character : value) {
    if ((character >= '0' && character <= '9') || character == '-') {
      digits.push_back(character);
    }
  }
  if (digits.empty()) {
    return std::nullopt;
  }
  int parsed = 0;
  const auto [last, error] =
      std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
  return error == std::errc{} && last == digits.data() + digits.size()
             ? std::optional<int>(parsed)
             : std::nullopt;
}

inline std::string compactDestination(std::string_view value) {
  std::string output;
  output.reserve(value.size());
  for (const char character : value) {
    if ((character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') || character == '-') {
      output.push_back(character);
    }
  }
  return output;
}

inline std::optional<std::size_t> timerIndexForMotion(int motion,
                                                       int side) noexcept {
  if (side != 2) {
    switch (motion) {
    case 1: return 0;
    case 6: return 1;
    case 7: return 2;
    case 8: return 3;
    case 10: return 4;
    default: return std::nullopt;
    }
  }
  switch (motion) {
  case 1: return 5;
  case 7: return 6;
  case 10: return 7;
  default: return std::nullopt;
  }
}

inline int multiplyAsJavaInt(int left, std::size_t right) noexcept {
  const auto product = static_cast<std::uint32_t>(left) *
                       static_cast<std::uint32_t>(right);
  return static_cast<std::int32_t>(product);
}

} // namespace pomyu_chara_cycles_detail

// Port of the PomyuCharaLoader path which writes PlaySkin.pomyu motion-cycle
// lengths. Non-PLAY objects never call setPMcharaTime, so they retain the
// supplied processor cycles unchanged.
[[nodiscard]] inline std::optional<std::array<int, 8>>
pomyuMotionCyclesFromChp(std::string_view contents, int type, int side,
                         std::array<int, 8> cycles) {
  if (type != 0) {
    return cycles;
  }

  constexpr int kUnspecified = std::numeric_limits<int>::min();
  int anime = 100;
  std::array<int, 20> frame;
  frame.fill(kUnspecified);
  std::array<std::vector<std::string_view>, 3> motionLines;

  std::size_t lineBegin = 0;
  while (lineBegin <= contents.size()) {
    const std::size_t lineEnd = contents.find('\n', lineBegin);
    std::string_view line = contents.substr(
        lineBegin, lineEnd == std::string_view::npos ? contents.size() - lineBegin
                                                      : lineEnd - lineBegin);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (line.starts_with('#')) {
      const auto raw = pomyu_chara_cycles_detail::splitTabs(line);
      if (raw.size() > 1) {
        const auto values = pomyu_chara_cycles_detail::parseFields(raw);
        if (pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0], "#Patern") ||
            pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0], "#Pattern")) {
          motionLines[0].push_back(line);
        } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0],
                                                                 "#Texture")) {
          motionLines[1].push_back(line);
        } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0],
                                                                 "#Layer")) {
          motionLines[2].push_back(line);
        } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0],
                                                                 "#Flame") ||
                   pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0],
                                                                 "#Frame")) {
          if (values.size() > 2) {
            const auto index = pomyu_chara_cycles_detail::parseDecimal(values[1]);
            const auto value = pomyu_chara_cycles_detail::parseDecimal(values[2]);
            if (!index || !value) {
              return std::nullopt;
            }
            if (*index >= 0 && *index < static_cast<int>(frame.size())) {
              frame[static_cast<std::size_t>(*index)] = *value;
            }
          }
        } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(raw[0],
                                                                 "#Anime")) {
          if (values.size() > 1) {
            const auto value = pomyu_chara_cycles_detail::parseDecimal(values[1]);
            if (!value) {
              return std::nullopt;
            }
            anime = *value;
          }
        }
      }
    }
    if (lineEnd == std::string_view::npos) {
      break;
    }
    lineBegin = lineEnd + 1;
  }

  for (int &value : frame) {
    if (value == kUnspecified) {
      value = anime;
    }
    if (value < 1) {
      value = 100;
    }
  }

  for (const auto &lines : motionLines) {
    for (const std::string_view line : lines) {
      const auto raw = pomyu_chara_cycles_detail::splitTabs(line);
      const auto values = pomyu_chara_cycles_detail::parseFields(raw);
      if (values.size() <= 1) {
        continue;
      }
      const auto motion = pomyu_chara_cycles_detail::parseDecimal(values[1]);
      if (!motion) {
        return std::nullopt;
      }
      const auto timer =
          pomyu_chara_cycles_detail::timerIndexForMotion(*motion, side);
      if (!timer) {
        continue;
      }
      std::array<std::string, 4> destination{};
      for (std::size_t index = 0; index < destination.size(); ++index) {
        if (values.size() > index + 2) {
          destination[index] =
              pomyu_chara_cycles_detail::compactDestination(values[index + 2]);
        }
      }
      if (destination[0].empty() || destination[0].size() % 2 != 0 ||
          (!destination[1].empty() &&
           destination[1].size() != destination[0].size()) ||
          (!destination[2].empty() &&
           destination[2].size() != destination[0].size()) ||
          (!destination[3].empty() &&
           destination[3].size() != destination[0].size())) {
        continue;
      }
      const int cycle = pomyu_chara_cycles_detail::multiplyAsJavaInt(
          frame[static_cast<std::size_t>(*motion)], destination[0].size() / 2);
      // PomyuCharaProcessor.setPMcharaTime ignores non-positive values.
      if (cycle >= 1) {
        cycles[*timer] = cycle;
      }
    }
  }
  return cycles;
}

} // namespace skin
