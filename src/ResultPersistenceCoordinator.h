#pragma once

#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace result_persistence {

enum class SaveState {
  Saved,
  InvalidAttempt,
  Unstaged,
  PendingScore,
  PendingAcknowledgement,
  UnstagedConflict,
  PendingConflict,
};

[[nodiscard]] std::string_view saveStateUserMessage(SaveState state) noexcept;

struct SaveOutcome {
  SaveState state = SaveState::Unstaged;
  std::optional<StageReceipt> receipt;
  std::string userMessage;
  std::string diagnostic;

  [[nodiscard]] bool saved() const noexcept {
    return state == SaveState::Saved;
  }
  [[nodiscard]] bool durable() const noexcept;
  [[nodiscard]] bool retryable() const noexcept;
  [[nodiscard]] bool requiresUserDecision(bool attemptAvailable,
                                          bool continueChosen) const noexcept;
  [[nodiscard]] const StageReceipt *
  validatedReceiptFor(const ChartResultAttempt &attempt) const noexcept;
};

struct RecoverySummary {
  std::size_t attempted = 0;
  std::size_t saved = 0;
  std::size_t pending = 0;
  std::size_t conflicts = 0;
  std::string userMessage;
  std::string diagnostic;
};

[[nodiscard]] std::string_view recoveryUserMessage() noexcept;
[[nodiscard]] RecoverySummary recoveryFailureSummary(std::string diagnostic);

struct Dependencies {
  std::function<StageOutcome(const ChartResultAttempt &)> stage;
  std::function<PendingReadOutcome(std::string_view)> loadPending;
  std::function<PendingBatchOutcome(std::size_t)> listPending;
  std::function<ProjectionOutcome(const PendingChartScoreWrite &)> project;
  std::function<AcknowledgeOutcome(std::string_view, int)> acknowledge;
  std::function<RecoveryMarkOutcome(std::string_view, RecoveryAttemptKind)>
      recordRecoveryAttempt;
};

class Coordinator {
public:
  Coordinator(ScoreDBHelper &score, ReplayDBHelper &replay);
  explicit Coordinator(Dependencies dependencies);

  SaveOutcome persist(const ChartResultAttempt &attempt);
  RecoverySummary recoverAll(std::size_t limit = 256);

private:
  Dependencies dependencies_;
};

} // namespace result_persistence
