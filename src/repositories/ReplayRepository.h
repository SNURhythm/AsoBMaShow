#pragma once

#include "../CourseIdentity.h"
#include "../ReplayData.h"
#include "../ResultPersistenceModel.h"
#include "../ir/IrOutboxModels.h"
#include "../ir/IrRemoteScoreModels.h"
#include "../ir/IrScoreReconciliation.h"
#include "ScoreRepositoryModels.h"
#include "../bms_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ir {

struct IrRemoteSnapshotMutation {
  std::string providerId;
  std::string serverOrigin;
  std::int64_t synchronizedAtUnixMillis = 0;
  std::vector<IrRemoteScore> scores;
  std::vector<IrSubmissionReceipt> upsertedReceipts;
  std::vector<std::int64_t> deletedReceiptIds;
  std::vector<std::int64_t> settledOutboxRowIds;
  std::vector<std::int64_t> purgedSucceededOutboxRowIds;
};

struct IrRemoteSnapshotApplyOutcome {
  enum class Status { Applied, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  int remoteScoreCount = 0;
  int remoteScoresAdded = 0;
  int remoteScoresRemoved = 0;
  int receiptsUpserted = 0;
  int receiptsDeleted = 0;
  int outboxRowsSettled = 0;
  int ambiguousReceiptsPreserved = 0;
  std::int64_t syncGeneration = 0;
  std::string diagnostic;
};

struct IrRemoteScoreMirrorStateOutcome {
  enum class Status { Loaded, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  std::int64_t syncGeneration = 0;
  std::size_t scoreCount = 0;
  std::string diagnostic;
};

struct IrRemoteScoreReadOutcome {
  enum class Status { Loaded, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  std::vector<IrRemoteScore> scores;
  std::string diagnostic;
};

// Records reads are intentionally smaller than the complete account mirror.
// Exceeding this per-chart bound is an invalid projection rather than a
// silently truncated result list.
inline constexpr std::size_t kMaximumIrRemoteScoresPerChart = 512;

struct IrRemoteScoreLookupOutcome {
  enum class Status { Loaded, NotFound, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  std::optional<IrRemoteScore> score;
  std::string diagnostic;
};

} // namespace ir

namespace result_persistence {

enum class StageStatus {
  Staged,
  AlreadyStaged,
  StorageFailure,
  IntegrityConflict,
};

struct StageOutcome {
  StageStatus status = StageStatus::StorageFailure;
  std::optional<StageReceipt> receipt;
  std::string diagnostic;
};

struct PendingChartScoreWrite {
  std::string attemptId;
  int replayId = 0;
  std::string createdAt;
  ChartScoreWrite score;
};

enum class PendingReadStatus {
  Found,
  NotFound,
  StorageFailure,
  IntegrityConflict,
};

struct PendingReadOutcome {
  PendingReadStatus status = PendingReadStatus::StorageFailure;
  std::optional<PendingChartScoreWrite> value;
  std::string diagnostic;
};

struct PendingBatchEntry {
  PendingReadStatus status = PendingReadStatus::IntegrityConflict;
  std::string attemptId;
  std::optional<PendingChartScoreWrite> value;
  std::string diagnostic;
};

struct PendingBatchOutcome {
  bool storageAvailable = false;
  std::vector<PendingBatchEntry> entries;
  std::size_t remaining = 0;
  std::string diagnostic;
};

enum class RecoveryAttemptKind { StorageFailure, IntegrityConflict };

enum class RecoveryMarkStatus { Recorded, NotFound, StorageFailure };

struct RecoveryMarkOutcome {
  RecoveryMarkStatus status = RecoveryMarkStatus::StorageFailure;
  std::string diagnostic;
};

enum class AcknowledgeStatus {
  Acknowledged,
  AlreadyAcknowledged,
  StorageFailure,
  IntegrityConflict,
};

struct AcknowledgeOutcome {
  AcknowledgeStatus status = AcknowledgeStatus::StorageFailure;
  std::string diagnostic;
};

} // namespace result_persistence

namespace replay_summary_scan {
// Positive-limit summary reads inspect at most the requested rows plus this
// corruption allowance. If the budget is exhausted, the API fails closed and
// returns fewer rows with one aggregate diagnostic. limit <= 0 remains the
// explicit unbounded/all-valid-rows mode.
inline constexpr int kChunkSize = 64;
inline constexpr int kCorruptCandidateAllowance = 512;
inline constexpr int kMaxCourseStagesPerCandidate = 256;
} // namespace replay_summary_scan

struct ReplaySummary {
  int id = 0;
  bool courseReplay = false;
  bool autoPlay = false;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  int finalScore = 0;
  int maxScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
  int eventCount = 0;
  int touchSampleCount = 0;
  std::optional<bms_parser::ChartMeta> chartMeta;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  int completedCharts = 0;
  int totalCharts = 0;
  int stageCount = 0;
  int rulesetVersion = 0;
  ScoreEligibility eligibility = ScoreEligibility::LegacyUnverified;
  audio::PlaybackRate playback;
  std::shared_ptr<const ScoreProvenance> provenance;
  std::optional<std::string> attemptId;
  bool hasCanonicalAttemptFingerprint = false;
  std::optional<ir::IrOutboxState> requestedIrOutboxState;
  bool irSubmissionEligible = false;
  bool hasIrReceipt = false;
  std::string receiptProviderId;
  std::string receiptServerOrigin;
  std::string receiptRemoteScoreId;
  ir::IrRecordState irRecordState = ir::IrRecordState::Hidden;
};

enum class IrUploadReplayReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrUploadReplayReadOutcome {
  IrUploadReplayReadStatus status = IrUploadReplayReadStatus::StorageFailure;
  std::vector<ReplaySummary> replays;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

inline constexpr std::size_t kMaximumIrUploadCandidateRows = 16384;

struct ReplayResultRecord {
  ReplayData replay;
  std::optional<std::string> attemptId;
  std::optional<std::string> attemptFingerprint;
  std::int64_t playedAtUnixMillis = 0;
};

struct CourseReplayLookup {
  std::string courseKey;
  int legacyCourseId = 0;
};

class ReplayRepository {
public:
  static constexpr int kCurrentSchemaVersion = 10;

