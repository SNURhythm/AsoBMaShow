#pragma once

#include "../AtomicFile.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct IrCredentials {
  std::map<std::string, std::string> apiKeys;

  bool operator==(const IrCredentials &) const = default;
};

enum class IrCredentialLoadStatus { Loaded, Missing, Invalid, FutureVersion };

struct IrCredentialLoadResult {
  IrCredentials credentials;
  IrCredentialLoadStatus status = IrCredentialLoadStatus::Missing;
  std::vector<std::string> diagnostics;
};

struct IrCredentialWriteResult {
  bool succeeded = false;
  std::string diagnostic;
};

class IrCredentialStore {
public:
  static constexpr int kCurrentSchemaVersion = 1;
  static constexpr std::size_t kMaximumApiKeyBytes = 4 * 1024;
  static constexpr std::size_t kMaximumFileBytes = 64 * 1024;

  [[nodiscard]] static IrCredentialLoadResult
  load(const std::filesystem::path &path);
  [[nodiscard]] static IrCredentialWriteResult
  save(const std::filesystem::path &path, const IrCredentials &credentials,
       const atomic_file::Operations *operations = nullptr);
  [[nodiscard]] static IrCredentialWriteResult
  replaceApiKey(const std::filesystem::path &path, std::string providerId,
                std::string apiKey,
                const atomic_file::Operations *operations = nullptr);
  [[nodiscard]] static IrCredentialWriteResult
  removeApiKey(const std::filesystem::path &path, std::string providerId,
               const atomic_file::Operations *operations = nullptr);
};

} // namespace ir
