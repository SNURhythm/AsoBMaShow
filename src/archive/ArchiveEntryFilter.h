#pragma once
#include <filesystem>

namespace archive_file {
// The same system-file exclusion used when building browsing indexes.
bool isSystemEntry(const std::filesystem::path &);
}
