#pragma once
#include "DownloadJobs.h"

namespace downloads::platform {
std::filesystem::path privateRoot();
void initialize();
std::string startTransfer(const Job &job, const std::filesystem::path &destination);
void cancelTransfer(const Job &job);
void finishActivity(const Job &job, bool success);
void reportProgress(const Job &job);
} // namespace downloads::platform
