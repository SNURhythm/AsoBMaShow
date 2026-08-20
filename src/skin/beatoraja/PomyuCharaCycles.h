#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

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

template <std::size_t Capacity> struct ParsedFields {
  std::array<std::string_view, Capacity> values{};
  std::size_t size = 0;

  [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept {
    return values[index];
  }
};

template <std::size_t Capacity>
inline ParsedFields<Capacity> parseFields(std::string_view value) noexcept {
  constexpr std::size_t kMaximumRawFields = 64;
  ParsedFields<Capacity> output;
  std::size_t begin = 0;
  std::size_t rawFields = 0;
  while (output.size < Capacity && rawFields < kMaximumRawFields) {
    ++rawFields;
    const std::size_t end = value.find('\t', begin);
    std::string_view field = value.substr(
        begin, end == std::string_view::npos ? value.size() - begin
                                              : end - begin);
    if (!field.empty()) {
      if (field.starts_with('/')) {
        break;
      }
      const std::size_t comment = field.find("//");
      output.values[output.size++] = field.substr(0, comment);
      if (comment != std::string_view::npos) {
        break;
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
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

struct PomyuCharaResourceRequirements {
  std::optional<std::string> charBmp;
  std::optional<std::string> charBmp2p;
  std::optional<std::string> charTex;
  std::optional<std::string> charTex2p;
  bool hasTextureDefinitions = false;
};

[[nodiscard]] inline std::optional<PomyuCharaResourceRequirements>
pomyuResourceRequirementsFromChp(std::string_view contents,
                                 std::stop_token stop = {}) {
  constexpr std::size_t kMaximumRelevantFieldBytes = 64U * 1024U;
  PomyuCharaResourceRequirements result;
  std::size_t lineBegin = 0;
  while (lineBegin <= contents.size()) {
    if (stop.stop_requested()) {
      return std::nullopt;
    }
    const std::size_t lineEnd = contents.find('\n', lineBegin);
    std::string_view line = contents.substr(
        lineBegin,
        lineEnd == std::string_view::npos ? contents.size() - lineBegin
                                           : lineEnd - lineBegin);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    const std::size_t firstTab = line.find('\t');
    if (line.starts_with('#') && firstTab != std::string_view::npos) {
      const std::string_view command = line.substr(0, firstTab);
      if (pomyu_chara_cycles_detail::equalsIgnoreCase(command, "#Texture")) {
        result.hasTextureDefinitions = true;
      }
      std::optional<std::string> *destination = nullptr;
      if (pomyu_chara_cycles_detail::equalsIgnoreCase(command, "#CharBMP")) {
        destination = &result.charBmp;
      } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                              "#CharBMP2P")) {
        destination = &result.charBmp2p;
      } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                              "#CharTex")) {
        destination = &result.charTex;
      } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                              "#CharTex2P")) {
        destination = &result.charTex2p;
      }
      if (destination != nullptr) {
        const auto values = pomyu_chara_cycles_detail::parseFields<2>(line);
        if (values.size > 1) {
          if (values[1].size() > kMaximumRelevantFieldBytes) {
            return std::nullopt;
          }
          *destination = std::string(values[1]);
        }
      }
    }
    if (lineEnd == std::string_view::npos) {
      break;
    }
    lineBegin = lineEnd + 1;
  }
  return result;
}

