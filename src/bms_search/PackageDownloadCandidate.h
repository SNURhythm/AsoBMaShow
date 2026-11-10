#pragma once

#include "Internal.h"

namespace asobmshow::bms_search {

using PackageArchiveSupportCheck = bool (*)(const std::string &extension);

DownloadCandidate configurePackageDownloadCandidate(
    DownloadCandidate candidate, const std::string &downloadUrl,
    const std::string &archiveName, const std::string &md5,
    PackageArchiveSupportCheck supportCheck);

} // namespace asobmshow::bms_search
