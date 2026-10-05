#include <tuple>
#include "../i18n/Localization.h"
#include "GameplaySkinSettingsPresentation.h"
#include "../skin/beatoraja/GameplaySkinSourceFormat.h"

#include <bit>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace skin {

bool gameplaySkinPreviewCanReload(const GameplaySkinSettingsSnapshot &snapshot,
                                bool presentationReady) noexcept {
  return snapshot.state != GameplaySkinSettingsState::Busy && presentationReady;
}

namespace {

class PresentationKeyEncoder {
public:
  void boolean(bool value) {
    value_.push_back('B');
    value_.push_back(value ? '1' : '0');
  }

  void signedNumber(std::int64_t value) {
    value_.push_back('I');
    appendDecimal(value);
    value_.push_back(';');
  }

  void unsignedNumber(std::uint64_t value) {
    value_.push_back('U');
    appendDecimal(value);
    value_.push_back(';');
  }

  void floatingPoint(float value) {
    value_.push_back('F');
    appendDecimal(std::bit_cast<std::uint32_t>(value));
    value_.push_back(';');
  }

  void text(std::string_view value) {
    value_.push_back('S');
    appendDecimal(value.size());
    value_.push_back(':');
    value_.append(value);
  }

  [[nodiscard]] std::string finish() && { return std::move(value_); }

private:
  template <typename Number> void appendDecimal(Number value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    value_.append(buffer, result.ptr);
  }

  std::string value_;
};

template <typename Enum>
void encodeEnum(PresentationKeyEncoder &encoder, Enum value) {
  encoder.unsignedNumber(static_cast<std::uint64_t>(value));
}

void encodePackage(PresentationKeyEncoder &encoder,
                   const SkinPackageId &package) {
  encoder.text(package.directoryName);
  encoder.text(package.collisionKey);
}

void encodeEntry(PresentationKeyEncoder &encoder, const SkinEntryId &entry) {
  encodePackage(encoder, entry.package);
  encoder.text(entry.packageRelativePath);
  encoder.text(entry.collisionKey);
}

void encodeDiagnostic(PresentationKeyEncoder &encoder,
                      const SkinDiagnostic &diagnostic) {
  encoder.text(diagnostic.code);
  encoder.text(diagnostic.message);
  encoder.text(diagnostic.virtualPath);
  encodeEnum(encoder, diagnostic.severity);
  encoder.boolean(diagnostic.source.has_value());
  if (diagnostic.source) {
    encoder.text(diagnostic.source->virtualPath);
    encoder.unsignedNumber(diagnostic.source->line);
    encoder.unsignedNumber(diagnostic.source->column);
  }
}

void encodeMetadata(PresentationKeyEncoder &encoder,
                    const SkinEntryMetadataSnapshot &metadata) {
  encoder.text(metadata.displayName);
  encoder.text(metadata.author);
  encoder.signedNumber(metadata.skinType);
  encoder.signedNumber(metadata.authoredWidth);
  encoder.signedNumber(metadata.authoredHeight);

  encoder.unsignedNumber(metadata.categories.size());
  for (const auto &category : metadata.categories) {
    encoder.text(category.name);
    encoder.unsignedNumber(category.items.size());
    for (const auto &item : category.items) {
      encoder.text(item);
    }
  }

  encoder.unsignedNumber(metadata.options.size());
  for (const auto &option : metadata.options) {
    encoder.text(option.category);
    encoder.text(option.name);
    encoder.unsignedNumber(option.choices.size());
    for (const auto &choice : option.choices) {
      encoder.text(choice.label);
      encoder.signedNumber(choice.value);
    }
    encoder.text(option.defaultLabel);
  }

  encoder.unsignedNumber(metadata.files.size());
  for (const auto &file : metadata.files) {
    encoder.text(file.category);
    encoder.text(file.name);
    encoder.text(file.pattern);
    encoder.text(file.defaultValue);
    encoder.unsignedNumber(file.choices.size());
    for (const auto &choice : file.choices) {
      encoder.text(choice);
    }
  }

  encoder.unsignedNumber(metadata.offsets.size());
  for (const auto &offset : metadata.offsets) {
    encoder.text(offset.category);
    encoder.text(offset.name);
    encoder.signedNumber(offset.id);
    encoder.unsignedNumber(offset.permissions);
  }
}

void encodeViewport(PresentationKeyEncoder &encoder,
                    const ViewportSettings &viewport) {
  encodeEnum(encoder, viewport.mode);
  encodeEnum(encoder, viewport.customBase);
  encoder.floatingPoint(viewport.scaleX);
  encoder.floatingPoint(viewport.scaleY);
  encoder.floatingPoint(viewport.translateX);
  encoder.floatingPoint(viewport.translateY);
  encoder.boolean(viewport.centerPlayArea);
  encoder.boolean(viewport.keepHudFixed);
  encoder.floatingPoint(viewport.playAreaZoom);
}

void encodeSettings(PresentationKeyEncoder &encoder,
                    const EntryProfileSettings &settings) {
  encoder.unsignedNumber(settings.options.size());
  for (const auto &[name, value] : settings.options) {
    encoder.text(name);
    encoder.signedNumber(value);
  }

  encoder.unsignedNumber(settings.filePaths.size());
  for (const auto &[name, value] : settings.filePaths) {
    encoder.text(name);
    encoder.text(value);
  }

  encoder.unsignedNumber(settings.offsets.size());
  for (const auto &[name, value] : settings.offsets) {
    encoder.text(name);
    encoder.signedNumber(value.x);
    encoder.signedNumber(value.y);
    encoder.signedNumber(value.w);
    encoder.signedNumber(value.h);
    encoder.signedNumber(value.r);
    encoder.signedNumber(value.a);
  }
  encodeViewport(encoder, settings.viewport);
}

const char *progressPhaseLabel(SkinProgressPhase phase) {
  switch (phase) {
  case SkinProgressPhase::Inspecting:
    return i18n::tr("settings.skins.inspecting_skin_files.label");
  case SkinProgressPhase::Copying:
    return i18n::tr("settings.skins.copying_skin_files.label");
  case SkinProgressPhase::Validating:
    return i18n::tr("settings.skins.validating_skin.label");
  case SkinProgressPhase::Publishing:
    return i18n::tr("settings.skins.publishing_skin.label");
  }
  return i18n::tr("settings.skins.working_on_skin.label");
}

std::string formatProgressBytes(std::uint64_t bytes) {
  constexpr double kib = 1024.0;
  constexpr double mib = kib * 1024.0;
  constexpr double gib = mib * 1024.0;
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1);
  if (bytes >= static_cast<std::uint64_t>(gib)) {
    stream << static_cast<double>(bytes) / gib << " GB";
  } else if (bytes >= static_cast<std::uint64_t>(mib)) {
    stream << static_cast<double>(bytes) / mib << " MB";
  } else if (bytes >= static_cast<std::uint64_t>(kib)) {
    stream << static_cast<double>(bytes) / kib << " KB";
  } else {
    return std::to_string(bytes) + " B";
  }
  return std::move(stream).str();
}

} // namespace

