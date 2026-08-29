#include "SkinProfileSettings.h"

#include "../FileChecksum.h"
#include "SkinTargetTraits.h"
#include "package/SkinPathPolicy.h"

#include <utf8proc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>

namespace skin {
namespace {

void appendDigestU32(std::string &bytes, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

void appendDigestI32(std::string &bytes, int value) {
  appendDigestU32(bytes,
                  static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
}

void appendDigestText(std::string &bytes, std::string_view value) {
  appendDigestU32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.append(value);
}

std::optional<std::string> normalizeUtf8(std::string_view value) {
  if (value.empty() || value.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  utf8proc_uint8_t *mapped = nullptr;
  const auto size = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t *>(value.data()),
      static_cast<utf8proc_ssize_t>(value.size()), &mapped,
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
  if (size < 0 || mapped == nullptr) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char *>(mapped),
                     static_cast<std::size_t>(size));
  std::free(mapped);
  return result;
}

std::optional<std::string> configurationKey(std::string_view value) {
  auto normalized = normalizeUtf8(value);
  if (!normalized) {
    return std::nullopt;
  }
  return normalized;
}

bool isDrivePath(std::string_view value) {
  return value.size() >= 2 &&
         ((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= 'A' && value[0] <= 'Z')) &&
         value[1] == ':';
}

std::optional<std::string> virtualFileValue(std::string_view value) {
  auto normalized = normalizeUtf8(value);
  if (!normalized || normalized->front() == '/' ||
      normalized->find('\\') != std::string::npos || isDrivePath(*normalized)) {
    return std::nullopt;
  }
  std::size_t start = 0;
  while (start <= normalized->size()) {
    const auto end = normalized->find('/', start);
    const auto component = normalized->substr(
        start, (end == std::string::npos ? normalized->size() : end) - start);
    if (component.empty() || component == "." || component == "..") {
      return std::nullopt;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return normalized;
}

template <typename Value, typename NormalizeValue>
void sanitizeMap(std::map<std::string, Value> &values,
                 NormalizeValue normalizeValue) {
  std::map<std::string, Value> sanitized;
  for (const auto &[rawKey, rawValue] : values) {
    auto key = configurationKey(rawKey);
    auto value = normalizeValue(rawValue);
    if (!key || !value) {
      continue;
    }
    sanitized.try_emplace(std::move(*key), std::move(*value));
  }
  values = std::move(sanitized);
}

void sanitizeViewport(ViewportSettings &viewport) {
  const bool validMode = viewport.mode == ViewportMode::Fit ||
                         viewport.mode == ViewportMode::Stretch ||
                         viewport.mode == ViewportMode::Custom;
  const bool validBase = viewport.customBase == CustomViewportBase::Fit ||
                         viewport.customBase == CustomViewportBase::Stretch;
  const bool validNumbers = std::isfinite(viewport.scaleX) &&
                            std::isfinite(viewport.scaleY) &&
                            std::isfinite(viewport.translateX) &&
                            std::isfinite(viewport.translateY) &&
                            viewport.scaleX > 0.0F && viewport.scaleY > 0.0F;
  if (!validMode || !validBase || !validNumbers) {
    viewport = {};
    return;
  }
  viewport.scaleX =
      std::clamp(viewport.scaleX, SkinProfileSettingsPolicy::minCustomScale,
                 SkinProfileSettingsPolicy::maxCustomScale);
  viewport.scaleY =
      std::clamp(viewport.scaleY, SkinProfileSettingsPolicy::minCustomScale,
                 SkinProfileSettingsPolicy::maxCustomScale);
  viewport.translateX = std::clamp(
      viewport.translateX, SkinProfileSettingsPolicy::minCustomTranslation,
      SkinProfileSettingsPolicy::maxCustomTranslation);
  viewport.translateY = std::clamp(
      viewport.translateY, SkinProfileSettingsPolicy::minCustomTranslation,
      SkinProfileSettingsPolicy::maxCustomTranslation);
}

void sanitizeEntry(EntryProfileSettings &entry) {
  sanitizeMap(entry.options,
              [](int value) { return std::optional<int>(value); });
  sanitizeMap(entry.filePaths,
              [](const std::string &value) { return virtualFileValue(value); });
  sanitizeMap(entry.offsets,
              [](ConfigOffset value) { return std::optional<ConfigOffset>(value); });
  sanitizeViewport(entry.viewport);
}

} // namespace

std::string skinConfigurationDigest(const EntryProfileSettings &settings) {
  std::string framed("ASOBMSKIN-CONFIG-V1", 19);
  framed.push_back('\0');

  framed.push_back(static_cast<char>(0x01));
  appendDigestU32(framed, static_cast<std::uint32_t>(settings.options.size()));
  for (const auto &[key, value] : settings.options) {
    appendDigestText(framed, key);
    appendDigestI32(framed, value);
  }

  framed.push_back(static_cast<char>(0x02));
  appendDigestU32(framed,
                  static_cast<std::uint32_t>(settings.filePaths.size()));
  for (const auto &[key, value] : settings.filePaths) {
    appendDigestText(framed, key);
    appendDigestText(framed, value);
  }

  framed.push_back(static_cast<char>(0x03));
  appendDigestU32(framed, static_cast<std::uint32_t>(settings.offsets.size()));
  for (const auto &[key, value] : settings.offsets) {
    appendDigestText(framed, key);
    appendDigestI32(framed, value.x);
    appendDigestI32(framed, value.y);
    appendDigestI32(framed, value.w);
    appendDigestI32(framed, value.h);
    appendDigestI32(framed, value.r);
    appendDigestI32(framed, value.a);
  }
  return file_checksum::sha256(framed);
}

std::optional<std::string>
normalizeSkinConfigurationKey(std::string_view value) {
  return configurationKey(value);
}

std::optional<SkinProfileId>
makeSkinProfileId(std::string_view existingPlayerProfileId) {
  if (existingPlayerProfileId.size() != 36) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < existingPlayerProfileId.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (existingPlayerProfileId[index] != '-') {
        return std::nullopt;
      }
      continue;
    }
    const unsigned char character =
        static_cast<unsigned char>(existingPlayerProfileId[index]);
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f') ||
          (character >= 'A' && character <= 'F'))) {
      return std::nullopt;
    }
  }
  return SkinProfileId{.opaque = std::string(existingPlayerProfileId)};
}

void SkinProfileSettings::sanitize() {
  if (safetyLevel != SkinSafetyLevel::Standard &&
      safetyLevel != SkinSafetyLevel::BeatorajaCompatibility &&
      safetyLevel != SkinSafetyLevel::Unrestricted) {
    safetyLevel = SkinSafetyLevel::Standard;
  }
  // A legacy profile represented one optional 7K selection plus a global
  // enable bit. Preserve its disabled state by only migrating when it was
  // enabled. Once a new-format map is present, it is wholly authoritative.
  const bool hasAuthoritativeSelections = !selectedSkinEntries.empty();
  if (!hasAuthoritativeSelections && gameplayCompatibilityEnabled &&
      selected7KeyEntry) {
    selectedSkinEntries.try_emplace(0, *selected7KeyEntry);
  }
  for (const auto &[skinType, entry] : selectedGameplayEntries) {
    selectedSkinEntries.try_emplace(skinType, entry);
  }

  std::map<int, std::string> selectedCollisionKeys;
  for (const auto &[skinType, selectedEntry] : selectedSkinEntries) {
    if (!skinTargetTraitForType(skinType)) {
      continue;
    }
    const auto selectedPackage =
        normalizePackageId(selectedEntry.package.directoryName);
    const auto normalizedEntry =
        selectedPackage.package
            ? normalizeEntryPath(*selectedPackage.package,
                                 selectedEntry.packageRelativePath)
            : SkinEntryIdResult{};
    if (normalizedEntry.entry &&
        normalizedEntry.entry->collisionKey == selectedEntry.collisionKey) {
      selectedCollisionKeys.try_emplace(skinType,
                                        normalizedEntry.entry->collisionKey);
    }
  }

  std::map<SkinEntryId, EntryProfileSettings> sanitized;
  std::set<std::string, std::less<>> retainedCollisionKeys;
  for (auto &[rawEntry, settings] : entries) {
    const auto package = normalizePackageId(rawEntry.package.directoryName);
    if (!package.package ||
        package.package->collisionKey != rawEntry.package.collisionKey) {
      continue;
    }
    const auto entry =
        normalizeEntryPath(*package.package, rawEntry.packageRelativePath);
    if (!entry.entry || entry.entry->collisionKey != rawEntry.collisionKey) {
      continue;
    }
    if (retainedCollisionKeys.contains(entry.entry->collisionKey)) {
      continue;
    }
    sanitizeEntry(settings);
    retainedCollisionKeys.insert(entry.entry->collisionKey);
    sanitized.try_emplace(*entry.entry, std::move(settings));
  }
  entries = std::move(sanitized);

  selectedSkinEntries.clear();
  for (const auto &[skinType, selectedCollisionKey] : selectedCollisionKeys) {
    for (const auto &[entry, settings] : entries) {
      (void)settings;
      if (entry.collisionKey == selectedCollisionKey) {
        selectedSkinEntries.try_emplace(skinType, entry);
        break;
      }
    }
  }

  selected7KeyEntry.reset();
  if (const auto legacySelection = selectedSkinEntries.find(0);
      legacySelection != selectedSkinEntries.end()) {
    selected7KeyEntry = legacySelection->second;
  }
  gameplayCompatibilityEnabled = !selectedSkinEntries.empty();
  selectedGameplayEntries.clear();
  for (const auto &[skinType, entry] : selectedSkinEntries) {
    if (const auto trait = skinTargetTraitForType(skinType);
        trait && trait->kind == SkinTargetKind::Gameplay) {
      selectedGameplayEntries.try_emplace(skinType, entry);
    }
  }
}

} // namespace skin
