#include "IrCredentialMigration.h"

#include "../AtomicFile.h"
#include "IrCredentialStore.h"

#include <system_error>
#include <utility>

namespace ir {
namespace {

IrCredentialMigrationResult failure(std::string diagnostic) {
  return {.status = IrCredentialMigrationStatus::Failed,
          .diagnostic = std::move(diagnostic)};
}

bool removeLegacyFile(const std::filesystem::path &path,
                      std::string &diagnostic) {
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  if (error) {
    diagnostic = "legacy credential file could not be removed";
    return false;
  }
  if (!removed) {
    return true;
  }
  return true;
}

} // namespace

IrCredentialMigrationOperations defaultIrCredentialMigrationOperations() {
  return {
      .removeBackupArtifacts =
          [](const std::filesystem::path &path, std::string &diagnostic) {
            return atomic_file::removeBackupArtifacts(path, diagnostic);
          },
      .removeLegacyFile = removeLegacyFile,
      .syncDirectory =
          [](const std::filesystem::path &path, std::string &diagnostic) {
            return atomic_file::syncDirectory(path, diagnostic);
          },
  };
}

IrCredentialMigrationResult migrateLegacyIrCredentials(
    std::string_view profileId, const std::filesystem::path &legacyPath,
    IrCredentialBackend &backend,
    const IrCredentialMigrationOperations *operations) noexcept {
  try {
    if (!isValidCredentialProfileId(profileId)) {
      return failure("credential migration profile identity is invalid");
    }
    const IrCredentialMigrationOperations defaults =
        defaultIrCredentialMigrationOperations();
    const auto &ops = operations == nullptr ? defaults : *operations;
    if (!ops.removeBackupArtifacts || !ops.removeLegacyFile ||
        !ops.syncDirectory) {
      return failure("credential migration cleanup is unavailable");
    }
    const auto loaded = IrCredentialStore::load(legacyPath);
    if (loaded.status == IrCredentialLoadStatus::Missing) {
      std::string ignoredDiagnostic;
      if (!ops.removeBackupArtifacts(legacyPath, ignoredDiagnostic)) {
        return failure("legacy credential artifacts could not be removed");
      }
      return {.status = IrCredentialMigrationStatus::NotNeeded};
    }
    if (loaded.status != IrCredentialLoadStatus::Loaded) {
      return failure("legacy credential file could not be migrated safely");
    }

    std::size_t migrated = 0;
    for (const auto &[providerId, apiKey] : loaded.credentials.apiKeys) {
      const auto replaced = backend.replace(profileId, providerId, apiKey);
      if (!replaced.succeeded) {
        return failure("secure credential storage could not be updated");
      }
      const auto verified = backend.load(profileId, providerId);
      if (verified.status != IrCredentialBackendReadStatus::Loaded ||
          !verified.apiKey || *verified.apiKey != apiKey) {
        return failure("secure credential storage verification failed");
      }
      ++migrated;
    }

    std::string ignoredDiagnostic;
    if (!ops.removeBackupArtifacts(legacyPath, ignoredDiagnostic)) {
      return failure("legacy credential artifacts could not be removed");
    }
    if (!ops.removeLegacyFile(legacyPath, ignoredDiagnostic)) {
      return failure("legacy credential file could not be removed");
    }
    const auto parent = legacyPath.parent_path().empty()
                            ? std::filesystem::path(".")
                            : legacyPath.parent_path();
    if (!ops.syncDirectory(parent, ignoredDiagnostic)) {
      return failure("legacy credential file removal could not be persisted");
    }
    return {.status = IrCredentialMigrationStatus::Succeeded,
            .migratedCredentials = migrated};
  } catch (...) {
    return failure("credential migration failed unexpectedly");
  }
}

} // namespace ir