std::vector<SkinTargetTrait> gameplaySkinSettingsTargets() {
  std::vector<SkinTargetTrait> targets(skinTargetTraits().begin(), skinTargetTraits().end());
  for (const auto &[keys, label] : {std::pair{4, "4K"}, {5, "5K"}, {6, "6K"}, {7, "7K"}, {8, "8K"}}) {
    targets.push_back({-keys, SkinTargetKind::Gameplay, keys, label});
  }
  const auto order = [](const SkinTargetTrait &target) {
    if (target.kind != SkinTargetKind::Gameplay) return std::tuple{100, target.skinType, 0};
    const int keys = target.keyMode == 10 ? 5 : target.keyMode == 14 ? 7
                   : target.keyMode == 48 ? 24 : target.keyMode;
    return std::tuple{keys, target.keyMode, target.skinType};
  };
  std::ranges::sort(targets, {}, order);
  return targets;
}

bool gameplaySkinEntrySelectableForTarget(
    const GameplaySkinEntryRow &entry,
    const SkinTargetTrait &target) noexcept {
  if (entry.validation != SkinValidationDisposition::SelectableGameplay ||
      entry.metadata.skinType != skinSourceTypeForTarget(target.skinType)) {
    return false;
  }
  if (target.kind != SkinTargetKind::MusicSelect) {
    return true;
  }
  return gameplaySkinSourceFormatForPath(entry.entry.packageRelativePath) ==
         GameplaySkinSourceFormat::Lua;
}

