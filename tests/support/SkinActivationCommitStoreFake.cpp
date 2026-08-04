#include "SkinActivationCommitStoreFake.h"

#include <algorithm>
#include <list>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace skin::test_support {
namespace {

static_assert(std::is_nothrow_move_constructible_v<CommitActivationResult>);

CommitActivationResult
terminalOwnerFailure(std::uint64_t storeTicket,
                     const SkinProfileCommitResult &owner) {
  CommitActivationResult result;
  result.ticket = storeTicket;
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

struct SkinActivationCommitStoreFake::State {
  struct Pending {
    std::uint64_t storeTicket = 0;
    std::uint64_t ownerTicket = 0;
    std::unique_ptr<PreparedSkinActivation> prepared;
  };

  std::list<Pending> pending;
  ActivationCommitDisposition nextDisposition =
      ActivationCommitDisposition::ActivatedRequested;
  std::uint64_t nextStoreTicket = 0;
  std::size_t removedProfiles = 0;
  bool throwOnRemove = false;
};

SkinActivationCommitStoreFake::SkinActivationCommitStoreFake()
    : state_(std::make_unique<State>()) {}

SkinActivationCommitStoreFake::~SkinActivationCommitStoreFake() = default;

CommitActivationResult
SkinActivationCommitStoreFake::beginPreparedActivationCommit(
    PreparedSkinActivation &&prepared, ISkinProfileSettingsOwner &owner) {
  constexpr std::uint64_t storeTicketNamespace = std::uint64_t{1} << 63;
  const auto storeTicket = storeTicketNamespace | ++state_->nextStoreTicket;
  state_->pending.push_back(
      {.storeTicket = storeTicket,
       .prepared =
           std::make_unique<PreparedSkinActivation>(std::move(prepared))});
  auto pending = std::prev(state_->pending.end());
  SkinProfileCommitResult ownerResult;
  try {
    ownerResult =
        owner.beginCommit(pending->prepared->profileId,
                          pending->prepared->expectedProfileGeneration,
                          pending->prepared->candidateProfileSettings);
  } catch (...) {
    state_->pending.erase(pending);
    throw;
  }
  if (ownerResult.status != SkinProfileCommitResult::Status::Pending) {
    state_->pending.erase(pending);
    return terminalOwnerFailure(0, ownerResult);
  }

  pending->ownerTicket = ownerResult.ticket;
  return {.disposition = ActivationCommitDisposition::PendingProfileSave,
          .ticket = storeTicket,
          .profileSnapshot = std::move(ownerResult.snapshot)};
}

CommitActivationResult
SkinActivationCommitStoreFake::pollPreparedActivationCommit(
    std::uint64_t ticket, ISkinProfileSettingsOwner &owner) {
  const auto found =
      std::find_if(state_->pending.begin(), state_->pending.end(),
                   [ticket](const State::Pending &pending) {
                     return pending.storeTicket == ticket;
                   });
  if (found == state_->pending.end()) {
    return {.disposition = ActivationCommitDisposition::RetainedPrevious,
            .ticket = ticket,
            .diagnostics = {{.code = "skin.test.unknown_store_ticket",
                             .message = "Unknown test store ticket"}}};
  }

  auto ownerResult = owner.pollCommit(found->ownerTicket);
  if (ownerResult.status == SkinProfileCommitResult::Status::Pending) {
    return {.disposition = ActivationCommitDisposition::PendingProfileSave,
            .ticket = ticket,
            .profileSnapshot = std::move(ownerResult.snapshot)};
  }
  if (ownerResult.status != SkinProfileCommitResult::Status::Persisted) {
    auto result = terminalOwnerFailure(ticket, ownerResult);
    owner.acknowledgeCommit(found->ownerTicket);
    state_->pending.erase(found);
    return result;
  }

  const auto disposition = state_->nextDisposition;
  CommitActivationResult result{.disposition = disposition,
                                .ticket = ticket,
                                .profileSnapshot =
                                    std::move(ownerResult.snapshot)};
  if (disposition == ActivationCommitDisposition::ActivatedRequested) {
    result.activation = std::move(found->prepared->activation);
  }
  state_->nextDisposition = ActivationCommitDisposition::ActivatedRequested;
  owner.acknowledgeCommit(found->ownerTicket);
  state_->pending.erase(found);
  return result;
}

void SkinActivationCommitStoreFake::removeProfileActivations(
    const SkinProfileId &) {
  if (std::exchange(state_->throwOnRemove, false)) {
    throw std::runtime_error("Synthetic profile removal failure");
  }
  ++state_->removedProfiles;
}

void SkinActivationCommitStoreFake::setNextActivationDisposition(
    ActivationCommitDisposition disposition) {
  state_->nextDisposition = disposition;
}

void SkinActivationCommitStoreFake::throwOnNextProfileRemoval() {
  state_->throwOnRemove = true;
}

std::size_t
SkinActivationCommitStoreFake::removedProfileCount() const noexcept {
  return state_->removedProfiles;
}

} // namespace skin::test_support
