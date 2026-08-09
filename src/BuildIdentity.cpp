#include "BuildIdentity.h"

#include <string_view>

#if __has_include("BuildIdentityConfig.h")
#include "BuildIdentityConfig.h"
#endif

#ifndef ASOBMASHOW_BUILD_COMMIT
#error "ASOBMASHOW_BUILD_COMMIT must be supplied by the build system"
#endif

#ifndef ASOBMASHOW_BUILD_CONFIGURATION
#error "ASOBMASHOW_BUILD_CONFIGURATION must be supplied by the build system"
#endif

#ifndef ASOBMASHOW_SOURCE_CLEAN
#error "ASOBMASHOW_SOURCE_CLEAN must be supplied by the build system"
#endif

static_assert(ASOBMASHOW_SOURCE_CLEAN == 0 || ASOBMASHOW_SOURCE_CLEAN == 1,
              "ASOBMASHOW_SOURCE_CLEAN must be 0 or 1");

#define ASOBMASHOW_STRINGIFY_INNER(value) #value
#define ASOBMASHOW_STRINGIFY(value) ASOBMASHOW_STRINGIFY_INNER(value)

#if defined(__APPLE__) && defined(__clang__)
#if __has_attribute(retain)
#define ASOBMASHOW_RETAIN_IDENTITY __attribute__((used, retain))
#else
#define ASOBMASHOW_RETAIN_IDENTITY __attribute__((used))
#endif
#else
#define ASOBMASHOW_RETAIN_IDENTITY
#endif

namespace skin {
namespace {

constexpr char kIdentityPrefix[] = "AsoBMaShowBuildIdentityV1|";
ASOBMASHOW_RETAIN_IDENTITY constexpr char kCompiledIdentity[] =
    "AsoBMaShowBuildIdentityV1|" ASOBMASHOW_BUILD_COMMIT "|"
    ASOBMASHOW_BUILD_CONFIGURATION "|"
    ASOBMASHOW_STRINGIFY(ASOBMASHOW_SOURCE_CLEAN);

bool isLowerHexCommit(const std::string_view value) noexcept {
  if (value.size() != 40) {
    return false;
  }
  bool hasNonzeroDigit = false;
  for (const char character : value) {
    const bool decimal = character >= '0' && character <= '9';
    const bool lowerHex = character >= 'a' && character <= 'f';
    if (!decimal && !lowerHex) {
      return false;
    }
    hasNonzeroDigit = hasNonzeroDigit || character != '0';
  }
  return hasNonzeroDigit;
}

bool isPlaceholderConfiguration(const std::string_view value) noexcept {
  return value.empty() || value == "unknown" || value == "Unknown" ||
         value == "placeholder" || value == "Placeholder" ||
         value == "dirty" || value == "Dirty";
}

} // namespace

bool SkinBuildIdentity::validForAcceptance() const noexcept {
  return cleanSource && isLowerHexCommit(commit) &&
         !isPlaceholderConfiguration(configuration);
}

SkinBuildIdentity compiledSkinBuildIdentity() {
  const std::string_view marker{kCompiledIdentity};
  constexpr std::size_t prefixLength = sizeof(kIdentityPrefix) - 1;
  const std::size_t commitEnd = marker.find('|', prefixLength);
  const std::size_t configurationEnd = marker.find('|', commitEnd + 1);
  if (commitEnd == std::string_view::npos ||
      configurationEnd == std::string_view::npos) {
    return {};
  }
  return {
      std::string{marker.substr(prefixLength, commitEnd - prefixLength)},
      std::string{marker.substr(commitEnd + 1,
                                configurationEnd - commitEnd - 1)},
      marker.substr(configurationEnd + 1) == "1",
  };
}

} // namespace skin

#undef ASOBMASHOW_STRINGIFY
#undef ASOBMASHOW_STRINGIFY_INNER
#undef ASOBMASHOW_RETAIN_IDENTITY
