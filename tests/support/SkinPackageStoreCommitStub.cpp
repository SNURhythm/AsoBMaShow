#include "SkinPackageStoreCommitStub.h"

#include <map>
#include <memory>

namespace skin {
namespace {

struct StubStoreState {
  std::map<std::uint64_t, std::unique_ptr<PreparedSkinActivation>> pending;
  ActivationCommitDisposition nextDisposition =
      ActivationCommitDisposition::ActivatedRequested;
  std::size_t removedProfiles = 0;
};

std::map<const SkinPackageStore *, StubStoreState> &stubStates() {
  static std::map<const SkinPackageStore *, StubStoreState> states;
  return states;
}

CommitActivationResult
terminalOwnerFailure(const SkinProfileCommitResult &owner) {
  CommitActivationResult result;
  result.ticket = owner.ticket;
  result.profileSnapshot = owner.snapshot;
  if (owner.failure) {
    result.diagnostics.push_back(*owner.failure);
  }
  result.disposition =
      owner.status == SkinProfileCommitResult::Status::GenerationChanged
          ? ActivationCommitDisposition::ProfileGenerationChanged
          : ActivationCommitDisposition::RetainedPrevious;
  return result;
}

} // namespace

CommitActivationResult SkinPackageStore::beginPreparedActivationCommit(
    PreparedSkinActivation &&prepared, ISkinProfileSettingsOwner &owner) {
  auto ownerResult =
      owner.beginCommit(prepared.profileId, prepared.expectedProfileGeneration,
                        prepared.candidateProfileSettings);
  if (ownerResult.status != SkinProfileCommitResult::Status::Pending) {
    return terminalOwnerFailure(ownerResult);
  }

  const auto ticket = ownerResult.ticket;
  stubStates()[this].pending.emplace(
      ticket, std::make_unique<PreparedSkinActivation>(std::move(prepared)));
  return {.disposition = ActivationCommitDisposition::PendingProfileSave,
          .ticket = ticket,
          .profileSnapshot = std::move(ownerResult.snapshot)};
}

CommitActivationResult SkinPackageStore::pollPreparedActivationCommit(
    std::uint64_t ticket, ISkinProfileSettingsOwner &owner) {
  auto &state = stubStates()[this];
  const auto found = state.pending.find(ticket);
  if (found == state.pending.end()) {
    return {.disposition = ActivationCommitDisposition::RetainedPrevious,
            .ticket = ticket,
            .diagnostics = {{.code = "skin.test.unknown_store_ticket",
                             .message = "Unknown test store ticket"}}};
  }

  auto ownerResult = owner.pollCommit(ticket);
  if (ownerResult.status == SkinProfileCommitResult::Status::Pending) {
    return {.disposition = ActivationCommitDisposition::PendingProfileSave,
            .ticket = ticket,
            .profileSnapshot = std::move(ownerResult.snapshot)};
  }
  if (ownerResult.status != SkinProfileCommitResult::Status::Persisted) {
    state.pending.erase(found);
    return terminalOwnerFailure(ownerResult);
  }

  const auto disposition = state.nextDisposition;
  state.nextDisposition = ActivationCommitDisposition::ActivatedRequested;
  CommitActivationResult result{.disposition = disposition,
                                .ticket = ticket,
                                .profileSnapshot =
                                    std::move(ownerResult.snapshot)};
  if (disposition == ActivationCommitDisposition::ActivatedRequested) {
    result.activation = std::move(found->second->activation);
  }
  state.pending.erase(found);
  return result;
}

void SkinPackageStore::removeProfileActivations(const SkinProfileId &) {
  ++stubStates()[this].removedProfiles;
}

namespace test_support {

void setNextActivationDisposition(SkinPackageStore &store,
                                  ActivationCommitDisposition disposition) {
  stubStates()[&store].nextDisposition = disposition;
}

std::size_t removedProfileCount(const SkinPackageStore &store) {
  return stubStates()[&store].removedProfiles;
}

} // namespace test_support
} // namespace skin
