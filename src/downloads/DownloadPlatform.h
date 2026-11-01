#pragma once
#include "DownloadJobs.h"

namespace downloads::platform {
std::filesystem::path privateRoot();
void initialize();
// iOS entry-point setup, before UIApplicationMain.
void prepareApplicationLaunch();
std::string startTransfer(const Job &job, const std::filesystem::path &destination);
void cancelTransfer(const Job &job);
void beginImport(const Job &job);
void finishActivity(const Job &job, bool success);
void reportProgress(const Job &job);
} // namespace downloads::platform
