#include "scene/GameplaySkinSettingsPresentation.h"
#include "skin/beatoraja/BeatorajaSkinConfiguration.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

skin::SkinEntryId entryId(std::string suffix = {}) {
  return {.package = {.directoryName = "package" + suffix,
                      .collisionKey = "package-key" + suffix},
          .packageRelativePath = "play/7key" + suffix + ".lua",
          .collisionKey = "entry-key" + suffix};
}

skin::GameplaySkinEntryRow entryRow() {
  skin::GameplaySkinEntryRow row;
  row.entry = entryId();
  row.metadata.displayName = "Display";
  row.metadata.author = "Author";
  row.metadata.skinType = 0;
  row.metadata.authoredWidth = 1280;
  row.metadata.authoredHeight = 720;
  row.metadata.categories = {{.name = "Mode", .items = {"Normal", "Mirror"}}};
  row.metadata.options = {
      {.category = "Lane",
       .name = "Lane cover",
       .choices = {{.label = "Off", .value = 0}, {.label = "On", .value = 1}},
       .defaultLabel = "Off"}};
  row.metadata.files = {{.category = "Sound",
                         .name = "Judge sound",
                         .pattern = "*.wav",
                         .defaultValue = "default.wav",
                         .choices = {"default.wav", "quiet.wav"}}};
  row.metadata.offsets = {
      {.category = "Judge",
       .name = "Judge position",
       .id = 7,
       .permissions = skin::kOffsetPermissionX | skin::kOffsetPermissionY}};
  row.revisionDigest = "same-revision";
  row.configurationDigest = "same-configuration";
  row.validation = skin::SkinValidationDisposition::Selectable7Key;
  row.settings.options["Lane cover"] = 1;
  row.settings.filePaths["Judge sound"] = "quiet.wav";
  row.settings.offsets["Judge position"] = {
      .x = 1, .y = 2, .w = 3, .h = 4, .r = 5, .a = 6};
  row.settings.viewport = {.mode = skin::ViewportMode::Custom,
                           .customBase = skin::CustomViewportBase::Stretch,
                           .scaleX = 1.25F,
                           .scaleY = 0.75F,
                           .translateX = 17.0F,
                           .translateY = -9.0F};
  row.diagnostics = {
      {.code = "warning",
       .message = "Something changed",
       .virtualPath = "play/7key.lua",
       .severity = skin::DiagnosticSeverity::Warning,
       .source = skin::SkinSourceLocation{
           .virtualPath = "play/7key.lua", .line = 2, .column = 4}}};
  return row;
}

skin::GameplaySkinSettingsSnapshot snapshotWithEntry() {
  skin::GameplaySkinSettingsSnapshot snapshot;
  snapshot.state = skin::GameplaySkinSettingsState::Ready;
  snapshot.featureAvailable = true;
  snapshot.compatibilityEnabled = true;
  snapshot.selected7KeyEntry = entryId();
  snapshot.selectedGameplayEntries.emplace(0, entryId());
  snapshot.entries = {entryRow()};
  snapshot.hasPackageProgress = true;
  snapshot.progress = {.phase = skin::SkinProgressPhase::Publishing,
                       .completedBytes = 10,
                       .totalBytes = 20,
                       .completedFiles = 3};
  snapshot.statusMessage = "Ready";
  return snapshot;
}

template <typename Mutation>
void requirePresentationChange(const skin::GameplaySkinSettingsSnapshot &base,
                               Mutation mutation, const char *message) {
  auto changed = base;
  mutation(changed);
  require(skin::gameplaySkinSettingsPresentationKey(base) !=
              skin::gameplaySkinSettingsPresentationKey(changed),
          message);
}

void testCollisionConfirmationAvailabilityIsExact() {
  skin::GameplaySkinSettingsSnapshot snapshot;
  snapshot.state = skin::GameplaySkinSettingsState::Ready;
  snapshot.canCancel = true;
  snapshot.preparedName = skin::SkinPackageNameSuggestion{
      .originalSourceName = "source", .suggestedPackageName = "skin"};

  auto availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(!availability.ordinaryActions,
          "ordinary actions stay disabled during collision confirmation");
  require(availability.canCancel, "collision confirmation remains cancellable");
  require(!availability.canEditPreparedName,
          "automatic installs never expose a package-name edit gate");
  require(availability.canInstallPrepared,
          "a collision confirmation permits replacement");

  snapshot.preparedName->validationError = "invalid";
  availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(!availability.canEditPreparedName,
          "an invalid stale suggestion never exposes name editing");
  require(!availability.canInstallPrepared,
          "invalid NameReady cannot be installed");

  snapshot.state = skin::GameplaySkinSettingsState::Busy;
  snapshot.preparedName->validationError.clear();
  availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(!availability.canEditPreparedName &&
              !availability.canInstallPrepared && availability.canCancel,
          "Busy never exposes collision confirmation actions");

  snapshot.state = skin::GameplaySkinSettingsState::Ready;
  snapshot.canCancel = false;
  availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(availability.ordinaryActions && !availability.canEditPreparedName &&
              !availability.canInstallPrepared,
          "idle Ready with stale name data is not a collision confirmation");

  snapshot.canCancel = true;
  snapshot.preparedName.reset();
  availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(!availability.ordinaryActions && availability.canCancel &&
              !availability.canEditPreparedName &&
              !availability.canInstallPrepared,
          "a cancellable Ready snapshot without a prepared name is not a "
          "collision confirmation");

  snapshot.state = skin::GameplaySkinSettingsState::Error;
  snapshot.canCancel = false;
  snapshot.preparedName = skin::SkinPackageNameSuggestion{
      .originalSourceName = "stale", .suggestedPackageName = "stale"};
  availability = skin::gameplaySkinSettingsActionAvailability(snapshot);
  require(availability.ordinaryActions && !availability.canCancel &&
              !availability.canEditPreparedName &&
              !availability.canInstallPrepared,
          "terminal Error is not a collision confirmation even when stale "
          "name data exists");
}

