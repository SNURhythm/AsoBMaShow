#pragma once
#include "DownloadCoordinator.h"

namespace downloads {
BmsSearchResult importArchiveJob(const Job &job, const std::filesystem::path &archive,
    const std::filesystem::path &privateJobRoot, std::atomic_bool &cancelled,
    BmsSearchDownloadProgressCallback progress, BeforePublication beforePublication);
BmsSearchResult resolveArchiveJob(const Job &job, const std::filesystem::path &privateJobRoot,
    BmsSearchPendingArtifactDecision decision, std::atomic_bool &cancelled,
    BeforePublication beforePublication);
} // namespace downloads
