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

using FindBmsPrepareVariantRemoval = std::function<bool(
    const std::vector<std::filesystem::path> &, std::string &)>;

bool commitFindBmsPendingArtifact(
    const BmsSearchPendingArtifact &artifact, std::string &errorMessage,
    FindBmsRenameOperation renameOperation = {},
    std::vector<std::filesystem::path> *removedPaths = nullptr,
    FindBmsPrepareVariantRemoval prepareRemoval = {});

bool deleteFindBmsPendingArtifact(const BmsSearchPendingArtifact &artifact,
                                  std::string &errorMessage);

// Idempotent cleanup after a journal has verified that the destination was
// installed. Does not read or copy published payloads.
bool finishFindBmsPendingArtifactPublication(const BmsSearchPendingArtifact &artifact,
    std::string &errorMessage, std::vector<std::filesystem::path> *removedPaths,
    FindBmsPrepareVariantRemoval prepareRemoval = {});

} // namespace asobmshow::bms_search