GameplaySkinSelection gameplaySkinSelectionForTarget(
    const GameplaySkinSettingsSnapshot &snapshot, const SkinTargetTrait &target) {
  GameplaySkinSelection result;
  for (const auto &candidate : snapshot.entries) {
    if (gameplaySkinEntrySelectableForTarget(candidate, target)) {
      result.entries.push_back(&candidate);
    }
  }
  result.followsOriginal = (target.skinType == -5 && snapshot.follow5K1S) ||
                           (target.skinType == -7 && snapshot.follow7K1S);
  const int effectiveTarget = result.followsOriginal
      ? skinSourceTypeForTarget(target.skinType)
      : target.skinType;
  const auto selected = snapshot.selectedSkinEntries.find(effectiveTarget);
  result.hasSelectedEntry = selected != snapshot.selectedSkinEntries.end();
  if (result.hasSelectedEntry) {
    const auto selectedCandidate = std::ranges::find_if(
        result.entries, [&selected](const auto *candidate) {
          return candidate->entry == selected->second;
        });
    if (selectedCandidate != result.entries.end()) {
      result.selectedRow = *selectedCandidate;
    }
  }

  return result;
}

std::optional<SkinTargetTrait> gameplaySkinSettingsTargetForKeyMode(
    const GameplaySkinSettingsSnapshot &snapshot, int keyMode) noexcept {
  auto target = gameplaySkinTargetForKeyMode(keyMode);
  if (target && ((keyMode == -5 && snapshot.follow5K1S) ||
                 (keyMode == -7 && snapshot.follow7K1S))) {
    target = skinTargetTraitForType(skinSourceTypeForTarget(target->skinType));
  }
  return target;
}

const GameplaySkinEntryRow *gameplaySkinSettingsEntryForKeyMode(
    const GameplaySkinSettingsSnapshot &snapshot, int keyMode) noexcept {
  const auto target = gameplaySkinSettingsTargetForKeyMode(snapshot, keyMode);
  if (!target || !snapshot.featureAvailable || !snapshot.compatibilityEnabled)
    return nullptr;
  const auto selected = snapshot.selectedSkinEntries.find(target->skinType);
  if (selected == snapshot.selectedSkinEntries.end()) return nullptr;
  const auto row = std::ranges::find_if(snapshot.entries, [&](const auto &entry) {
    return entry.entry == selected->second &&
           gameplaySkinEntrySelectableForTarget(entry, *target);
  });
  return row == snapshot.entries.end() ? nullptr : &*row;
}

std::string gameplaySkinPreviewConfigurationKey(
    const GameplaySkinSettingsSnapshot &snapshot, int keyMode) {
  PresentationKeyEncoder encoder;
  encoder.signedNumber(keyMode);
  encodeEnum(encoder, snapshot.safetyLevel);
  if (const auto *row = gameplaySkinSettingsEntryForKeyMode(snapshot, keyMode)) {
    encodeEntry(encoder, row->entry);
    encoder.text(row->revisionDigest);
    encoder.text(row->configurationDigest);
    encodeViewport(encoder, row->settings.viewport);
  }
  return std::move(encoder).finish();
}

