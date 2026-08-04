#pragma once

#include "../SkinProfileSettings.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace skin {

struct ConfiguredOption {
  std::string name;
  int value = 0;
};

struct ConfiguredFile {
  std::string name;
  std::string pattern;
  std::string selectedValue;
};

using OffsetPermissionMask = std::uint8_t;
inline constexpr OffsetPermissionMask kOffsetPermissionX = 1U << 0U;
inline constexpr OffsetPermissionMask kOffsetPermissionY = 1U << 1U;
inline constexpr OffsetPermissionMask kOffsetPermissionW = 1U << 2U;
inline constexpr OffsetPermissionMask kOffsetPermissionH = 1U << 3U;
inline constexpr OffsetPermissionMask kOffsetPermissionR = 1U << 4U;
inline constexpr OffsetPermissionMask kOffsetPermissionA = 1U << 5U;

struct BeatorajaSkinConfiguration {
  std::vector<ConfiguredOption> orderedOptions;
  std::map<std::string, int> options;
  std::set<int> enabledOptionIds;
  std::vector<ConfiguredFile> orderedFiles;
  std::map<std::string, std::string> filePaths;
  std::map<std::string, ConfigOffset> offsets;
  std::map<std::string, OffsetPermissionMask> offsetPermissions;
  std::map<int, ConfigOffset> offsetsById;
  std::string lowercaseSha256;
};

[[nodiscard]] std::string
skinConfigurationDigest(const BeatorajaSkinConfiguration &configuration);

} // namespace skin