// Port of the PomyuCharaLoader path which writes PlaySkin.pomyu motion-cycle
// lengths. Non-PLAY objects never call setPMcharaTime, so they retain the
// supplied processor cycles unchanged.
[[nodiscard]] inline std::optional<std::array<int, 8>>
pomyuMotionCyclesFromChp(std::string_view contents, int type, int side,
                         std::array<int, 8> cycles,
                         std::stop_token stop = {}) {
  if (type != 0) {
    return cycles;
  }

  constexpr int kUnspecified = std::numeric_limits<int>::min();
  constexpr std::size_t kMaximumRelevantFieldBytes = 64U * 1024U;
  int anime = 100;
  std::array<int, 20> frame;
  frame.fill(kUnspecified);
  const auto forEachLine = [&](const auto &visit) {
    std::size_t lineBegin = 0;
    while (lineBegin <= contents.size()) {
      if (stop.stop_requested()) {
        return false;
      }
      const std::size_t lineEnd = contents.find('\n', lineBegin);
      std::string_view line = contents.substr(
          lineBegin,
          lineEnd == std::string_view::npos ? contents.size() - lineBegin
                                             : lineEnd - lineBegin);
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      if (!visit(line)) {
        return false;
      }
      if (lineEnd == std::string_view::npos) {
        break;
      }
      lineBegin = lineEnd + 1;
    }
    return true;
  };

  if (!forEachLine([&](std::string_view line) {
        if (!line.starts_with('#')) {
          return true;
        }
        const std::size_t firstTab = line.find('\t');
        if (firstTab == std::string_view::npos) {
          return true;
        }
        const std::string_view command = line.substr(0, firstTab);
        if (pomyu_chara_cycles_detail::equalsIgnoreCase(command, "#Flame") ||
            pomyu_chara_cycles_detail::equalsIgnoreCase(command, "#Frame")) {
          const auto values =
              pomyu_chara_cycles_detail::parseFields<3>(line);
          if (values.size > 2) {
            if (values[1].size() > kMaximumRelevantFieldBytes ||
                values[2].size() > kMaximumRelevantFieldBytes) {
              return false;
            }
            const auto index =
                pomyu_chara_cycles_detail::parseDecimal(values[1]);
            const auto value =
                pomyu_chara_cycles_detail::parseDecimal(values[2]);
            if (!index || !value) {
              return false;
            }
            if (*index >= 0 && *index < static_cast<int>(frame.size())) {
              frame[static_cast<std::size_t>(*index)] = *value;
            }
          }
        } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                                "#Anime")) {
          const auto values =
              pomyu_chara_cycles_detail::parseFields<2>(line);
          if (values.size > 1) {
            if (values[1].size() > kMaximumRelevantFieldBytes) {
              return false;
            }
            const auto value =
                pomyu_chara_cycles_detail::parseDecimal(values[1]);
            if (!value) {
              return false;
            }
            anime = *value;
          }
        }
        return true;
      })) {
    return std::nullopt;
  }

  for (int &value : frame) {
    if (value == kUnspecified) {
      value = anime;
    }
    if (value < 1) {
      value = 100;
    }
  }

  for (int motionKind = 0; motionKind < 3; ++motionKind) {
    if (!forEachLine([&](std::string_view line) {
      if (!line.starts_with('#')) {
        return true;
      }
      const std::size_t firstTab = line.find('\t');
      if (firstTab == std::string_view::npos) {
        return true;
      }
      const std::string_view command = line.substr(0, firstTab);
      const bool matches =
          motionKind == 0
              ? (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                               "#Patern") ||
                 pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                               "#Pattern"))
              : motionKind == 1
                    ? pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                                   "#Texture")
                    : pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                                   "#Layer");
      if (!matches) {
        return true;
      }
      const auto values = pomyu_chara_cycles_detail::parseFields<6>(line);
      if (values.size <= 1) {
        return true;
      }
      if (values[1].size() > kMaximumRelevantFieldBytes) {
        return false;
      }
      const auto motion = pomyu_chara_cycles_detail::parseDecimal(values[1]);
      if (!motion) {
        return false;
      }
      const auto timer =
          pomyu_chara_cycles_detail::timerIndexForMotion(*motion, side);
      if (!timer) {
        return true;
      }
      std::array<std::string, 4> destination{};
      for (std::size_t index = 0; index < destination.size(); ++index) {
        if (values.size > index + 2) {
          if (values[index + 2].size() > kMaximumRelevantFieldBytes) {
            return false;
          }
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
        return true;
      }
      const int cycle = pomyu_chara_cycles_detail::multiplyAsJavaInt(
          frame[static_cast<std::size_t>(*motion)], destination[0].size() / 2);
      // PomyuCharaProcessor.setPMcharaTime ignores non-positive values.
      if (cycle >= 1) {
        cycles[*timer] = cycle;
      }
      return true;
    })) {
      return std::nullopt;
    }
  }
  return cycles;
}

} // namespace skin
