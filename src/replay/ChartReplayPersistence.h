#pragma once

#include "BeatorajaReplayCodec.h"
#include "ChartReplayAgreement.h"
#include "ReplayFileAssociationCoordinator.h"

#include "../ModernResult.h"
#include "../ir/IrOutboxModels.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "../repositories/ReplayRepository.h"
#include "../repositories/ScoreRepository.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace replay {

struct ChartReplayPersistenceAttempt {
  result_persistence::ModernChartResult result;
  std::optional<ir::IrSubmissionSnapshot> irSnapshot;
  std::optional<ReplayChartDocument> replay;
};

enum class ChartReplayPersistenceState {
  SavedWithReplay,
  SavedWithoutReplay,
  PendingScore,
  PendingAcknowledgement,
  Retryable,
  InvalidAttempt,
  IntegrityConflict,
};

struct ChartReplayPersistenceOutcome {
  ChartReplayPersistenceState state = ChartReplayPersistenceState::Retryable;
  std::optional<ModernChartStageReceipt> receipt;
  bool replayAttached = false;
  std::string diagnostic;

  [[nodiscard]] bool saved() const noexcept {
    return state == ChartReplayPersistenceState::SavedWithReplay ||
           state == ChartReplayPersistenceState::SavedWithoutReplay;
  }
  [[nodiscard]] bool durable() const noexcept {
    return receipt.has_value() &&
           state != ChartReplayPersistenceState::Retryable &&
           state != ChartReplayPersistenceState::InvalidAttempt;
  }
  [[nodiscard]] bool retryable() const noexcept {
    return state == ChartReplayPersistenceState::Retryable ||
           state == ChartReplayPersistenceState::PendingScore ||
           state == ChartReplayPersistenceState::PendingAcknowledgement;
  }
};

struct ChartReplayRecoverySummary {
  std::size_t attempted = 0;
  std::size_t saved = 0;
  std::size_t pending = 0;
  std::size_t conflicts = 0;
  std::string diagnostic;
};

[[nodiscard]] inline constexpr std::string_view
chartReplayRecoveryUserMessage() noexcept {
  return "Some previously completed results are still waiting to be saved. "
         "They were kept safely and will be retried later.";
}

struct ChartReplayPersistenceDependencies {
  std::function<ModernChartResultReadOutcome(std::string_view)> loadResult;
  ReplayFileAssociationCoordinatorDependencies fileAssociation;
  std::function<std::optional<std::vector<std::byte>>(
      const ReplayChartDocument &, std::int64_t, std::string &)>
      encode;
  std::function<ModernChartStageOutcome(
      const result_persistence::ModernChartResult &,
      const std::optional<ir::IrSubmissionSnapshot> &,
      const std::optional<ModernReplayFileAttachment> &,
      std::span<const ir::IrOutboxDraft>)>
      stage;
  std::function<result_persistence::PendingReadOutcome(std::string_view)>
      loadPending;
  std::function<result_persistence::PendingBatchOutcome(std::size_t)>
      listPending;
  std::function<result_persistence::ProjectionOutcome(
      const result_persistence::PendingChartScoreWrite &)>
      project;
  std::function<result_persistence::AcknowledgeOutcome(std::string_view, int)>
      acknowledge;
  std::function<result_persistence::RecoveryMarkOutcome(
      std::string_view, result_persistence::RecoveryAttemptKind)>
      recordRecoveryAttempt;
};

class ChartReplayPersistence {
public:
  ChartReplayPersistence(ScoreRepository &score, ReplayRepository &repository);
  explicit ChartReplayPersistence(
      ChartReplayPersistenceDependencies dependencies);

  [[nodiscard]] ChartReplayPersistenceOutcome
  persist(const ChartReplayPersistenceAttempt &attempt,
          std::span<const ir::IrOutboxDraft> irDrafts = {});

  [[nodiscard]] ChartReplayRecoverySummary recoverAll(std::size_t limit = 256);

private:
  ChartReplayPersistenceDependencies dependencies_;
};

} // namespace replay