std::vector<GameplaySkinCatalogItem>
gameplaySkinSettingsCatalogItems(const SkinEntryMetadataSnapshot &metadata) {
  std::vector<GameplaySkinCatalogItem> result;
  std::vector<bool> groupedOptions(metadata.options.size(), false);
  std::vector<bool> groupedFiles(metadata.files.size(), false);
  std::vector<bool> groupedOffsets(metadata.offsets.size(), false);

  const auto markGrouped = [&groupedOptions, &groupedFiles, &groupedOffsets](
                               const GameplaySkinCatalogItem &item) {
    switch (item.kind) {
    case GameplaySkinCatalogItemKind::Option:
      groupedOptions[item.declarationIndex] = true;
      break;
    case GameplaySkinCatalogItemKind::File:
      groupedFiles[item.declarationIndex] = true;
      break;
    case GameplaySkinCatalogItemKind::Offset:
      groupedOffsets[item.declarationIndex] = true;
      break;
    case GameplaySkinCatalogItemKind::CategoryHeading:
    case GameplaySkinCatalogItemKind::Separator:
      break;
    }
  };

  const auto resolveCategoryItem = [&metadata](std::string_view category) {
    std::optional<GameplaySkinCatalogItem> resolved;
    // JSONSkinLoader reuses one CustomItem[] slot per category.item entry.
    // Its option, file, and offset passes each assign that same slot, so the
    // last matching declaration in the last pass wins.
    for (std::size_t index = 0; index < metadata.options.size(); ++index) {
      if (metadata.options[index].category == category) {
        resolved = {.kind = GameplaySkinCatalogItemKind::Option,
                    .declarationIndex = index};
      }
    }
    for (std::size_t index = 0; index < metadata.files.size(); ++index) {
      if (metadata.files[index].category == category) {
        resolved = {.kind = GameplaySkinCatalogItemKind::File,
                    .declarationIndex = index};
      }
    }
    for (std::size_t index = 0; index < metadata.offsets.size(); ++index) {
      if (metadata.offsets[index].category == category) {
        resolved = {.kind = GameplaySkinCatalogItemKind::Offset,
                    .declarationIndex = index};
      }
    }
    return resolved;
  };

  // SkinConfigurationView emits categories in their declared order. Do not
  // infer hierarchy from labels or declaration names.
  for (const auto &category : metadata.categories) {
    result.push_back({.kind = GameplaySkinCatalogItemKind::CategoryHeading,
                      .label = category.name});
    for (const auto &item : category.items) {
      if (const auto resolved = resolveCategoryItem(item)) {
        result.push_back(*resolved);
        markGrouped(*resolved);
      }
    }
    result.push_back({.kind = GameplaySkinCatalogItemKind::Separator});
  }

  const auto hasUngrouped = [](const std::vector<bool> &grouped) {
    return std::ranges::any_of(grouped, [](bool value) { return !value; });
  };
  if (!metadata.categories.empty() &&
      (hasUngrouped(groupedOptions) || hasUngrouped(groupedFiles) ||
       hasUngrouped(groupedOffsets))) {
    result.push_back(
        {.kind = GameplaySkinCatalogItemKind::CategoryHeading, .label = i18n::tr("settings.skins.other.label")});
  }

  const auto appendUngrouped =
      [&result](const auto &declarations, const std::vector<bool> &grouped,
                GameplaySkinCatalogItemKind kind) {
        for (std::size_t index = 0; index < declarations.size(); ++index) {
          if (!grouped[index]) {
            result.push_back({.kind = kind, .declarationIndex = index});
          }
        }
      };
  appendUngrouped(metadata.options, groupedOptions,
                  GameplaySkinCatalogItemKind::Option);
  appendUngrouped(metadata.files, groupedFiles,
                  GameplaySkinCatalogItemKind::File);
  appendUngrouped(metadata.offsets, groupedOffsets,
                  GameplaySkinCatalogItemKind::Offset);
  return result;
}

std::vector<const GameplaySkinEntryRow *>
gameplaySkinManagementEntries(const GameplaySkinSettingsSnapshot &snapshot) {
  std::vector<const GameplaySkinEntryRow *> result;
  for (const auto &entry : snapshot.entries) {
    if (entry.validation != SkinValidationDisposition::SelectableGameplay) {
      result.push_back(&entry);
    }
  }
  return result;
}

GameplaySkinSettingsActionAvailability gameplaySkinSettingsActionAvailability(
    const GameplaySkinSettingsSnapshot &snapshot) noexcept {
  const bool nameReady = snapshot.state == GameplaySkinSettingsState::Ready &&
                         snapshot.preparedName.has_value();
  return {
      .ordinaryActions = snapshot.state != GameplaySkinSettingsState::Busy &&
                         !nameReady,
      .canCancel = snapshot.canCancel,
      .canEditPreparedName = false,
      .canInstallPrepared = nameReady && snapshot.preparedName->ok(),
  };
}

bool gameplaySkinTraitsRuntimeAvailable(bool luaFeatureAvailable,
                                        bool controllerAvailable) noexcept {
  return luaFeatureAvailable && controllerAvailable;
}

