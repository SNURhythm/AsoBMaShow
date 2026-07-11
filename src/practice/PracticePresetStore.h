#pragma once

#include "PracticeConfiguration.h"
#include "../VersionedJson.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace practice {
struct NamedPreset {
  std::string id;
  std::string name;
  Configuration configuration;
};

struct PresetData {
  Configuration lastUsed;
  std::vector<NamedPreset> named;
};

struct PresetLoadResult {
  PresetData data;
  versioned_json::LoadStatus status;
  std::vector<std::string> diagnostics;
};

class PresetStore {
public:
  explicit PresetStore(std::filesystem::path practiceDirectory,
                       const atomic_file::Operations *operations = nullptr);

  PresetLoadResult load(std::string_view chartSha256,
                        long long chartEndMicros) const;
  bool saveLastUsed(std::string_view chartSha256, const Configuration &,
                    std::string &error);
  std::optional<std::string> saveNamed(std::string_view chartSha256,
                                       std::string name, const Configuration &,
                                       std::string &error);
  bool updateNamed(std::string_view chartSha256, std::string_view presetId,
                   const Configuration &, std::string &error);
  bool renameNamed(std::string_view chartSha256, std::string_view presetId,
                   std::string name, std::string &error);
  bool deleteNamed(std::string_view chartSha256, std::string_view presetId,
                   std::string &error);

private:
  std::filesystem::path practiceDirectory_;
  const atomic_file::Operations *operations_ = nullptr;
};
} // namespace practice
