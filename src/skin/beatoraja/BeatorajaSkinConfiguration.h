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
  std::vector<std::string> choices;
};

// Materialized values behind Beatoraja's persisted Random sentinels. Profile
// state and its digest retain `-1`/`Random`; replay export carries this value
// only between its course preflight and the later stage session.
struct RuntimeConfiguredFileSelection {
  std::string name;
  std::string pattern;
  std::string selectedValue;

  bool operator==(const RuntimeConfiguredFileSelection &) const = default;
};

struct RuntimeSkinConfigurationSelection {
  std::vector<ConfiguredOption> orderedOptions;
  std::vector<RuntimeConfiguredFileSelection> orderedFiles;
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
  // LR2SkinHeaderLoader seeds every declared choice into one shared 0/1 map;
  // negative conditions distinguish a known false choice from an unknown
  // runtime-only MainState property through this complete state.
  std::map<int, int> optionStates;
  std::vector<ConfiguredFile> orderedFiles;
  std::map<std::string, std::string> filePaths;
  std::map<std::string, ConfigOffset> offsets;
  std::map<std::string, OffsetPermissionMask> offsetPermissions;
  std::map<int, ConfigOffset> offsetsById;
  std::string lowercaseSha256;
};

[[nodiscard]] inline RuntimeSkinConfigurationSelection
runtimeSkinConfigurationSelection(const BeatorajaSkinConfiguration &configuration) {
  RuntimeSkinConfigurationSelection selection;
  selection.orderedOptions = configuration.orderedOptions;
  selection.orderedFiles.reserve(configuration.orderedFiles.size());
  for (const auto &file : configuration.orderedFiles) {
    selection.orderedFiles.push_back(
        {.name = file.name,
         .pattern = file.pattern,
         .selectedValue = file.selectedValue});
  }
  return selection;
}

[[nodiscard]] std::string
skinConfigurationDigest(const BeatorajaSkinConfiguration &configuration);

} // namespace skin
