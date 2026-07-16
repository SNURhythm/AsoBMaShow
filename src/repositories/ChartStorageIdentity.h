#pragma once

#include "../path.h"

#include <filesystem>
#include <string>

namespace chart_storage_identity {

std::string StoredPathText(std::filesystem::path path);
void ToRelativePath(std::filesystem::path &path);
void ToAbsolutePath(std::filesystem::path &path);
void ConfigureArchiveCachePathNormalization();

} // namespace chart_storage_identity
