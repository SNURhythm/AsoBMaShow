#pragma once

#include "ReplayCapabilities.h"
#include "ReplayFileStore.h"

#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace replay {

struct ReplayFileActionRequest {
  ModernReplayOwnerKind owner = ModernReplayOwnerKind::ChartResult;
  std::string attemptId;
};

enum class ReplayFileActionState {
  Verified,
  UserDeleted,
  Missing,
  Corrupt,
  Mismatched,
  UnsupportedCodecVersion,
  IoFailure,
  ResultNotFound,
  Invalid,
};

// Projects the cheap reference/file inspection state into the shared Records
// capability vocabulary. This deliberately does not claim that a verified
// file has decoded or materialized successfully; action consumers perform
// those stronger checks when the user selects a replay-dependent action.
[[nodiscard]] ReplayState
replayStateForFileAction(ReplayFileActionState state) noexcept;

struct ReplaySharePreparation {
  std::filesystem::path sourcePath;
  std::shared_ptr<void> sourceLifetime;
  std::string suggestedFilename;
};

struct ReplayFileActionOutcome {
  ReplayFileActionState state = ReplayFileActionState::Invalid;
  bool changed = false;
  bool cleanupPending = false;
  std::optional<ReplaySharePreparation> share;
  std::string diagnostic;
};

class ReplayFileActionService {
public:
  explicit ReplayFileActionService(ReplayRepository &repository);
  ReplayFileActionService(ReplayRepository &repository,
                          ReplayFileStore &store);

  [[nodiscard]] ReplayFileActionOutcome
  inspect(const ReplayFileActionRequest &request);
  [[nodiscard]] ReplayFileActionOutcome
  prepareShare(const ReplayFileActionRequest &request);
  [[nodiscard]] ReplayFileActionOutcome
  remove(const ReplayFileActionRequest &request);

private:
  struct ResolvedReference {
    ModernReplayOwnerKind owner = ModernReplayOwnerKind::ChartResult;
    std::string attemptId;
    ModernReplayFileReference reference;
  };

  [[nodiscard]] std::optional<ResolvedReference>
  resolve(const ReplayFileActionRequest &request,
          ReplayFileActionOutcome &outcome);
  [[nodiscard]] ReplayFileActionOutcome
  inspectResolved(const ResolvedReference &resolved) const;

  std::unique_ptr<ReplayFileStore> ownedStore_;
  ReplayRepository &repository_;
  ReplayFileStore &store_;
};

} // namespace replay
