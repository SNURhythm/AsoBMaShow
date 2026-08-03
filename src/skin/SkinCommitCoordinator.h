#pragma once

#include "package/SkinPackageStore.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skin {

using SkinActivationClientId = std::uint64_t;

struct SkinActivationSubmissionResult {
  bool accepted = false;
  std::uint64_t ticket = 0;
  std::vector<SkinDiagnostic> diagnostics;
};

struct SkinActivationCompletion {
  SkinActivationClientId client = 0;
  std::uint64_t ticket = 0;
  CommitActivationResult result;
};

struct SkinProfileCommitSubmissionResult {
  bool accepted = false;
  std::uint64_t ticket = 0;
  std::vector<SkinDiagnostic> diagnostics;
};

struct SkinProfileCommitCompletion {
  SkinActivationClientId client = 0;
  std::uint64_t ticket = 0;
  SkinProfileCommitResult result;
};

class SkinProfileMutationBarrier {
public:
  SkinProfileMutationBarrier(SkinProfileMutationBarrier &&) noexcept;
  SkinProfileMutationBarrier &operator=(SkinProfileMutationBarrier &&) noexcept;
  SkinProfileMutationBarrier(const SkinProfileMutationBarrier &) = delete;
  SkinProfileMutationBarrier &
  operator=(const SkinProfileMutationBarrier &) = delete;
  ~SkinProfileMutationBarrier();

  const SkinProfileId &profileId() const noexcept;

private:
  SkinProfileMutationBarrier(SkinProfileId profile,
                             std::function<void()> resume);
  struct State;
  std::unique_ptr<State> state_;
  friend class SkinCommitCoordinator;
};

struct BeginSkinProfileMutationResult {
  std::optional<SkinProfileMutationBarrier> barrier;
  std::string error;
};

class SkinCommitCoordinator {
public:
  SkinCommitCoordinator(SkinPackageStore &, ISkinProfileSettingsOwner &);
  ~SkinCommitCoordinator();

  SkinCommitCoordinator(const SkinCommitCoordinator &) = delete;
  SkinCommitCoordinator &operator=(const SkinCommitCoordinator &) = delete;

  SkinActivationClientId createClient();
  SkinActivationSubmissionResult
  submitActivation(SkinActivationClientId client,
                   PreparedSkinActivation &&prepared);
  SkinProfileCommitSubmissionResult
  submitProfileSettings(SkinActivationClientId client,
                        const VersionedSkinProfileSettings &base,
                        SkinProfileSettings candidate);
  void poll();
  std::vector<SkinActivationCompletion>
  takeCompletions(SkinActivationClientId client);
  std::vector<SkinProfileCommitCompletion>
  takeProfileCompletions(SkinActivationClientId client);
  void detachClient(SkinActivationClientId client) noexcept;
  std::vector<VersionedSkinProfileSettings> takeRevalidationRequests();
  BeginSkinProfileMutationResult
  beginProfileMutation(const SkinProfileId &profile);
  void finishProfileMutation(SkinProfileMutationBarrier &&barrier,
                             bool mutationSucceeded,
                             bool profileStillExists) noexcept;
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
