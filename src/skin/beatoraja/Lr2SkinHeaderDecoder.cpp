#include "Lr2SkinHeaderDecoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>
#include <string>

namespace skin {
namespace {

int parseInteger(std::string_view value) {
  int result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::invalid_argument("invalid LR2 integer");
  }
  return result;
}

std::string upperAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](char character) {
    return character >= 'a' && character <= 'z'
               ? static_cast<char>(character - ('a' - 'A'))
               : character;
  });
  return value;
}

void replaceAll(std::string &value, std::string_view needle,
                std::string_view replacement) {
  if (needle.empty()) return;
  std::size_t position = 0;
  while ((position = value.find(needle, position)) != std::string::npos) {
    value.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
}

bool recognizedSkinType(int type) { return type >= 0 && type <= 18; }

bool gameplayHeaderType(int type) {
  switch (type) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
  case 16:
  case 17:
    return true;
  default:
    return false;
  }
}

void addGameplayDefaults(BeatorajaSkinHeader &header) {
  header.options.push_back(
      {.name = "BGA Size", .choices = {{"Normal", 30}, {"Extend", 31}}});
  header.options.push_back(
      {.name = "Ghost",
       .choices = {{"Off", 34}, {"Type A", 35}, {"Type B", 36},
                   {"Type C", 37}}});
  header.options.push_back(
      {.name = "Score Graph", .choices = {{"Off", 38}, {"On", 39}}});
  header.options.push_back(
      {.name = "Judge Detail",
       .choices = {{"Off", 1997}, {"EARLY/LATE", 1998}, {"+-ms", 1999}}});

  header.offsets.push_back({.name = "All offset(%)",
                            .id = 10,
                            .permissions = kOffsetPermissionX |
                                           kOffsetPermissionY |
                                           kOffsetPermissionW |
                                           kOffsetPermissionH});
  header.offsets.push_back({.name = "Notes offset",
                            .id = 30,
                            .permissions = kOffsetPermissionH});
  header.offsets.push_back({.name = "Judge offset",
                            .id = 32,
                            .permissions = kOffsetPermissionX |
                                           kOffsetPermissionY |
                                           kOffsetPermissionW |
                                           kOffsetPermissionH |
                                           kOffsetPermissionA});
  header.offsets.push_back({.name = "Judge Detail offset",
                            .id = 33,
                            .permissions = kOffsetPermissionX |
                                           kOffsetPermissionY |
                                           kOffsetPermissionW |
                                           kOffsetPermissionH |
                                           kOffsetPermissionA});
}

std::string numericCharacters(std::string_view value) {
  std::string result;
  for (char character : value) {
    if ((character >= '0' && character <= '9') || character == '-') {
      result.push_back(character);
    }
  }
  return result;
}

SkinDiagnostic headerDiagnostic(const Lr2SkinCommand &command) {
  return {.code = "skin_lr2_header_command_invalid",
          .message = "Invalid LR2 header command #" + command.name,
          .virtualPath = command.source.virtualPath,
          .severity = DiagnosticSeverity::Error,
          .source = command.source};
}

void requireFields(const Lr2SkinCommand &command, std::size_t count) {
  if (command.fields.size() < count) {
    throw std::out_of_range("missing LR2 header field");
  }
}

void decodeInformation(const Lr2SkinCommand &command,
                       BeatorajaSkinHeader &header) {
  requireFields(command, 3);
  const int authoredType = parseInteger(command.fields[0]);
  header.type = recognizedSkinType(authoredType) ? authoredType : -1;
  header.name = command.fields[1];
  header.author = command.fields[2];
  if (!recognizedSkinType(authoredType)) {
    throw std::out_of_range("unknown LR2 skin type");
  }
  if (gameplayHeaderType(authoredType)) addGameplayDefaults(header);
}

void decodeResolution(const Lr2SkinCommand &command,
                      BeatorajaSkinHeader &header) {
  requireFields(command, 1);
  constexpr std::array<std::pair<int, int>, 4> resolutions{{
      {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160}}};
  const int index = parseInteger(command.fields[0]);
  if (index < 0 || static_cast<std::size_t>(index) >= resolutions.size()) {
    throw std::out_of_range("unknown LR2 resolution");
  }
  header.width = resolutions[static_cast<std::size_t>(index)].first;
  header.height = resolutions[static_cast<std::size_t>(index)].second;
}

