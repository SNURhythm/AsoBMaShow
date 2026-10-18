#pragma once
#include "StoragePreflight.h"

namespace storage_management {
struct StorageEvidenceMetrics {
  std::uint64_t directoryChecks = 0, chartChecks = 0, membershipEnumerations = 0, membershipEntries = 0;
};
// One active directory, plus bounded chart-only evidence for its ancestors.
class StorageDirectoryEvidence {
public:
  StorageDirectoryEvidence(StorageDirectorySummary &, const StoragePreflight &,
      StorageDirectoryLimits, StorageEvidenceMetrics * = nullptr);
  bool Load(Checkpoint, std::string &);
  StorageDirectoryInventory &Candidates() { return *candidates_; }
  StorageDirectoryInventory &Directory() { return *directory_; }
  bool Candidate(const AssetCandidate &, Checkpoint, std::string &) const;
  bool BeforeCommit(const AssetCandidate &, const std::vector<StorageFileChange> &, Checkpoint, std::string &) const;
  bool Advance(const AssetCandidate &, const FileResult &, Checkpoint, std::string &);
  bool AdvanceRecovery(const std::vector<StorageFileChange> &, Checkpoint, std::string &);
  bool Finish(Checkpoint, std::string &);
private:
  struct ChartScope {
    StorageDirectorySummary summary;
    std::unique_ptr<StorageDirectoryInventory> charts;
  };
  bool CheckScopes(bool changingLocal, Checkpoint, std::string &) const;
  bool CheckEntries(const std::vector<StorageFileChange> &, Checkpoint, std::string &, StorageDirectoryStamp &) const;
  bool CheckChanges(const std::vector<StorageFileChange> &, Checkpoint, std::string &) const;
  bool ApplyChanges(const std::vector<StorageFileChange> &, std::string &);
  StorageDirectorySummary &summary_;
  const StoragePreflight &preflight_;
  StorageDirectoryLimits limits_;
  StorageEvidenceMetrics *metrics_;
  std::unique_ptr<StorageDirectoryInventory> directory_, candidates_;
  std::vector<ChartScope> scopes_;
};
}
