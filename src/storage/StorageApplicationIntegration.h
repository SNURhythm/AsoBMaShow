#pragma once
#include "StorageRecoveryProgress.h"
#include <filesystem>
#include <vector>
class ApplicationContext;
namespace storage_management {
class StorageTask;
// Database registrations are available; no normal library/media work has begun.
bool InitializeStorageServices(ApplicationContext &, StorageRecoveryProgressCallback = {});
std::filesystem::path StoragePrivateRoot(ApplicationContext &);
void PollStorageTask(ApplicationContext &);
void RequestCancelStorageTask(ApplicationContext &);
void CancelAndWaitStorageTask(ApplicationContext &);
void ShutdownStorageServices(ApplicationContext &);
}