void decodeCustomOption(const Lr2SkinCommand &command,
                        BeatorajaSkinHeader &header) {
  requireFields(command, 2);
  SkinHeaderOption option{.name = command.fields[0]};
  std::vector<std::string> contents;
  for (std::size_t index = 2; index < command.fields.size(); ++index) {
    if (!command.fields[index].empty()) {
      contents.push_back(command.fields[index]);
    }
  }
  for (std::size_t index = 0; index < contents.size(); ++index) {
    option.choices.push_back(
        {.label = std::move(contents[index]),
         .value = parseInteger(command.fields[1]) + static_cast<int>(index)});
  }
  header.options.push_back(std::move(option));
}

void decodeCustomFile(const Lr2SkinCommand &command,
                      BeatorajaSkinHeader &header,
                      std::string_view skinPath) {
  requireFields(command, 2);
  std::string pattern = command.fields[1];
  replaceAll(pattern, "LR2files\\Theme", skinPath);
  std::ranges::replace(pattern, '\\', '/');
  header.files.push_back({.name = command.fields[0],
                          .pattern = std::move(pattern),
                          .defaultValue = command.fields.size() >= 3
                                              ? command.fields[2]
                                              : std::string{}});
}

void decodeCustomOffset(const Lr2SkinCommand &command,
                        BeatorajaSkinHeader &header) {
  requireFields(command, 2);
  constexpr std::array<OffsetPermissionMask, 6> bits{
      kOffsetPermissionX, kOffsetPermissionY, kOffsetPermissionW,
      kOffsetPermissionH, kOffsetPermissionR, kOffsetPermissionA};
  OffsetPermissionMask permissions = 0;
  for (std::size_t index = 0; index < bits.size(); ++index) {
    const bool allowed = index + 2 >= command.fields.size() ||
                         parseInteger(command.fields[index + 2]) > 0;
    if (allowed) permissions |= bits[index];
  }
  header.offsets.push_back({.name = command.fields[0],
                            .id = parseInteger(command.fields[1]),
                            .permissions = permissions});
}

void decodeAdditionSettings(const Lr2SkinCommand &command,
                            BeatorajaSkinHeader &header) {
  constexpr std::array<std::string_view, 4> names{
      "BGA Size", "Ghost", "Score Graph", "Judge Detail"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    requireFields(command, index + 1);
    if (numericCharacters(command.fields[index]) != "0") continue;
    const auto found = std::find_if(
        header.options.rbegin(), header.options.rend(), [&](const auto &option) {
          return option.name == names[index];
        });
    if (found != header.options.rend()) {
      header.options.erase(std::next(found).base());
    }
  }
}

} // namespace

HeaderDecodeResult
Lr2SkinHeaderDecoder::decode(std::span<const Lr2SkinCommand> commands,
                             std::string_view skinPath) const {
  HeaderDecodeResult result;
  BeatorajaSkinHeader header;
  header.width = 640;
  header.height = 480;
  for (const auto &command : commands) {
    // LR2SkinHeaderLoader registers INCLUDE as a no-op and never opens the
    // included document during its header pass.
    if (command.includeChain.size() > 1) continue;
    try {
      const std::string name = upperAscii(command.name);
      if (name == "INFORMATION") {
        decodeInformation(command, header);
      } else if (name == "RESOLUTION") {
        decodeResolution(command, header);
      } else if (name == "CUSTOMOPTION") {
        decodeCustomOption(command, header);
      } else if (name == "CUSTOMFILE") {
        decodeCustomFile(command, header, skinPath);
      } else if (name == "CUSTOMOFFSET") {
        decodeCustomOffset(command, header);
      } else if (name == "CUSTOMOPTION_ADDITION_SETTING") {
        decodeAdditionSettings(command, header);
      }
    } catch (...) {
      result.diagnostics.push_back(headerDiagnostic(command));
    }
  }
  result.header = std::move(header);
  return result;
}

} // namespace skin