void testMetadataChangesInvalidateAnUnchangedDigest() {
  const auto base = snapshotWithEntry();
  requirePresentationChange(
      base,
      [](auto &value) { value.entries[0].metadata.displayName = "Other"; },
      "display-name changes invalidate presentation");
  requirePresentationChange(
      base, [](auto &value) { value.entries[0].metadata.author = "Other"; },
      "author changes invalidate presentation");
  requirePresentationChange(
      base, [](auto &value) { value.entries[0].metadata.authoredWidth = 1920; },
      "authored dimensions invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.entries[0].metadata.options[0].choices[0].label = "Disabled";
      },
      "option labels invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.entries[0].metadata.files[0].choices[0] = "replacement.wav";
      },
      "file choices invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.entries[0].metadata.offsets[0].permissions |=
            skin::kOffsetPermissionW;
      },
      "offset permissions invalidate presentation");
}

void testActionDrivingChangesInvalidatePresentation() {
  const auto base = snapshotWithEntry();
  requirePresentationChange(
      base, [](auto &value) { value.canCancel = true; },
      "cancellability changes invalidate presentation");
  requirePresentationChange(
      base, [](auto &value) { value.hasPackageProgress = false; },
      "package-progress visibility changes invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.rescanProgress.phase =
            skin::SkinRescanProgressPhase::ScanningVisiblePackages;
      },
      "rescan lifecycle phase changes invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.collisionPackage = skin::SkinPackageId{
            .directoryName = "collision", .collisionKey = "collision-key"};
      },
      "collision changes invalidate presentation");
  requirePresentationChange(
      base, [](auto &value) { value.featureAvailable = false; },
      "feature availability changes invalidate presentation");
  requirePresentationChange(
      base,
      [](auto &value) {
        value.selectedGameplayEntries.emplace(1, entryId("-5k"));
      },
      "trait-specific selection changes invalidate presentation");
}

void testPresentationEncodingHasNoDelimiterOrOptionalAmbiguity() {
  auto left = snapshotWithEntry();
  left.preparedName =
      skin::SkinPackageNameSuggestion{.originalSourceName = "source",
                                      .suggestedPackageName = "a:b",
                                      .validationError = "c"};
  auto right = left;
  right.preparedName->suggestedPackageName = "a";
  right.preparedName->validationError = "b:c";
  require(skin::gameplaySkinSettingsPresentationKey(left) !=
              skin::gameplaySkinSettingsPresentationKey(right),
          "length-prefixed strings cannot shift delimiters between fields");

  skin::SkinDiagnosticHistoryRecord record;
  record.recordSerial = 1;
  record.entry = entryId("-history");
  record.revisionDigest = "revision";
  record.configurationDigest = "configuration";
  record.diagnostic = {.code = "code", .message = "message"};
  left.history = {record};
  left.history[0].luaLine = 7;
  left.history[0].frameSerial.reset();
  right = left;
  right.history[0].luaLine.reset();
  right.history[0].frameSerial = 7;
  require(skin::gameplaySkinSettingsPresentationKey(left) !=
              skin::gameplaySkinSettingsPresentationKey(right),
          "optional history fields encode presence and identity");
}

void testCachedControllerPresentationKeyAvoidsReencodingStaticCatalogRows() {
  auto snapshot = snapshotWithEntry();
  snapshot.cachedPresentationKey = "catalog:12;profile:9;state:ready";
  require(skin::gameplaySkinSettingsPresentationKey(snapshot) ==
              snapshot.cachedPresentationKey,
          "controller-projected settings reuse their cached catalog key");
}

