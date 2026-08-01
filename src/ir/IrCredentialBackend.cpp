#include "IrCredentialBackend.h"

#include "../AtomicFile.h"
#include "../targets.h"
#include "IrCredentialStore.h"
#include "IrProfileSettings.h"

#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
#include "IosKeychainCredentialBackend.h"
#endif

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace ir {
namespace {

class FileIrCredentialBackend final : public IrCredentialBackend {
public:
  explicit FileIrCredentialBackend(std::filesystem::path applicationDataRoot)
      : applicationDataRoot_(std::move(applicationDataRoot)) {}

  bool requiresLegacyFileMigration() const noexcept override { return false; }

  IrCredentialBackendReadResult
  load(std::string_view profileId,
       std::string_view providerId) noexcept override {
    try {
      if (!valid(profileId, providerId)) {
        return failedRead("credential identity is invalid");
      }
      auto loaded = IrCredentialStore::load(pathFor(profileId));
      if (loaded.status == IrCredentialLoadStatus::Missing) {
        return {.status = IrCredentialBackendReadStatus::Missing};
      }
      if (loaded.status != IrCredentialLoadStatus::Loaded) {
        return failedRead("credential file could not be read safely");
      }
      const auto found =
          loaded.credentials.apiKeys.find(std::string(providerId));
      if (found == loaded.credentials.apiKeys.end()) {
        return {.status = IrCredentialBackendReadStatus::Missing};
      }
      return {.status = IrCredentialBackendReadStatus::Loaded,
              .apiKey = std::move(found->second)};
    } catch (...) {
      return failedRead("credential file read failed unexpectedly");
    }
  }

  IrCredentialBackendWriteResult
  replace(std::string_view profileId, std::string_view providerId,
          std::string_view apiKey) noexcept override {
    try {
      if (!valid(profileId, providerId) ||
          !IrCredentialStore::isApiKeyFormatValid(apiKey)) {
        return failedWrite("credential identity or key is invalid");
      }
      const auto result = IrCredentialStore::replaceApiKey(
          pathFor(profileId), std::string(providerId), std::string(apiKey));
      return {.succeeded = result.succeeded,
              .diagnostic = std::move(result.diagnostic)};
    } catch (...) {
      return failedWrite("credential file update failed unexpectedly");
    }
  }

  IrCredentialBackendWriteResult
  remove(std::string_view profileId,
         std::string_view providerId) noexcept override {
    try {
      if (!valid(profileId, providerId)) {
        return failedWrite("credential identity is invalid");
      }
      const auto result = IrCredentialStore::removeApiKey(
          pathFor(profileId), std::string(providerId));
      return {.succeeded = result.succeeded,
              .diagnostic = std::move(result.diagnostic)};
    } catch (...) {
      return failedWrite("credential file removal failed unexpectedly");
    }
  }

  IrCredentialBackendWriteResult
  removeProfile(std::string_view profileId) noexcept override {
    try {
      if (!isValidCredentialProfileId(profileId)) {
        return failedWrite("credential profile identity is invalid");
      }
      const auto path = pathFor(profileId);
      std::string diagnostic;
      if (!atomic_file::removeBackupArtifacts(path, diagnostic)) {
        return failedWrite(std::move(diagnostic));
      }
      std::error_code error;
      std::filesystem::remove(path, error);
      if (error) {
        return failedWrite("credential file could not be removed");
      }
      return {.succeeded = true};
    } catch (...) {
      return failedWrite("profile credential removal failed unexpectedly");
    }
  }

private:
  [[nodiscard]] bool valid(std::string_view profileId,
                           std::string_view providerId) const noexcept {
    return isValidCredentialProfileId(profileId) &&
           isValidProviderId(providerId);
  }

  [[nodiscard]] std::filesystem::path
  pathFor(std::string_view profileId) const {
    return applicationDataRoot_ / "profiles" / std::string(profileId) /
           "ir-credentials.json";
  }

  static IrCredentialBackendReadResult failedRead(std::string diagnostic) {
    return {.status = IrCredentialBackendReadStatus::Failed,
            .diagnostic = std::move(diagnostic)};
  }

  static IrCredentialBackendWriteResult failedWrite(std::string diagnostic) {
    return {.succeeded = false, .diagnostic = std::move(diagnostic)};
  }

  std::filesystem::path applicationDataRoot_;
};

} // namespace

bool isValidCredentialProfileId(std::string_view profileId) noexcept {
  return !profileId.empty() && profileId.size() <= 128 &&
         std::ranges::all_of(profileId, [](unsigned char character) {
           return std::isalnum(character) != 0 || character == '-' ||
                  character == '_';
         });
}

std::unique_ptr<IrCredentialBackend> CreatePlatformIrCredentialBackend(
    const std::filesystem::path &applicationDataRoot) {
#if TARGET_OS_IOS || TARGET_OS_IPHONE || TARGET_OS_SIMULATOR
  (void)applicationDataRoot;
  return CreateIosKeychainCredentialBackend();
#else
  return std::make_unique<FileIrCredentialBackend>(applicationDataRoot);
#endif
}

} // namespace ir
