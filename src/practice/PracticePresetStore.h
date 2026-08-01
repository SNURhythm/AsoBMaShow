#pragma once

#include "PracticeConfiguration.h"
#include "../VersionedJson.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace practice {
inline constexpr int kPresetSchemaVersion = 1;

enum class PresetFileKind { Invalid, Primary, AtomicSidecar };

[[nodiscard]] PresetFileKind
classifyPresetFilename(std::string_view filename) noexcept;

struct PresetFileValidationResult {
  versioned_json::LoadStatus status = versioned_json::LoadStatus::InvalidRoot;
  std::vector<std::string> diagnostics;

  [[nodiscard]] bool valid() const noexcept {
    return status == versioned_json::LoadStatus::Loaded;
  }
};

[[nodiscard]] PresetFileValidationResult
validatePresetFile(const std::filesystem::path &path,
                   int expectedSchemaVersion);

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

  [[nodiscard]] bool usable() const noexcept;
  [[nodiscard]] std::optional<std::string> notice() const;
};

[[nodiscard]] bool
installPresetLoadState(PresetLoadResult loaded, bool applyLastUsed,
                       Configuration &configuration,
                       std::vector<NamedPreset> &namedPresets,
                       std::optional<std::string> &selectedPresetId);

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
