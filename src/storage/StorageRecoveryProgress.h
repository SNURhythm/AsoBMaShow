#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace storage_management {
enum class StorageRecoveryPhase { CheckingFolders, CheckingArchive, RestoringArchive, FinalizingArchive, Files, Complete };
struct StorageRecoveryProgress {
  StorageRecoveryPhase phase = StorageRecoveryPhase::CheckingFolders;
  std::filesystem::path currentPath;
  std::uint64_t completedOperations = 0;
  // Durable source position within the current archive, not bytes newly read
  // or a percentage of all recovery work. Untouched prefixes can be skipped.
  std::uint64_t archiveBytesDone = 0;
};
using StorageRecoveryProgressCallback = std::function<void(const StorageRecoveryProgress &)>;

// Presentation is observational: an unavailable UI must never interrupt a
// durable operation or turn successful recovery into a failed recovery.
inline void NotifyStorageRecoveryProgress(const StorageRecoveryProgressCallback &notify,
                                         const StorageRecoveryProgress &progress) noexcept {
  if (notify) { try { notify(progress); } catch (...) {} }
}
}
