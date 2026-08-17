#pragma once

#include "GameplaySkinSettingsController.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct GameplaySkinSettingsActionAvailability {
  bool ordinaryActions = false;
  bool canCancel = false;
  bool canEditPreparedName = false;
  bool canInstallPrepared = false;
};

// The native settings screen consumes this projection in the same sequence as
// Beatoraja's SkinConfigurationView: declared category heading, its resolved
// items, a separator, then ungrouped declarations under Other.  Declaration
// indices address the corresponding metadata vectors.
enum class GameplaySkinCatalogItemKind {
  CategoryHeading,
  Separator,
  Option,
  File,
  Offset,
};

struct GameplaySkinCatalogItem {
  GameplaySkinCatalogItemKind kind = GameplaySkinCatalogItemKind::Separator;
  std::size_t declarationIndex = 0;
  std::string label;
};

[[nodiscard]] std::vector<GameplaySkinCatalogItem>
gameplaySkinSettingsCatalogItems(const SkinEntryMetadataSnapshot &metadata);

// Invalid and unavailable entries are not selectable gameplay skins, but they
// must remain reachable for revalidation and removal.
[[nodiscard]] std::vector<const GameplaySkinEntryRow *>
gameplaySkinManagementEntries(const GameplaySkinSettingsSnapshot &snapshot);

[[nodiscard]] GameplaySkinSettingsActionAvailability
gameplaySkinSettingsActionAvailability(
    const GameplaySkinSettingsSnapshot &snapshot) noexcept;

// The native Built-in trait panel replaces the legacy controls only when the
// Lua feature and its settings controller are both available. Otherwise the
// normal Timing, Visual, and Lane tabs retain those controls.
[[nodiscard]] bool gameplaySkinTraitsRuntimeAvailable(
    bool luaFeatureAvailable, bool controllerAvailable) noexcept;

// Returns a canonical, unambiguous encoding of every snapshot field that can
// affect the rendered settings tab or any action exposed by it.
[[nodiscard]] std::string gameplaySkinSettingsPresentationKey(
    const GameplaySkinSettingsSnapshot &snapshot);

// Returns the subset of snapshot state that changes the shape of the native
// settings tab. Live operation state and committed configuration values are
// updated without reconstructing the full view tree.
[[nodiscard]] std::string gameplaySkinSettingsLayoutKey(
    const GameplaySkinSettingsSnapshot &snapshot);

// Formats only progress emitted by the package worker. The picker and
// profile-inventory phases deliberately remain status text because they do
// not have measured byte/file totals.
[[nodiscard]] std::string
gameplaySkinPackageProgressDisplayText(const SkinProgress &progress);

[[nodiscard]] std::string
gameplaySkinRescanProgressDisplayText(const SkinRescanProgress &progress);

[[nodiscard]] ViewportSettings
gameplaySkinViewportWithMode(ViewportSettings current,
                             ViewportMode mode) noexcept;

[[nodiscard]] ViewportSettings
gameplaySkinViewportWithCustomBase(ViewportSettings current,
                                   CustomViewportBase customBase) noexcept;

// These values are written back into retained text inputs after an accepted
// edit, keeping the field aligned with the queued and persisted value.
[[nodiscard]] int gameplaySkinSanitizedOffsetComponent(
    std::string_view text, int fallback) noexcept;

[[nodiscard]] float gameplaySkinSanitizedViewportComponent(
    std::string_view text, float fallback, float minimum, float maximum);

} // namespace skin
