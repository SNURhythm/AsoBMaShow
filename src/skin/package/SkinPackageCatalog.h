#pragma once

#include "SkinPackageTypes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace skin {

struct SkinDiagnosticHistoryRecord;

struct SkinCatalogEntrySnapshot {
  SkinEntryId entry;
  std::string revisionDigest;
  SkinValidationDisposition validation = SkinValidationDisposition::Invalid;
  std::optional<SkinEntryMetadataSnapshot> metadata;
  std::vector<std::string> validatedConfigurationDigests;
  std::vector<SkinDiagnostic> diagnostics;
};

struct SkinPackageCatalogSnapshot {
  std::uint64_t catalogGeneration = 0;
  std::uint64_t sourceGeneration = 0;
  std::vector<SkinPackageId> packages;
  std::vector<SkinCatalogEntrySnapshot> entries;
};

class SkinPackageCatalog {
public:
  explicit SkinPackageCatalog(std::filesystem::path privateCatalogRoot);
  ~SkinPackageCatalog();

  SkinPackageCatalog(const SkinPackageCatalog &) = delete;
  SkinPackageCatalog &operator=(const SkinPackageCatalog &) = delete;

  void recover();
  std::shared_ptr<const SkinPackageCatalogSnapshot> snapshot() const noexcept;
  std::vector<SkinDiagnosticHistoryRecord> loadDiagnosticHistory() const;
  bool replaceDiagnosticHistory(
      std::span<const SkinDiagnosticHistoryRecord> records);
  void flush();
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
