#pragma once

#include "SkinTreeSnapshotter.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace skin {

class PreparedPackage {
public:
  PreparedPackage(PreparedPackage &&) noexcept;
  PreparedPackage &operator=(PreparedPackage &&) noexcept;
  PreparedPackage(const PreparedPackage &) = delete;
  PreparedPackage &operator=(const PreparedPackage &) = delete;
  ~PreparedPackage();

  const SkinPackageId &packageId() const noexcept;
  const SkinRevision &candidateRevision() const noexcept;
  std::span<const SkinEntryId> entries() const noexcept;
  const std::filesystem::path &visibleStagingRoot() const noexcept;
  SkinRevisionReadView readView() const noexcept;

private:
  PreparedPackage(PreparedSkinRevision, std::vector<SkinEntryId>,
                  std::filesystem::path);
  struct State;
  std::unique_ptr<State> state_;
  friend class SkinArchiveImporter;
};

struct PreparePackageResult {
  std::optional<PreparedPackage> prepared;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

class SkinArchiveImporter {
public:
  SkinArchiveImporter(SkinStorageRoots, const SkinAliasDetector &);

  PreparePackageResult prepareArchive(const std::filesystem::path &,
                                      const SkinPackageId &, std::stop_token,
                                      SkinProgressCallback);
  PreparePackageResult prepareFolder(const std::filesystem::path &,
                                     const SkinPackageId &, std::stop_token,
                                     SkinProgressCallback);

private:
  SkinStorageRoots roots_;
  const SkinAliasDetector &aliases_;
};

} // namespace skin