  ReplayRepository();
  explicit ReplayRepository(std::filesystem::path databasePath);
  ~ReplayRepository();
  ReplayRepository(const ReplayRepository &) = delete;
  ReplayRepository &operator=(const ReplayRepository &) = delete;

  void SetDatabasePath(std::filesystem::path databasePath);
  [[nodiscard]] std::filesystem::path GetDatabasePath() const;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePath() const;
  bool BindDatabasePath(std::filesystem::path databasePath,
                        std::string &errorMessage);
  [[nodiscard]] static bool HasActiveReads();
  [[nodiscard]] static bool HasActiveWrites();
  void Shutdown();
  bool EnsureSchema();
  std::optional<int> SaveReplay(const ReplayData &replay);
  std::optional<int> SaveCourseReplay(const CourseReplayData &replay);
  result_persistence::StageOutcome
  StageChartResult(const result_persistence::ChartResultAttempt &attempt,
                   std::span<const ir::IrOutboxDraft> irDrafts);
  result_persistence::PendingReadOutcome
  LoadPendingChartScore(std::string_view attemptId);
  result_persistence::PendingBatchOutcome
  ListPendingChartScores(std::size_t limit = 256);
  result_persistence::AcknowledgeOutcome
  AcknowledgePendingChartScoreAndActivateIr(std::string_view attemptId,
                                            int replayId);
  result_persistence::RecoveryMarkOutcome
  RecordPendingChartScoreRecoveryAttempt(
      std::string_view attemptId, result_persistence::RecoveryAttemptKind kind);
  ir::IrOutboxInsertOutcome
  EnqueueReadyIrOutboxDraft(const ir::IrOutboxDraft &draft, bool userIntent);
  ir::IrOutboxReadOutcome LoadIrOutbox(std::string_view providerId,
                                       std::string_view attemptId);
  ir::IrOutboxBatchOutcome ListDueIrOutbox(std::int64_t nowMs,
                                           std::size_t limit = 64);
  ir::IrOutboxBatchOutcome ListDueIrOutbox(std::string_view providerId,
                                           std::int64_t nowMs,
                                           std::size_t limit = 64);
  std::optional<std::int64_t>
  NextIrOutboxAttemptAfter(std::string_view providerId, std::int64_t nowMs);
  ir::IrOutboxBatchOutcome ListIrOutbox(std::size_t limit = 1024);
  ir::IrOutboxClaimOutcome ClaimIrOutbox(std::int64_t rowId,
                                         ir::IrOutboxState expectedState,
                                         std::int64_t nowMs);
  ir::IrOutboxBatchClaimOutcome
  ClaimIrOutboxBatch(std::span<const ir::IrOutboxClaimRequest> requests,
                     std::int64_t nowMs);
  ir::IrOutboxMutationOutcome
  BlockIrOutboxConfiguration(std::int64_t rowId,
                             ir::IrOutboxState expectedState,
                             std::string_view errorCode,
                             std::string_view errorMessage, std::int64_t nowMs);
  ir::IrOutboxMutationOutcome
  ApplyIrOutboxDelivery(const ir::IrOutboxDeliveryUpdate &update);
  ir::IrOutboxMutationOutcome ApplyIrOutboxDeliveries(
      std::span<const ir::IrOutboxDeliveryUpdate> updates);
  ir::IrReceiptReadOutcome
  LoadIrSubmissionReceipt(std::string_view providerId,
                          std::string_view serverOrigin,
                          std::string_view attemptId);
  ir::IrOutboxMutationOutcome
  ClearIrSubmissionReceipts(std::string_view providerId,
                            std::string_view serverOrigin);
  ir::IrReconciliationReadOutcome
  LoadIrReconciliationCandidates(std::string_view providerId,
                                 std::string_view serverOrigin);
  IrUploadReplayReadOutcome
  ListIrUploadCandidateReplays(std::string_view providerId,
                               std::string_view serverOrigin);
  ir::IrRemoteSnapshotApplyOutcome
  ApplyIrRemoteSnapshot(const ir::IrRemoteSnapshotMutation &mutation);
  ir::IrRemoteScoreReadOutcome
  ListIrRemoteScores(std::string_view providerId,
                     std::string_view serverOrigin);
  ir::IrRemoteScoreMirrorStateOutcome LoadIrRemoteScoreMirrorState(
      std::string_view providerId, std::string_view serverOrigin);
  ir::IrRemoteScoreReadOutcome ListIrRemoteScoresForChart(
      std::string_view providerId, std::string_view serverOrigin,
      std::string_view chartMd5, std::string_view chartSha256);
  ir::IrRemoteScoreLookupOutcome
  LoadIrRemoteScore(std::string_view providerId,
                    std::string_view serverOrigin,
                    std::string_view remoteScoreId);
  ir::IrOutboxMutationOutcome
  ClearIrRemoteScores(std::string_view providerId,
                      std::string_view serverOrigin);
  ir::IrOutboxMutationOutcome
  ClearIrAccountEvidence(std::string_view providerId,
                         std::string_view serverOrigin);
  ir::IrOutboxMutationOutcome
  ClearIrProviderAccountEvidence(std::string_view providerId);
  ir::IrOutboxMutationOutcome RetryIrOutbox(std::int64_t rowId,
                                            std::int64_t nowMs);
  ir::IrOutboxMutationOutcome RetryAllIrOutbox(std::string_view providerId,
                                               std::int64_t nowMs);
  ir::IrOutboxMutationOutcome UnblockIrOutbox(std::string_view providerId,
                                              std::int64_t nowMs);
  ir::IrOutboxMutationOutcome DiscardIrOutbox(std::int64_t rowId);
  ir::IrOutboxCounts CountIrOutbox(std::string_view providerId);
  ir::IrOutboxMutationOutcome RecoverStaleIrOutbox(std::int64_t nowMs);
  ir::IrOutboxMutationOutcome PurgeSucceededIrOutbox(std::int64_t olderThanMs);
  bool ClearIrOutbox(std::string &errorMessage);
  static bool
  ClearIrOutboxSnapshot(const std::filesystem::path &snapshotDatabasePath,
                        std::string &errorMessage);
  // Pass limit <= 0 to return all matching rows.
  std::vector<ReplaySummary>
  ListReplays(const bms_parser::ChartMeta &chartMeta, int limit = 100,
              std::string_view irProviderId = {},
              std::string_view irServerOrigin = {});
  std::vector<ReplaySummary> ListCourseReplays(const CourseReplayLookup &lookup,
                                               int limit = 100);
  std::optional<ReplayData> LoadReplay(int replayId,
                                       const bms_parser::ChartMeta &chartMeta);
  std::optional<ReplayResultRecord>
  LoadReplayResult(int replayId, const bms_parser::ChartMeta &chartMeta);
  std::optional<CourseReplayData> LoadCourseReplay(int replayId);
  bool
  RecoverCourseRecords(std::span<const course_identity::Definition> definitions,
                       std::span<const CourseScoreEvidence> scoreEvidence,
                       std::string &errorMessage);
  std::optional<ReplayData>
  LoadLatestReplay(const bms_parser::ChartMeta &chartMeta);

private:
  struct Impl;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePathLocked() const;
  bool EnsureSessionDatabaseLocked();
  void ShutdownLocked();
  std::unique_ptr<Impl> impl_;
};
