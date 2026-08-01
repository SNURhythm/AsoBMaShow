#pragma once

#include "../BmsSearchService.h"

#include <functional>
#include <system_error>

namespace asobmshow::bms_search {

struct FindBmsDownloadAttempt {
  std::filesystem::path root;
  std::filesystem::path archivePath;
  std::filesystem::path extractedPath;
};

std::filesystem::path findBmsStagingBasePath();

std::optional<FindBmsDownloadAttempt>
createFindBmsDownloadAttempt(const std::string &archiveName,
                             std::string &errorMessage);

using FindBmsRenameOperation = std::function<void(
    const std::filesystem::path &, const std::filesystem::path &,
    std::error_code &)>;

bool commitFindBmsPendingArtifact(
    const BmsSearchPendingArtifact &artifact, std::string &errorMessage,
    FindBmsRenameOperation renameOperation = {},
    std::vector<std::filesystem::path> *removedPaths = nullptr);

bool deleteFindBmsPendingArtifact(const BmsSearchPendingArtifact &artifact,
                                  std::string &errorMessage);

} // namespace asobmshow::bms_search
