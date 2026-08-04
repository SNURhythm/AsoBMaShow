#pragma once

#include "../../ProfileSettingsPersistenceCoordinator.h"
#include "SkinTreeSnapshotter.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skin {

struct ValidatedSkinActivation {
  SkinRevisionLease revision;
  SkinEntryId entry;
  EntryProfileSettings reconciledSettings;
  std::string configurationDigest;
};

struct PreparedSkinActivation {
  std::uint64_t sourceGeneration = 0;
  std::uint64_t catalogGeneration = 0;
  std::uint64_t expectedProfileGeneration = 0;
  SkinProfileId profileId;
  ValidatedSkinActivation activation;
  SkinProfileSettings candidateProfileSettings;
};

enum class ActivationCommitDisposition : std::uint8_t {
  PendingProfileSave,
  ActivatedRequested,
  RetainedPrevious,
  ProfileGenerationChanged,
  SourceGenerationChanged,
  ProfileCommittedNeedsRevalidation,
};

struct CommitActivationResult {
  ActivationCommitDisposition disposition =
      ActivationCommitDisposition::RetainedPrevious;
  std::uint64_t ticket = 0;
  std::optional<ValidatedSkinActivation> activation;
  std::optional<VersionedSkinProfileSettings> profileSnapshot;
  std::vector<SkinDiagnostic> diagnostics;
};

// The coordinator only needs this durable activation-commit boundary. The
// package store owns the prepared activation after admission and returns a
// terminal result from subsequent polls.
class SkinActivationCommitStore {
public:
  virtual ~SkinActivationCommitStore() = default;

  virtual CommitActivationResult
  beginPreparedActivationCommit(PreparedSkinActivation &&prepared,
                                ISkinProfileSettingsOwner &owner) = 0;
  virtual CommitActivationResult
  pollPreparedActivationCommit(std::uint64_t ticket,
                               ISkinProfileSettingsOwner &owner) = 0;
  virtual void removeProfileActivations(const SkinProfileId &profile) = 0;
};

} // namespace skin
