#pragma once

#include "../targets.h"
#include <array>
#include <filesystem>
#include <span>
#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

namespace downloads {

// These roots come from the OS, never from a persisted job or journal. Keep
// commit/backup siblings of Documents, on the same app-owned storage volume.
inline std::filesystem::path androidPublicationTransactionRoot(
    const std::filesystem::path &downloadRoot,
    std::span<const std::filesystem::path> appRoots) {
  std::error_code error;
  const auto destination = std::filesystem::weakly_canonical(downloadRoot, error);
  if (error || !destination.is_absolute()) return {};
  for (const auto &appRoot : appRoots) {
    if (!appRoot.is_absolute()) continue;
    const auto root = std::filesystem::canonical(appRoot, error);
    if (error) continue;
    const auto relative = destination.lexically_relative(root / "Documents");
    if (relative.empty() || relative == "." || *relative.begin() == "..") continue;
    const auto transactionRoot = root / ".asobmashow-download-transactions";
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(transactionRoot, error))) return {};
    if (error && error != std::errc::no_such_file_or_directory) return {};
    return transactionRoot;
  }
  return {}; // No public-directory fallback on an unrecognized volume.
}

inline std::filesystem::path publicationTransactionRoot(const std::filesystem::path &downloadRoot) {
#if TARGET_OS_ANDROID
  const std::array<std::filesystem::path, 2> roots{GetAndroidExternalFilesDir(), GetAndroidInternalFilesDir()};
  return androidPublicationTransactionRoot(downloadRoot, roots);
#else
  return downloadRoot / ".asobmashow-transactions";
#endif
}

} // namespace downloads
