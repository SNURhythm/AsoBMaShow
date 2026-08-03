#pragma once

#include "package/SkinPackageTypes.h"

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace skin {

struct SkinProfileSettingsPolicy {
  static constexpr std::size_t maxEntries = 64;
  static constexpr std::size_t maxOptionsPerEntry = 256;
  static constexpr std::size_t maxFilesPerEntry = 256;
  static constexpr std::size_t maxOffsetsPerEntry = 256;
  static constexpr std::size_t maxConfigurationKeyBytes = 128;
  static constexpr std::size_t maxConfigurationValueBytes = 1'024;
  static constexpr int minOffsetComponent = -32'768;
  static constexpr int maxOffsetComponent = 32'767;
  static constexpr float minCustomScale = 0.1F;
  static constexpr float maxCustomScale = 10.0F;
  static constexpr float minCustomTranslation = -8'192.0F;
  static constexpr float maxCustomTranslation = 8'192.0F;
};

enum class ViewportMode : std::uint8_t { Fit, Stretch, Custom };
enum class CustomViewportBase : std::uint8_t { Fit, Stretch };

struct ConfigOffset {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int r = 0;
  int a = 0;
  bool operator==(const ConfigOffset &) const = default;
};

struct ViewportSettings {
  ViewportMode mode = ViewportMode::Fit;
  CustomViewportBase customBase = CustomViewportBase::Fit;
  float scaleX = 1.0F;
  float scaleY = 1.0F;
  float translateX = 0.0F;
  float translateY = 0.0F;
  bool operator==(const ViewportSettings &) const = default;
};

struct EntryProfileSettings {
  std::map<std::string, int> options;
  std::map<std::string, std::string> filePaths;
  std::map<std::string, ConfigOffset> offsets;
  ViewportSettings viewport;
  bool operator==(const EntryProfileSettings &) const = default;
};

struct SkinProfileId {
  std::string opaque;
  auto operator<=>(const SkinProfileId &) const = default;
};

std::optional<SkinProfileId>
makeSkinProfileId(std::string_view existingPlayerProfileId);

std::optional<std::string>
normalizeSkinConfigurationKey(std::string_view value);

struct SkinProfileSettings {
  bool gameplayCompatibilityEnabled = false;
  std::optional<SkinEntryId> selected7KeyEntry;
  std::map<SkinEntryId, EntryProfileSettings> entries;

  void sanitize();
  bool operator==(const SkinProfileSettings &) const = default;
};

} // namespace skin