void testCatalogItemsFollowBeatorajaCategoryAndOtherOrder() {
  skin::SkinEntryMetadataSnapshot metadata;
  metadata.categories = {
      {.name = "Gameplay", .items = {"lane", "judge"}},
      {.name = "Appearance", .items = {"appearance"}},
  };
  metadata.options = {
      {.category = "lane", .name = "Lane position"},
      {.category = "", .name = "---------PLAY OPTION---------"},
      {.category = "appearance", .name = "Frame"},
  };
  metadata.files = {
      {.category = "judge", .name = "Judge font"},
      {.category = "", .name = "Loose file"},
  };
  metadata.offsets = {
      {.category = "lane", .name = "Lane offset"},
      {.category = "appearance", .name = "Frame offset"},
      {.category = "", .name = "Loose offset"},
  };

  const auto items = skin::gameplaySkinSettingsCatalogItems(metadata);
  const std::vector<skin::GameplaySkinCatalogItem> expected = {
      {.kind = skin::GameplaySkinCatalogItemKind::CategoryHeading,
       .label = "Gameplay"},
      {.kind = skin::GameplaySkinCatalogItemKind::Offset,
       .declarationIndex = 0},
      {.kind = skin::GameplaySkinCatalogItemKind::File,
       .declarationIndex = 0},
      {.kind = skin::GameplaySkinCatalogItemKind::Separator},
      {.kind = skin::GameplaySkinCatalogItemKind::CategoryHeading,
       .label = "Appearance"},
      {.kind = skin::GameplaySkinCatalogItemKind::Offset,
       .declarationIndex = 1},
      {.kind = skin::GameplaySkinCatalogItemKind::Separator},
      {.kind = skin::GameplaySkinCatalogItemKind::CategoryHeading,
       .label = "Other"},
      {.kind = skin::GameplaySkinCatalogItemKind::Option,
       .declarationIndex = 0},
      {.kind = skin::GameplaySkinCatalogItemKind::Option,
       .declarationIndex = 1},
      {.kind = skin::GameplaySkinCatalogItemKind::Option,
       .declarationIndex = 2},
      {.kind = skin::GameplaySkinCatalogItemKind::File,
       .declarationIndex = 1},
      {.kind = skin::GameplaySkinCatalogItemKind::Offset,
       .declarationIndex = 2},
  };
  require(items.size() == expected.size(),
          "catalog projection has the pinned Beatoraja item count");
  if (items.size() != expected.size()) {
    return;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(items[index].kind == expected[index].kind &&
                items[index].declarationIndex == expected[index].declarationIndex &&
                items[index].label == expected[index].label,
            "catalog projection preserves Beatoraja category and Other order");
  }
}

void testSkinPackageProgressUsesMeasuredWork() {
  require(
      skin::gameplaySkinPackageProgressDisplayText(
          {.phase = skin::SkinProgressPhase::Copying,
           .completedBytes = 1'536,
           .totalBytes = 3'072,
           .completedFiles = 7}) ==
          "Copying skin files — 50% (1.5 KB / 3.0 KB) • 7 files",
      "copy progress presents the operation service's measured byte and file work");
  require(skin::gameplaySkinPackageProgressDisplayText(
              {.phase = skin::SkinProgressPhase::Validating,
               .completedFiles = 3}) == "Validating skin • 3 files",
          "non-byte validation progress remains meaningful to the user");
}

void testSkinRescanProgressAvoidsInventedWorkTotals() {
  require(skin::gameplaySkinRescanProgressDisplayText(
              {.phase = skin::SkinRescanProgressPhase::LoadingProfileInventory}) ==
              "Loading skin profile inventory",
          "profile inventory has a concrete rescan status without fake totals");
  require(skin::gameplaySkinRescanProgressDisplayText(
              {.phase = skin::SkinRescanProgressPhase::ScanningVisiblePackages,
               .packageProgress = {.phase = skin::SkinProgressPhase::Copying,
                                   .completedBytes = 1,
                                   .totalBytes = 2,
                                   .completedFiles = 1}}) ==
              "Copying skin files — 50% (1 B / 2 B) • 1 file",
          "package scan delegates to measured package-worker progress");
}

void testViewportModeChangesPreserveEveryOtherField() {
  const skin::ViewportSettings original = {
      .mode = skin::ViewportMode::Custom,
      .customBase = skin::CustomViewportBase::Stretch,
      .scaleX = 1.5F,
      .scaleY = 0.625F,
      .translateX = 23.0F,
      .translateY = -31.0F,
  };

  for (const auto mode : {skin::ViewportMode::Fit, skin::ViewportMode::Stretch,
                          skin::ViewportMode::Custom}) {
    auto expected = original;
    expected.mode = mode;
    require(skin::gameplaySkinViewportWithMode(original, mode) == expected,
            "mode switch preserves customBase and every numeric field");
  }

  auto expected = original;
  expected.mode = skin::ViewportMode::Custom;
  expected.customBase = skin::CustomViewportBase::Fit;
  require(skin::gameplaySkinViewportWithCustomBase(
              original, skin::CustomViewportBase::Fit) == expected,
          "custom-base switch preserves all numeric fields");
}

} // namespace

int main() {
  testCollisionConfirmationAvailabilityIsExact();
  testMetadataChangesInvalidateAnUnchangedDigest();
  testActionDrivingChangesInvalidatePresentation();
  testPresentationEncodingHasNoDelimiterOrOptionalAmbiguity();
  testCachedControllerPresentationKeyAvoidsReencodingStaticCatalogRows();
  testCatalogItemsFollowBeatorajaCategoryAndOtherOrder();
  testSkinPackageProgressUsesMeasuredWork();
  testSkinRescanProgressAvoidsInventedWorkTotals();
  testViewportModeChangesPreserveEveryOtherField();
  return 0;
}
