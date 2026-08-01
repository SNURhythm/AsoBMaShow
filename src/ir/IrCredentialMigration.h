#pragma once

#include "IrCredentialBackend.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ir {

enum class IrCredentialMigrationStatus { Succeeded, NotNeeded, Failed };

struct IrCredentialMigrationResult {
  IrCredentialMigrationStatus status = IrCredentialMigrationStatus::Failed;
  std::size_t migratedCredentials = 0;
  std::string diagnostic;

  [[nodiscard]] bool ready() const noexcept {
    return status == IrCredentialMigrationStatus::Succeeded ||
           status == IrCredentialMigrationStatus::NotNeeded;
  }
};

struct IrCredentialMigrationOperations {
  std::function<bool(const std::filesystem::path &, std::string &)>
      removeBackupArtifacts;
  std::function<bool(const std::filesystem::path &, std::string &)>
      removeLegacyFile;
};

[[nodiscard]] IrCredentialMigrationOperations
defaultIrCredentialMigrationOperations();

[[nodiscard]] IrCredentialMigrationResult migrateLegacyIrCredentials(
    std::string_view profileId, const std::filesystem::path &legacyPath,
    IrCredentialBackend &backend,
    const IrCredentialMigrationOperations *operations = nullptr) noexcept;

} // namespace ir