std::string gameplaySkinSettingsPresentationKey(
    const GameplaySkinSettingsSnapshot &snapshot) {
  if (!snapshot.cachedPresentationKey.empty()) {
    return snapshot.cachedPresentationKey;
  }
  PresentationKeyEncoder encoder;
  encodeEnum(encoder, snapshot.state);
  encoder.boolean(snapshot.featureAvailable);
  encoder.boolean(snapshot.compatibilityEnabled);
  encodeEnum(encoder, snapshot.safetyLevel);
  encoder.boolean(snapshot.pendingSafetyLevel.has_value());
  if (snapshot.pendingSafetyLevel) {
    encodeEnum(encoder, *snapshot.pendingSafetyLevel);
  }
  encoder.text(snapshot.statusMessage);
  encoder.boolean(snapshot.canCancel);
  encoder.boolean(snapshot.hasPackageProgress);

  encodeEnum(encoder, snapshot.progress.phase);
  encoder.unsignedNumber(snapshot.progress.completedBytes);
  encoder.unsignedNumber(snapshot.progress.totalBytes);
  encoder.unsignedNumber(snapshot.progress.completedFiles);
  encodeEnum(encoder, snapshot.rescanProgress.phase);
  encodeEnum(encoder, snapshot.rescanProgress.packageProgress.phase);
  encoder.unsignedNumber(snapshot.rescanProgress.packageProgress.completedBytes);
  encoder.unsignedNumber(snapshot.rescanProgress.packageProgress.totalBytes);
  encoder.unsignedNumber(snapshot.rescanProgress.packageProgress.completedFiles);

  encoder.unsignedNumber(snapshot.follow5K1S);
  encoder.unsignedNumber(snapshot.follow7K1S);
  encoder.unsignedNumber(snapshot.selectedSkinEntries.size());
  for (const auto &[skinType, entry] : snapshot.selectedSkinEntries) {
    encoder.signedNumber(skinType);
    encodeEntry(encoder, entry);
  }

  encoder.boolean(snapshot.preparedName.has_value());
  if (snapshot.preparedName) {
    encoder.text(snapshot.preparedName->originalSourceName);
    encoder.text(snapshot.preparedName->suggestedPackageName);
    encoder.text(snapshot.preparedName->validationError);
  }

  encoder.boolean(snapshot.collisionPackage.has_value());
  if (snapshot.collisionPackage) {
    encodePackage(encoder, *snapshot.collisionPackage);
  }

  encoder.unsignedNumber(snapshot.entries.size());
  for (const auto &row : snapshot.entries) {
    encodeEntry(encoder, row.entry);
    encodeMetadata(encoder, row.metadata);
    encoder.text(row.revisionDigest);
    encoder.text(row.configurationDigest);
    encodeEnum(encoder, row.validation);
    encodeSettings(encoder, row.settings);
    encoder.unsignedNumber(row.diagnostics.size());
    for (const auto &diagnostic : row.diagnostics) {
      encodeDiagnostic(encoder, diagnostic);
    }
  }

  encoder.unsignedNumber(snapshot.history.size());
  for (const auto &record : snapshot.history) {
    encoder.unsignedNumber(record.recordSerial);
    encodeEntry(encoder, record.entry);
    encoder.text(record.revisionDigest);
    encoder.text(record.configurationDigest);
    encodeEnum(encoder, record.phase);
    encodeDiagnostic(encoder, record.diagnostic);
    encoder.boolean(record.luaLine.has_value());
    if (record.luaLine) {
      encoder.unsignedNumber(*record.luaLine);
    }
    encoder.boolean(record.frameSerial.has_value());
    if (record.frameSerial) {
      encoder.unsignedNumber(*record.frameSerial);
    }
  }
  return std::move(encoder).finish();
}

