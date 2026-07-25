#pragma once

#include "../path.h"

#include <filesystem>
#include <optional>
#include <string>

namespace chart_storage_identity {

std::optional<std::filesystem::path> RebaseLegacyIOSDocumentsPath(
    const std::filesystem::path &path,
    const std::filesystem::path &currentDocumentsRoot);

std::string StoredPathText(std::filesystem::path path);
std::string StoredFolderPathText(std::filesystem::path path);
void ToRelativePath(std::filesystem::path &path);
void ToAbsolutePath(std::filesystem::path &path);
void ConfigureArchiveCachePathNormalization();

} // namespace chart_storage_identity
