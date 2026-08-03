#include "SkinProfileSettings.h"

#include "package/SkinPathPolicy.h"

#include <utf8proc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>
#include <utility>

namespace skin {
namespace {

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
  if (!normalized || normalized->size() >
                         SkinProfileSettingsPolicy::maxConfigurationKeyBytes) {
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
  if (!normalized ||
      normalized->size() >
          SkinProfileSettingsPolicy::maxConfigurationValueBytes ||
      normalized->front() == '/' ||
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
void sanitizeMap(std::map<std::string, Value> &values, std::size_t limit,
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
  while (sanitized.size() > limit) {
    sanitized.erase(std::prev(sanitized.end()));
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
  sanitizeMap(entry.options, SkinProfileSettingsPolicy::maxOptionsPerEntry,
              [](int value) { return std::optional<int>(value); });
  sanitizeMap(entry.filePaths, SkinProfileSettingsPolicy::maxFilesPerEntry,
              [](const std::string &value) { return virtualFileValue(value); });
  sanitizeMap(
      entry.offsets, SkinProfileSettingsPolicy::maxOffsetsPerEntry,
      [](ConfigOffset value) {
        value.x =
            std::clamp(value.x, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        value.y =
            std::clamp(value.y, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        value.w =
            std::clamp(value.w, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        value.h =
            std::clamp(value.h, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        value.r =
            std::clamp(value.r, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        value.a =
            std::clamp(value.a, SkinProfileSettingsPolicy::minOffsetComponent,
                       SkinProfileSettingsPolicy::maxOffsetComponent);
        return std::optional<ConfigOffset>(value);
      });
  sanitizeViewport(entry.viewport);
}

} // namespace

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
  std::optional<std::string> selectedCollisionKey;
  if (selected7KeyEntry) {
    const auto selectedPackage =
        normalizePackageId(selected7KeyEntry->package.directoryName);
    const auto selectedEntry =
        selectedPackage.package
            ? normalizeEntryPath(*selectedPackage.package,
                                 selected7KeyEntry->packageRelativePath)
            : SkinEntryIdResult{};
    if (selectedEntry.entry &&
        selectedEntry.entry->collisionKey == selected7KeyEntry->collisionKey) {
      selectedCollisionKey = selectedEntry.entry->collisionKey;
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
    if (sanitized.size() == SkinProfileSettingsPolicy::maxEntries) {
      break;
    }
  }
  entries = std::move(sanitized);

  selected7KeyEntry.reset();
  if (selectedCollisionKey) {
    for (const auto &[entry, settings] : entries) {
      (void)settings;
      if (entry.collisionKey == *selectedCollisionKey) {
        selected7KeyEntry = entry;
        break;
      }
    }
  }
  if (!selected7KeyEntry) {
    gameplayCompatibilityEnabled = false;
  }
}

} // namespace skin
