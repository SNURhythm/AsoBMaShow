#pragma once

#include "CompletedAttempt.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/ReplayFileStore.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace result_persistence {

enum class SaveState {
  Saved,
  InvalidAttempt,
  UnfinalizedReplay,
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
  validatedReceiptFor(const CompletedChartAttempt &attempt) const noexcept;
};

struct SaveConflictDetails {
  std::string state;
  std::string reason;
  std::string attemptId;
  std::optional<int> resultId;
};

[[nodiscard]] std::optional<SaveConflictDetails>
saveConflictDetails(const SaveOutcome &outcome,
                    std::string_view attemptId = {});

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
  std::function<ReservationOutcome(std::string_view, std::string_view)> reserve;
  std::function<std::optional<std::vector<std::byte>>(
      const replay::ReplayPlaybackData &, std::int64_t, std::string &)>
      encodeReplay;
  std::function<std::optional<std::vector<std::byte>>(
      const replay::CourseReplayPlaybackData &, std::int64_t, std::string &)>
      encodeCourseReplay;
  std::function<replay::FinalizeOutcome(
      const replay::ReplayPathIdentity &, std::span<const std::byte>,
      const replay::ExpectedReplayIdentity &, std::string_view)>
      finalizeReplay;
  std::function<bool(const ReplayFileReservation &,
                     const replay::ReplayFileMetadata &, std::string &)>
      recordFinalizedReplay;
  std::function<StageOutcome(const PersistedChartResult &,
                             const ir::IrSubmissionSnapshot &,
                             const ReplayFileReference &,
                             std::span<const ir::IrOutboxDraft>)>
      stage;
  std::function<StageOutcome(const PersistedCourseResult &,
                             const ReplayFileReference &)>
      stageCourse;
  std::function<PendingReadOutcome(std::string_view)> loadPending;
  std::function<PendingBatchOutcome(std::size_t)> listPending;
  std::function<ProjectionOutcome(const PendingChartScoreWrite &)> project;
  std::function<AcknowledgeOutcome(std::string_view, int)>
      acknowledgeAndActivate;
  std::function<RecoveryMarkOutcome(std::string_view, RecoveryAttemptKind)>
      recordRecoveryAttempt;
};

class Coordinator {
public:
  Coordinator(ScoreRepository &score, ReplayRepository &replay);
  Coordinator(ScoreRepository &score, ReplayRepository &replay,
              replay::ReplayFileStore &fileStore,
              replay::BeatorajaReplayCodec &codec);
  explicit Coordinator(Dependencies dependencies);

  SaveOutcome persist(const CompletedChartAttempt &attempt,
                      std::span<const ir::IrOutboxDraft> irDrafts = {});
  SaveOutcome persistCourse(const CompletedCourseAttempt &attempt);
  RecoverySummary recoverAll(std::size_t limit = 256);

private:
  Dependencies dependencies_;
};

} // namespace result_persistence
