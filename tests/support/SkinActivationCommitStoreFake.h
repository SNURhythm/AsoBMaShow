#pragma once

#include "skin/package/SkinActivationCommitStore.h"

#include <cstddef>
#include <memory>

namespace skin::test_support {

class SkinActivationCommitStoreFake final : public SkinActivationCommitStore {
public:
  SkinActivationCommitStoreFake();
  ~SkinActivationCommitStoreFake() override;

  CommitActivationResult
  beginPreparedActivationCommit(PreparedSkinActivation &&prepared,
                                ISkinProfileSettingsOwner &owner) override;
  CommitActivationResult
  pollPreparedActivationCommit(std::uint64_t ticket,
                               ISkinProfileSettingsOwner &owner) override;
  void removeProfileActivations(const SkinProfileId &profile) override;

  void setNextActivationDisposition(ActivationCommitDisposition disposition);
  void throwOnNextProfileRemoval();
  std::size_t removedProfileCount() const noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace skin::test_support
