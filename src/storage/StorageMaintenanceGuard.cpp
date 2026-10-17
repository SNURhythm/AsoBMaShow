#include "StorageMaintenanceGuard.h"
#include <utility>
namespace storage_management {
StorageMaintenanceGuard::StorageMaintenanceGuard(
    std::function<void(const StorageReport &)> complete, std::function<void()> release)
    : complete_(std::move(complete)), release_(std::move(release)) {}
StorageMaintenanceGuard::StorageMaintenanceGuard(StorageMaintenanceGuard &&other) noexcept
    : complete_(std::move(other.complete_)), release_(std::exchange(other.release_, {})) {}
StorageMaintenanceGuard::~StorageMaintenanceGuard() {
  if (release_) release_();
}
void StorageMaintenanceGuard::Complete(const StorageReport &report) {
  if (auto callback = std::exchange(complete_, {})) callback(report);
}
}
