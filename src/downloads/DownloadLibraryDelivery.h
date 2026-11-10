#pragma once
#include "DownloadCoordinator.h"
#include <map>

namespace downloads {
enum class LibraryDeliveryState { Pending, Complete, Failed, Missing };
// Application-thread delivery survives scene changes. A relaunch may repeat
// an idempotent index operation, but only a successful result acknowledges it.
class DownloadLibraryDelivery {
public:
  using Enqueue = std::function<std::uint64_t(const Job &)>;
  using Status = std::function<LibraryDeliveryState(std::uint64_t)>;
  DownloadLibraryDelivery(Enqueue enqueue, Status status)
      : enqueue_(std::move(enqueue)), status_(std::move(status)) {}
  void poll(DownloadCoordinator &owner) {
    for (const auto &job : owner.readyForLibrary()) {
      if (!job.libraryError.empty()) continue;
      auto pending = pending_.find(job.id);
      if (pending == pending_.end()) {
        const auto task = enqueue_(job);
        if (task) pending_.emplace(job.id, task);
        continue;
      }
      const auto status = status_(pending->second);
      if (status == LibraryDeliveryState::Complete) {
        owner.acknowledgeLibrary(job.id); pending_.erase(pending);
      } else if (status == LibraryDeliveryState::Failed || status == LibraryDeliveryState::Missing) {
        owner.setLibraryError(job.id, "Files were saved, but library indexing did not finish. Retry to index them.");
        pending_.erase(pending);
      }
    }
  }
private:
  Enqueue enqueue_;
  Status status_;
  std::map<std::string, std::uint64_t> pending_;
};
}
