#include "SkinPathPolicy.h"

#include <utf8proc.h>

#include <cstdlib>
#include <string>
#include <string_view>

namespace skin {
namespace {

constexpr std::string_view kEmptyError = "skin path is empty";
constexpr std::string_view kNulError = "skin path contains NUL";
constexpr std::string_view kUtf8Error = "skin path is not valid UTF-8";
constexpr std::string_view kComponentError = "skin path has an unsafe component";
constexpr std::string_view kSeparatorError = "skin path has an unsafe separator";
constexpr std::string_view kAbsoluteError = "skin path must be relative";
constexpr std::string_view kLengthError = "skin path exceeds its byte limit";
constexpr std::string_view kDepthError = "skin path exceeds its component limit";

bool containsNul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

bool isAsciiDrivePath(std::string_view value) {
  return value.size() >= 2 &&
         ((value[0] >= 'A' && value[0] <= 'Z') ||
          (value[0] >= 'a' && value[0] <= 'z')) &&
         value[1] == ':';
}

std::optional<std::string> mapUtf8(std::string_view value,
                                   utf8proc_option_t options) {
  utf8proc_uint8_t *mapped = nullptr;
  const auto size = utf8proc_map(
      reinterpret_cast<const utf8proc_uint8_t *>(value.data()),
      static_cast<utf8proc_ssize_t>(value.size()), &mapped, options);
  if (size < 0 || mapped == nullptr) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<const char *>(mapped),
                     static_cast<std::size_t>(size));
  std::free(mapped);
  return result;
}

SkinUtf8NfcResult normalizeNfc(std::string_view value) {
  if (value.empty()) {
    return {.error = std::string(kEmptyError)};
  }
  if (containsNul(value)) {
    return {.error = std::string(kNulError)};
  }

  utf8proc_ssize_t offset = 0;
  while (offset < static_cast<utf8proc_ssize_t>(value.size())) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(value.data()) + offset,
        static_cast<utf8proc_ssize_t>(value.size()) - offset, &codepoint);
    if (consumed <= 0) {
      return {.error = std::string(kUtf8Error)};
    }
    offset += consumed;
  }

  auto normalized = mapUtf8(
      value, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE));
  if (!normalized) {
    return {.error = std::string(kUtf8Error)};
  }
  return {.value = std::move(*normalized)};
}

std::optional<std::string> collisionKeyFor(std::string_view nfc) {
  return mapUtf8(
      nfc, static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE |
                                           UTF8PROC_CASEFOLD));
}

bool isUnsafeComponent(std::string_view component) {
  return component.empty() || component == "." || component == "..";
}

SkinEntryIdResult invalidEntry(std::string_view error) {
  return {.error = std::string(error)};
}

} // namespace

SkinUtf8NfcResult normalizeSkinSourceNameNfc(std::string_view sourceName) {
  auto normalized = normalizeNfc(sourceName);
  if (!normalized.value) {
    return normalized;
  }
  if (normalized.value->find('/') != std::string::npos ||
      normalized.value->find('\\') != std::string::npos) {
    return {.error = std::string(kSeparatorError)};
  }
  if (isUnsafeComponent(*normalized.value)) {
    return {.error = std::string(kComponentError)};
  }
  return normalized;
}

SkinPackageIdResult normalizePackageId(std::string_view directoryName) {
  auto normalized = normalizeSkinSourceNameNfc(directoryName);
  if (!normalized.value) {
    return {.error = std::move(normalized.error)};
  }
  if (normalized.value->size() > SkinPackagePolicy::maxPackageNameBytes) {
    return {.error = std::string(kLengthError)};
  }
  if (isAsciiDrivePath(*normalized.value)) {
    return {.error = std::string(kAbsoluteError)};
  }

  auto collisionKey = collisionKeyFor(*normalized.value);
  if (!collisionKey) {
    return {.error = std::string(kUtf8Error)};
  }
  return {.package = SkinPackageId{.directoryName = std::move(*normalized.value),
                                   .collisionKey = std::move(*collisionKey)}};
}

SkinEntryIdResult normalizeEntryPath(const SkinPackageId &package,
                                     std::string_view packageRelativePath) {
  const auto normalizedPackage = normalizePackageId(package.directoryName);
  if (!normalizedPackage.package ||
      normalizedPackage.package->collisionKey != package.collisionKey) {
    return invalidEntry(kComponentError);
  }

  auto normalized = normalizeNfc(packageRelativePath);
  if (!normalized.value) {
    return {.error = std::move(normalized.error)};
  }
  const std::string &path = *normalized.value;
  if (path.front() == '/' || path.find('\\') != std::string::npos ||
      isAsciiDrivePath(path)) {
    return invalidEntry(kAbsoluteError);
  }
  if (path.size() > SkinPackagePolicy::maxPathBytes) {
    return invalidEntry(kLengthError);
  }

  std::uint32_t components = 0;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = path.find('/', start);
    const std::string_view component(
        path.data() + start,
        (end == std::string::npos ? path.size() : end) - start);
    if (isUnsafeComponent(component)) {
      return invalidEntry(kSeparatorError);
    }
    ++components;
    if (components > SkinPackagePolicy::maxPathComponents) {
      return invalidEntry(kDepthError);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  auto pathCollisionKey = collisionKeyFor(path);
  if (!pathCollisionKey) {
    return invalidEntry(kUtf8Error);
  }
  return {.entry = SkinEntryId{
              .package = *normalizedPackage.package,
              .packageRelativePath = path,
              .collisionKey = normalizedPackage.package->collisionKey + "/" +
                              *pathCollisionKey,
          }};
}

std::string installedRelativePath(const SkinEntryId &entry) {
  const auto package = normalizePackageId(entry.package.directoryName);
  if (!package.package || package.package->collisionKey != entry.package.collisionKey) {
    return {};
  }
  const auto normalizedEntry =
      normalizeEntryPath(*package.package, entry.packageRelativePath);
  if (!normalizedEntry.entry ||
      normalizedEntry.entry->collisionKey != entry.collisionKey) {
    return {};
  }
  return normalizedEntry.entry->package.directoryName + "/" +
         normalizedEntry.entry->packageRelativePath;
}

} // namespace skin
