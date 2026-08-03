#pragma once

#include "SkinPackageTypes.h"

#include <string>
#include <string_view>

namespace skin {

// Normalizes a single virtual filename component. It never accepts a host or
// package-relative path, and its result is valid NFC UTF-8.
SkinUtf8NfcResult normalizeSkinSourceNameNfc(std::string_view sourceName);

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
