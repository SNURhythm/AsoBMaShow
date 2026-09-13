#pragma once

#include "TemporaryCacheTypes.h"
#include "../ThreadCompat.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace archive_file {

// Owns synchronization and filesystem operations for materialized archive
// media. The facade supplies roots and identities so backend/index policy does
// not become a storage dependency. Use one owner for a shared cache namespace.
class TemporaryCache final {
public:
  using OutputPath = std::function<std::filesystem::path()>;
  using PathKey = std::function<std::string(const std::filesystem::path &)>;

  // The output identity is resolved under the mutation lock after root creation.
  // Same-size entries are reused; this is a cache, not content verification.
  std::optional<std::filesystem::path>
  materialize(const std::filesystem::path &root, const OutputPath &outputPath,
              const std::vector<unsigned char> &bytes,
              std::string *errorMessage = nullptr,
              const std::atomic_bool *cancelled = nullptr);

  // Protection applies to normalized top-level entry identities. The supplied
  // key function is evaluated during cleanup, including for protected paths.
  bool cleanup(const std::filesystem::path &root,
               TemporaryCacheCleanupResult &result,
               const std::vector<std::filesystem::path> &protectedPaths,
               const PathKey &pathKey, std::string *errorMessage = nullptr);

  // Best-effort observation: measurement does not block cache mutation.
  bool measure(const std::filesystem::path &root,
               TemporaryCacheUsageResult &result,
               std::string *errorMessage = nullptr,
               const std::stop_token *stopToken = nullptr) const;

private:
  std::mutex mutationMutex_;
};

} // namespace archive_file