std::string gameplaySkinSettingsLayoutKey(
    const GameplaySkinSettingsSnapshot &snapshot) {
  PresentationKeyEncoder encoder;
  encoder.boolean(snapshot.featureAvailable);
  encoder.boolean(snapshot.state == GameplaySkinSettingsState::Error);
  encoder.boolean(snapshot.compatibilityEnabled);
  encodeEnum(encoder, snapshot.safetyLevel);
  encoder.boolean(snapshot.pendingSafetyLevel.has_value());
  if (snapshot.pendingSafetyLevel) {
    encodeEnum(encoder, *snapshot.pendingSafetyLevel);
  }

  encoder.unsignedNumber(snapshot.follow5K1S);
  encoder.unsignedNumber(snapshot.follow7K1S);
  encoder.unsignedNumber(snapshot.selectedSkinEntries.size());
  for (const auto &[skinType, entry] : snapshot.selectedSkinEntries) {
    encoder.signedNumber(skinType);
    encodeEntry(encoder, entry);
  }

  encoder.boolean(snapshot.preparedName.has_value());
  if (snapshot.preparedName) {
    encoder.text(snapshot.preparedName->originalSourceName);
    encoder.text(snapshot.preparedName->suggestedPackageName);
    encoder.text(snapshot.preparedName->validationError);
  }

  encoder.boolean(snapshot.collisionPackage.has_value());
  if (snapshot.collisionPackage) {
    encodePackage(encoder, *snapshot.collisionPackage);
  }

  encoder.unsignedNumber(snapshot.entries.size());
  for (const auto &row : snapshot.entries) {
    encodeEntry(encoder, row.entry);
    encodeMetadata(encoder, row.metadata);
    encoder.text(row.revisionDigest);
    encodeEnum(encoder, row.validation);
    encoder.unsignedNumber(row.diagnostics.size());
    for (const auto &diagnostic : row.diagnostics) {
      encodeDiagnostic(encoder, diagnostic);
    }
  }

  encoder.unsignedNumber(snapshot.history.size());
  for (const auto &record : snapshot.history) {
    encoder.unsignedNumber(record.recordSerial);
    encodeEntry(encoder, record.entry);
    encoder.text(record.revisionDigest);
    encoder.text(record.configurationDigest);
    encodeEnum(encoder, record.phase);
    encodeDiagnostic(encoder, record.diagnostic);
    encoder.boolean(record.luaLine.has_value());
    if (record.luaLine) {
      encoder.unsignedNumber(*record.luaLine);
    }
    encoder.boolean(record.frameSerial.has_value());
    if (record.frameSerial) {
      encoder.unsignedNumber(*record.frameSerial);
    }
  }
  return std::move(encoder).finish();
}

std::string gameplaySkinPackageProgressDisplayText(const SkinProgress &progress) {
  std::string text = progressPhaseLabel(progress.phase);
  if (progress.totalBytes > 0) {
    const auto percent = static_cast<int>(std::lround(
        std::clamp(static_cast<double>(progress.completedBytes) /
                       static_cast<double>(progress.totalBytes),
                   0.0, 1.0) *
        100.0));
    text += " — " + std::to_string(percent) + "% (" +
            formatProgressBytes(progress.completedBytes) + " / " +
            formatProgressBytes(progress.totalBytes) + ")";
  }
  if (progress.completedFiles > 0) {
    text += " • " + std::to_string(progress.completedFiles) +
            (progress.completedFiles == 1 ? " file" : " files");
  }
  return text;
}

std::string
gameplaySkinRescanProgressDisplayText(const SkinRescanProgress &progress) {
  switch (progress.phase) {
  case SkinRescanProgressPhase::Idle:
    return {};
  case SkinRescanProgressPhase::LoadingProfileInventory:
    return i18n::tr("settings.skins.loading_skin_profile_inventory.label");
  case SkinRescanProgressPhase::ReconcilingActivations:
    return i18n::tr("settings.skins.reconciling_skin_activations.label");
  case SkinRescanProgressPhase::ScanningVisiblePackages:
    return gameplaySkinPackageProgressDisplayText(progress.packageProgress);
  case SkinRescanProgressPhase::Succeeded:
    return i18n::tr("settings.skins.skin_scan_complete.message");
  case SkinRescanProgressPhase::Failed:
    return i18n::tr("settings.skins.skin_scan_did_not_complete.message");
  }
  return i18n::tr("settings.skins.scanning_skin_packages.label");
}

ViewportSettings gameplaySkinViewportWithMode(ViewportSettings current,
                                              ViewportMode mode) noexcept {
  current.mode = mode;
  return current;
}

ViewportSettings
gameplaySkinViewportWithCustomBase(ViewportSettings current,
                                   CustomViewportBase customBase) noexcept {
  current.mode = ViewportMode::Custom;
  current.customBase = customBase;
  return current;
}

int gameplaySkinSanitizedOffsetComponent(std::string_view text,
                                         int fallback) noexcept {
  int value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? value
             : fallback;
}

float gameplaySkinSanitizedViewportComponent(std::string_view text,
                                             float fallback, float minimum,
                                             float maximum) {
  try {
    std::size_t consumed = 0;
    const float value = std::stof(std::string(text), &consumed);
    if (consumed != text.size() || !std::isfinite(value)) {
      return fallback;
    }
    return std::clamp(value, minimum, maximum);
  } catch (...) {
    return fallback;
  }
}

} // namespace skin
