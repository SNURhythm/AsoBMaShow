#pragma once

#include "../repositories/ChartRepository.h"

#include <filesystem>
#include <vector>

namespace chart_library_platform {

void clearFolderAccess();
void refreshFolderAccess(const std::vector<ChartEntry> &entries);
std::filesystem::path resolveFolderEntryPath(const ChartEntry &entry);

} // namespace chart_library_platform
