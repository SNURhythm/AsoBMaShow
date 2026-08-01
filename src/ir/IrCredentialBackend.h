#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ir {

enum class IrCredentialBackendReadStatus { Loaded, Missing, Failed };

struct IrCredentialBackendReadResult {
  IrCredentialBackendReadStatus status =
      IrCredentialBackendReadStatus::Failed;
  std::optional<std::string> apiKey;
  std::string diagnostic;
};

struct IrCredentialBackendWriteResult {
  bool succeeded = false;
  std::string diagnostic;
};

class IrCredentialBackend {
public:
  virtual ~IrCredentialBackend() = default;

  [[nodiscard]] virtual bool requiresLegacyFileMigration() const noexcept = 0;
  [[nodiscard]] virtual IrCredentialBackendReadResult
  load(std::string_view profileId, std::string_view providerId) noexcept = 0;
  [[nodiscard]] virtual IrCredentialBackendWriteResult
  replace(std::string_view profileId, std::string_view providerId,
          std::string_view apiKey) noexcept = 0;
  [[nodiscard]] virtual IrCredentialBackendWriteResult
  remove(std::string_view profileId, std::string_view providerId) noexcept = 0;
  [[nodiscard]] virtual IrCredentialBackendWriteResult
  removeProfile(std::string_view profileId) noexcept = 0;
};

[[nodiscard]] bool
isValidCredentialProfileId(std::string_view profileId) noexcept;

[[nodiscard]] std::unique_ptr<IrCredentialBackend>
CreatePlatformIrCredentialBackend(
    const std::filesystem::path &applicationDataRoot);

} // namespace ir
