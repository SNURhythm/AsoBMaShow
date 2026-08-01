#pragma once

#include "AtomicFile.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace versioned_json {
enum class LoadStatus {
  Loaded,
  Missing,
  IoError,
  Malformed,
  InvalidRoot,
  FutureVersion,
  MigrationFailed,
};

using Migration =
    std::function<bool(nlohmann::json &document, std::string &errorMessage)>;

struct LoadResult {
  LoadStatus status = LoadStatus::Missing;
  nlohmann::json document;
  std::vector<std::string> diagnostics;
};

LoadResult loadAndMigrate(const std::filesystem::path &path,
                          int currentVersion,
                          std::span<const Migration> migrations);

bool saveAtomic(const std::filesystem::path &path,
                const nlohmann::json &document, std::string &errorMessage,
                const atomic_file::Operations *operations = nullptr);

bool saveAtomicWithoutBackup(
    const std::filesystem::path &path, const nlohmann::json &document,
    std::string &errorMessage,
    const atomic_file::Operations *operations = nullptr);
} // namespace versioned_json
