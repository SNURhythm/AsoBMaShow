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
  static constexpr float minPlayAreaZoom = 0.5F;
  static constexpr float maxPlayAreaZoom = 3.0F;
  static constexpr float minCustomScale = 0.1F;
  static constexpr float maxCustomScale = 10.0F;
  static constexpr float minCustomTranslation = -8'192.0F;
  static constexpr float maxCustomTranslation = 8'192.0F;
};

enum class ViewportMode : std::uint8_t { Fit, Stretch, Custom };
enum class CustomViewportBase : std::uint8_t { Fit, Stretch };
enum class SkinSafetyLevel : std::uint8_t {
  Standard,
  BeatorajaCompatibility,
  Unrestricted,
};

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
  bool centerPlayArea = false;
  bool keepHudFixed = false;
  float playAreaZoom = 1.0F;
  bool operator==(const ViewportSettings &) const = default;
};

struct EntryProfileSettings {
  std::map<std::string, int> options;
  std::map<std::string, std::string> filePaths;
  std::map<std::string, ConfigOffset> offsets;
  ViewportSettings viewport;
  bool operator==(const EntryProfileSettings &) const = default;
};

// SkinConfigurationDigestV1 covers only the three persisted configuration
// maps. Viewport and every runtime/header-derived field are intentionally
// excluded so layout changes do not create a different configured skin.
[[nodiscard]] std::string
skinConfigurationDigest(const EntryProfileSettings &settings);

struct SkinProfileId {
  std::string opaque;
  auto operator<=>(const SkinProfileId &) const = default;
};

std::optional<SkinProfileId>
makeSkinProfileId(std::string_view existingPlayerProfileId);

std::optional<std::string>
normalizeSkinConfigurationKey(std::string_view value);

struct SkinProfileSettings {
  SkinSafetyLevel safetyLevel = SkinSafetyLevel::Standard;
  // Legacy gameplay-only view retained for older settings readers.
  std::map<int, SkinEntryId> selectedGameplayEntries;
  // Screen-target selection, keyed by Beatoraja SkinType or an additional
  // gameplay target (-4, -5, -6, -7, -8). New
  // settings writes use this map; selectedGameplayEntries remains readable
  // while profiles migrate from gameplay-only selection.
  std::map<int, SkinEntryId> selectedSkinEntries;
  bool follow5K1S = false;
  bool follow7K1S = false;

  [[nodiscard]] int effectiveTarget(int target) const noexcept {
    if (target == -5 && follow5K1S) return 1;
    if (target == -7 && follow7K1S) return 0;
    return target;
  }

  // Transitional derived aliases. New settings files do not write these, but
  // readers accept them so existing profiles preserve their 7K selection.
  bool gameplayCompatibilityEnabled = false;
  std::optional<SkinEntryId> selected7KeyEntry;
  std::map<SkinEntryId, EntryProfileSettings> entries;

  // Additional gameplay modes may use the same source entry, but own their
  // options, files, offsets and viewport independently of its native mode.
  std::map<int, std::map<SkinEntryId, EntryProfileSettings>> modeEntries;

  const std::map<SkinEntryId, EntryProfileSettings> &entriesForTarget(int target) const;
  std::map<SkinEntryId, EntryProfileSettings> &entriesForTarget(int target);

  void sanitize();
  bool operator==(const SkinProfileSettings &) const = default;
};

} // namespace skin
