#pragma once

#include "../../targets.h"
#include "SkinPackageTypes.h"

#if !defined(_WIN32)
#include <fcntl.h>
#endif

#include <string>
#include <string_view>

namespace skin {

// iOS Documents storage is intentionally user-editable through Files. Its
// package services must therefore avoid Darwin's no-follow open flag; other
// supported POSIX targets retain it.
constexpr int skinOpenNoFollowFlag() noexcept {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR || defined(_WIN32)
  return 0;
#else
  return O_NOFOLLOW;
#endif
}

// Normalizes a single virtual filename component. It never accepts a host or
// package-relative path, and its result is valid NFC UTF-8.
SkinUtf8NfcResult normalizeSkinSourceNameNfc(std::string_view sourceName);

// Returns the full Unicode case-folded collision key for an already-NFC
// virtual path. Invalid UTF-8 or non-canonical input fails closed.
std::optional<std::string>
skinPathCollisionKey(std::string_view nfcVirtualPath);

// A package is exactly one direct child of the Skins root.
SkinPackageIdResult normalizePackageId(std::string_view directoryName);

// Entries are lexical virtual paths. Filesystem canonical/no-follow
// containment is intentionally performed by the package opener, never by
// turning a host path back into this public identity.
SkinEntryIdResult normalizeEntryPath(const SkinPackageId &package,
                                     std::string_view packageRelativePath);

// Returns a virtual path only. Invalid manually-constructed IDs return empty
// rather than permitting a host path or an unsafe component to escape.
std::string installedRelativePath(const SkinEntryId &entry);

} // namespace skin
