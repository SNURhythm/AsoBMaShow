#include "VersionedJson.h"

#include <fstream>

namespace versioned_json {
LoadResult loadAndMigrate(const std::filesystem::path &path,
                          int currentVersion,
                          std::span<const Migration> migrations) {
  LoadResult result;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    result.status = ec ? LoadStatus::IoError : LoadStatus::Missing;
    if (ec) {
      result.diagnostics.push_back("settings existence check failed: " +
                                   ec.message());
    }
    return result;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    result.status = LoadStatus::IoError;
    result.diagnostics.push_back("unable to open versioned JSON file");
    return result;
  }
  try {
    input >> result.document;
  } catch (const nlohmann::json::exception &error) {
    result.status = LoadStatus::Malformed;
    result.diagnostics.push_back(std::string("malformed JSON: ") +
                                 error.what());
    return result;
  }
  if (input.bad()) {
    result.status = LoadStatus::IoError;
    result.diagnostics.push_back("I/O failure while reading versioned JSON");
    return result;
  }
  if (!result.document.is_object()) {
    result.status = LoadStatus::InvalidRoot;
    result.diagnostics.push_back("versioned JSON root must be an object");
    return result;
  }

  int version = 0;
  const auto versionIt = result.document.find("schemaVersion");
  if (versionIt != result.document.end()) {
    if (!versionIt->is_number_integer()) {
      result.status = LoadStatus::InvalidRoot;
      result.diagnostics.push_back("schemaVersion must be an integer");
      return result;
    }
    version = versionIt->get<int>();
  }
  if (version < 0) {
    result.status = LoadStatus::InvalidRoot;
    result.diagnostics.push_back("schemaVersion must be non-negative");
    return result;
  }
  if (version > currentVersion) {
    result.status = LoadStatus::FutureVersion;
    result.diagnostics.push_back("schemaVersion " + std::to_string(version) +
                                 " is newer than supported version " +
                                 std::to_string(currentVersion));
    return result;
  }

  while (version < currentVersion) {
    if (static_cast<std::size_t>(version) >= migrations.size() ||
        !migrations[static_cast<std::size_t>(version)]) {
      result.status = LoadStatus::MigrationFailed;
      result.diagnostics.push_back("missing migration from schema version " +
                                   std::to_string(version));
      return result;
    }
    std::string errorMessage;
    if (!migrations[static_cast<std::size_t>(version)](result.document,
                                                       errorMessage)) {
      result.status = LoadStatus::MigrationFailed;
      result.diagnostics.push_back(
          "migration from schema version " + std::to_string(version) +
          " failed" + (errorMessage.empty() ? std::string()
                                              : ": " + errorMessage));
      return result;
    }
    ++version;
    result.document["schemaVersion"] = version;
  }

  result.status = LoadStatus::Loaded;
  return result;
}

bool saveAtomic(const std::filesystem::path &path,
                const nlohmann::json &document, std::string &errorMessage,
                const atomic_file::Operations *operations) {
  if (!document.is_object()) {
    errorMessage = "versioned JSON root must be an object";
    return false;
  }
  const std::string encoded = document.dump(2) + "\n";
  return atomic_file::writeWithBackup(
      path, std::as_bytes(std::span(encoded)), errorMessage, operations);
}
} // namespace versioned_json
