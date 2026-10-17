#pragma once
#include "StorageTypes.h"

namespace storage_management {
// Created on the application thread only after all media producers acknowledge
// shutdown. Neither callback may capture a scene; the application owns them.
class StorageMaintenanceGuard {
public:
  StorageMaintenanceGuard(std::function<void(const StorageReport &)> complete,
                          std::function<void()> release);
  StorageMaintenanceGuard(StorageMaintenanceGuard &&) noexcept;
  StorageMaintenanceGuard &operator=(StorageMaintenanceGuard &&) = delete;
  StorageMaintenanceGuard(const StorageMaintenanceGuard &) = delete;
  ~StorageMaintenanceGuard();
  void Complete(const StorageReport &);
private:
  std::function<void(const StorageReport &)> complete_;
  std::function<void()> release_;
};
}
