#pragma once

#include "SkinTreeSnapshotter.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace skin {

enum class SkinImportIoOperation : std::uint8_t {
  VisibleRootIssued,
  BeforeVisibleDirectory,
  BeforeVisibleFile,
  AfterVisibleFileWritten,
  BeforeVisibleSnapshot,
  OwnedArchiveCopyChunk,
  RawZipRecord,
  OwnedArchiveHashChunk,
};

// Narrow observer seam for deterministic cancellation and path-race tests.
// Production leaves this unset.
class SkinImportIoObserver {
public:
  virtual ~SkinImportIoObserver() = default;
  virtual void reached(SkinImportIoOperation,
                       const std::filesystem::path &) const = 0;
};

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
                  std::filesystem::path, std::shared_ptr<void>);
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
  SkinArchiveImporter(
      SkinStorageRoots, const SkinAliasDetector &,
      std::shared_ptr<const SkinImportIoObserver> observer = {});

  PreparePackageResult prepareArchive(const std::filesystem::path &,
                                      const SkinPackageId &, std::stop_token,
                                      SkinProgressCallback);
  PreparePackageResult prepareFolder(const std::filesystem::path &,
                                     const SkinPackageId &, std::stop_token,
                                     SkinProgressCallback);

private:
  SkinStorageRoots roots_;
  const SkinAliasDetector &aliases_;
  std::shared_ptr<const SkinImportIoObserver> observer_;
};

} // namespace skin
