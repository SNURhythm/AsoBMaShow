#pragma once
#include "StorageRecoveryProgress.h"
#include <filesystem>
#include <vector>
class ApplicationContext;
namespace storage_management {
class StorageTask;
// Database registrations are available; no normal library/media work has begun.
// Hosts that own the event queue on the recovery thread can drain lifecycle
// events at safe checkpoints, including phases without progress notifications.
bool InitializeStorageServices(ApplicationContext &, StorageRecoveryProgressCallback = {},
                               std::function<void()> pollEvents = {});
std::filesystem::path StoragePrivateRoot(ApplicationContext &);
void PollStorageTask(ApplicationContext &);
void RequestCancelStorageTask(ApplicationContext &);
void CancelAndWaitStorageTask(ApplicationContext &);
void ShutdownStorageServices(ApplicationContext &);
}
