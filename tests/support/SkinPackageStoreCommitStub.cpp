#include "SkinPackageStoreCommitStub.h"

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace skin {
namespace {

struct StubStoreState {
  struct Pending {
    std::uint64_t ticket = 0;
    std::unique_ptr<PreparedSkinActivation> prepared;
  };

  std::list<Pending> pending;
  ActivationCommitDisposition nextDisposition =
      ActivationCommitDisposition::ActivatedRequested;
  std::size_t removedProfiles = 0;
  bool throwOnRemove = false;
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
  auto &state = stubStates()[this];
  state.pending.push_back({.prepared = std::make_unique<PreparedSkinActivation>(
                               std::move(prepared))});
  auto pending = std::prev(state.pending.end());
  SkinProfileCommitResult ownerResult;
  try {
    ownerResult =
        owner.beginCommit(pending->prepared->profileId,
                          pending->prepared->expectedProfileGeneration,
                          pending->prepared->candidateProfileSettings);
  } catch (...) {
    state.pending.erase(pending);
    throw;
  }
  if (ownerResult.status != SkinProfileCommitResult::Status::Pending) {
    state.pending.erase(pending);
    return terminalOwnerFailure(ownerResult);
  }

  const auto ticket = ownerResult.ticket;
  pending->ticket = ticket;
  return {.disposition = ActivationCommitDisposition::PendingProfileSave,
          .ticket = ticket,
          .profileSnapshot = std::move(ownerResult.snapshot)};
}

CommitActivationResult SkinPackageStore::pollPreparedActivationCommit(
    std::uint64_t ticket, ISkinProfileSettingsOwner &owner) {
  auto &state = stubStates()[this];
  const auto found =
      std::find_if(state.pending.begin(), state.pending.end(),
                   [ticket](const StubStoreState::Pending &pending) {
                     return pending.ticket == ticket;
                   });
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
    result.activation = std::move(found->prepared->activation);
  }
  state.pending.erase(found);
  return result;
}

void SkinPackageStore::removeProfileActivations(const SkinProfileId &) {
  auto &state = stubStates()[this];
  if (std::exchange(state.throwOnRemove, false)) {
    throw std::runtime_error("Synthetic profile removal failure");
  }
  ++state.removedProfiles;
}

namespace test_support {

void resetStore(SkinPackageStore &store) { stubStates()[&store] = {}; }

void setNextActivationDisposition(SkinPackageStore &store,
                                  ActivationCommitDisposition disposition) {
  stubStates()[&store].nextDisposition = disposition;
}

void throwOnNextProfileRemoval(SkinPackageStore &store) {
  stubStates()[&store].throwOnRemove = true;
}

std::size_t removedProfileCount(const SkinPackageStore &store) {
  return stubStates()[&store].removedProfiles;
}

} // namespace test_support
} // namespace skin
