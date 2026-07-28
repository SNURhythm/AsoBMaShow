#pragma once

#define ASOBMASHOW_HAS_MODERN_COURSE_REPOSITORY 1

#include "../CourseIdentity.h"
#include "../ModernResult.h"
#include "../ReplayData.h"
#include "../ResultPersistenceModel.h"
#include "../ir/IrOutboxModels.h"
#include "../ir/IrSubmissionSnapshot.h"
#include "../ir/IrUploadCandidates.h"
#include "../replay/BeatorajaReplayCodec.h"
#include "../replay/BeatorajaReplayPath.h"
#include "../replay/ReplayFileStore.h"
#include "../ir/IrRemoteScoreModels.h"
#include "../ir/IrScoreReconciliation.h"
#include "ScoreRepositoryModels.h"
#include "LegacyResultSummary.h"
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
  int modernResultId = 0;
  std::string createdAt;
  ChartScoreWrite score;

  [[nodiscard]] bool hasExactlyOneOwner() const noexcept {
    return (replayId > 0) != (modernResultId > 0);
  }

  [[nodiscard]] int ownerId() const noexcept {
    return replayId > 0 ? replayId : modernResultId;
  }
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
  std::string requestedIrOutboxDiagnostic;
  bool irSubmissionEligible = false;
  bool hasIrReceipt = false;
  std::string receiptProviderId;
  std::string receiptServerOrigin;
  std::string receiptRemoteScoreId;
  ir::IrRecordState irRecordState = ir::IrRecordState::Hidden;
};

struct CourseReplayLookup {
  std::string courseKey;
  int legacyCourseId = 0;
};

struct ModernReplayPathReservation {
  std::string attemptId;
  replay::ReplayPathIdentity identity;
  std::int64_t createdAtUnixMillis = 0;

  bool operator==(const ModernReplayPathReservation &) const = default;
};

enum class ModernReplayReservationStatus {
  Reserved,
  AlreadyReserved,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernReplayReservationOutcome {
  ModernReplayReservationStatus status =
      ModernReplayReservationStatus::StorageFailure;
  std::optional<ModernReplayPathReservation> reservation;
  std::string diagnostic;
};

enum class ModernReplayReservationReleaseStatus {
  Released,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernReplayReservationReleaseOutcome {
  ModernReplayReservationReleaseStatus status =
      ModernReplayReservationReleaseStatus::StorageFailure;
  std::string diagnostic;
};

struct ModernReplayFileAttachment {
  replay::ReplayPathIdentity identity;
  replay::ReplayFileMetadata metadata;

  bool operator==(const ModernReplayFileAttachment &) const = default;
};

struct ModernReplayFileReference {
  std::int64_t id = 0;
  int resultId = 0;
  bool userDeleted = false;
  replay::ReplayPathIdentity identity;
  replay::ReplayFileMetadata metadata;

  bool operator==(const ModernReplayFileReference &) const = default;
};

enum class ModernReplayOwnerKind { ChartResult, CourseResult };

enum class ModernReplayFileMutationStatus {
  Changed,
  AlreadyChanged,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernReplayFileMutationOutcome {
  ModernReplayFileMutationStatus status =
      ModernReplayFileMutationStatus::StorageFailure;
  std::string diagnostic;
};

struct ModernReplayFileInventoryEntry {
  ModernReplayOwnerKind owner = ModernReplayOwnerKind::ChartResult;
  std::string attemptId;
  ModernReplayFileReference reference;

  bool operator==(const ModernReplayFileInventoryEntry &) const = default;
};

enum class ModernReplayFileInventoryStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernReplayPathReservationInventoryOutcome {
  ModernReplayFileInventoryStatus status =
      ModernReplayFileInventoryStatus::StorageFailure;
  std::vector<ModernReplayPathReservation> reservations;
  std::string diagnostic;
};

struct ModernReplayFileInventoryOutcome {
  ModernReplayFileInventoryStatus status =
      ModernReplayFileInventoryStatus::StorageFailure;
  std::vector<ModernReplayFileInventoryEntry> entries;
  std::string diagnostic;
};

struct ModernChartResultRecord {
  result_persistence::ModernChartResult result;
  std::optional<ModernReplayFileReference> replayFile;

  bool operator==(const ModernChartResultRecord &) const = default;
};

enum class ModernChartStageStatus {
  Staged,
  AlreadyStaged,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernChartStageReceipt {
  std::string attemptId;
  int resultId = 0;
  std::string createdAt;

  bool operator==(const ModernChartStageReceipt &) const = default;
};

struct ModernChartStageOutcome {
  ModernChartStageStatus status = ModernChartStageStatus::StorageFailure;
  std::optional<ModernChartStageReceipt> receipt;
  std::string diagnostic;
};

enum class ModernChartResultReadStatus {
  Loaded,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernChartResultReadOutcome {
  ModernChartResultReadStatus status =
      ModernChartResultReadStatus::StorageFailure;
  std::optional<ModernChartResultRecord> record;
  std::string diagnostic;
};

struct ModernCourseResultRecord {
  result_persistence::ModernCourseResult result;
  std::optional<ModernReplayFileReference> replayFile;

  bool operator==(const ModernCourseResultRecord &) const = default;
};

enum class ModernCourseStageStatus {
  Staged,
  AlreadyStaged,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernCourseStageReceipt {
  std::string attemptId;
  int resultId = 0;
  std::string createdAt;

  bool operator==(const ModernCourseStageReceipt &) const = default;
};

struct ModernCourseStageOutcome {
  ModernCourseStageStatus status = ModernCourseStageStatus::StorageFailure;
  std::optional<ModernCourseStageReceipt> receipt;
  std::string diagnostic;
};

enum class ModernCourseResultReadStatus {
  Loaded,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernCourseResultReadOutcome {
  ModernCourseResultReadStatus status =
      ModernCourseResultReadStatus::StorageFailure;
  std::optional<ModernCourseResultRecord> record;
  std::string diagnostic;
};

enum class ModernCourseHistoryReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernCourseHistoryReadOutcome {
  ModernCourseHistoryReadStatus status =
      ModernCourseHistoryReadStatus::StorageFailure;
  std::vector<ModernCourseResultRecord> records;
  std::string diagnostic;
};

// A result-only source for idempotent score projection recovery. Replay file
// ownership is intentionally absent: a missing, deleted, or corrupt BRD cannot
// prevent durable course facts from restoring their score projection.
struct ModernCourseScoreSource {
  int resultId = 0;
  std::string createdAt;
  result_persistence::ModernCourseResult result;

  bool operator==(const ModernCourseScoreSource &) const = default;
};

enum class ModernCourseScoreSourceEntryStatus { Loaded, IntegrityConflict };

struct ModernCourseScoreSourceEntry {
  ModernCourseScoreSourceEntryStatus status =
      ModernCourseScoreSourceEntryStatus::IntegrityConflict;
  int resultId = 0;
  std::optional<ModernCourseScoreSource> source;
  std::string diagnostic;
};

enum class ModernCourseScoreSourceBatchStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernCourseScoreSourceBatchOutcome {
  ModernCourseScoreSourceBatchStatus status =
      ModernCourseScoreSourceBatchStatus::StorageFailure;
  std::vector<ModernCourseScoreSourceEntry> entries;
  bool hasMore = false;
  std::string diagnostic;
};

inline constexpr std::size_t kMaximumModernChartHistoryRows = 1024;
inline constexpr std::size_t kDefaultModernChartHistoryRows = 100;
inline constexpr std::size_t kMaximumModernCourseHistoryRows = 1024;
inline constexpr std::size_t kMaximumModernCourseScoreSourceRows = 256;

enum class ModernChartHistoryReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernChartHistoryReadOutcome {
  ModernChartHistoryReadStatus status =
      ModernChartHistoryReadStatus::StorageFailure;
  std::vector<ModernChartResultRecord> records;
  std::string diagnostic;
};

enum class ModernIrSnapshotReadStatus {
  Loaded,
  NotFound,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct ModernIrSnapshotReadOutcome {
  ModernIrSnapshotReadStatus status =
      ModernIrSnapshotReadStatus::StorageFailure;
  std::optional<ir::IrSubmissionSnapshot> snapshot;
  std::string diagnostic;
};

class ReplayRepository {
public:
  static constexpr int kCurrentSchemaVersion = 15;

  ReplayRepository();
  explicit ReplayRepository(std::filesystem::path databasePath);
  ~ReplayRepository();
  ReplayRepository(const ReplayRepository &) = delete;
  ReplayRepository &operator=(const ReplayRepository &) = delete;

  void SetDatabasePath(std::filesystem::path databasePath);
  [[nodiscard]] std::filesystem::path GetDatabasePath() const;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePath() const;
  [[nodiscard]] std::filesystem::path GetResolvedProfileRoot() const;
  bool BindDatabasePath(std::filesystem::path databasePath,
                        std::string &errorMessage);
  [[nodiscard]] static bool HasActiveReads();
  [[nodiscard]] static bool HasActiveWrites();
  void Shutdown();
  bool EnsureSchema();
  ModernReplayReservationOutcome
  ReserveModernReplayPath(std::string_view attemptId, std::string_view stem,
                          std::int64_t createdAtUnixMillis);
  ModernReplayReservationReleaseOutcome ReleaseModernReplayPathReservation(
      const ModernReplayPathReservation &reservation);
  ModernReplayPathReservationInventoryOutcome
  ListModernReplayPathReservations();
  ModernChartStageOutcome StageModernChartResult(
      const result_persistence::ModernChartResult &result,
      const std::optional<ir::IrSubmissionSnapshot> &snapshot,
      const std::optional<ModernReplayFileAttachment> &replayFile,
      std::span<const ir::IrOutboxDraft> irDrafts = {});
  ModernChartResultReadOutcome
  LoadModernChartResultByAttempt(std::string_view attemptId);
  ModernChartResultReadOutcome LoadModernChartResult(int resultId);
  ModernChartHistoryReadOutcome
  ListModernChartResults(
      std::string_view chartSha256,
      std::size_t limit = kDefaultModernChartHistoryRows);
  std::vector<LegacyChartResultSummary>
  ListLegacyChartSummaries(const bms_parser::ChartMeta &chartMeta,
                           std::size_t limit = 100);
  ModernCourseStageOutcome StageModernCourseResult(
      const result_persistence::ModernCourseResult &result,
      const std::optional<ModernReplayFileAttachment> &replayFile,
      const std::optional<replay::CoursePathInput> &replayPath = std::nullopt);
  ModernCourseResultReadOutcome
  LoadModernCourseResultByAttempt(std::string_view attemptId);
  ModernCourseResultReadOutcome LoadModernCourseResult(int resultId);
  ModernCourseHistoryReadOutcome
  ListModernCourseResults(std::string_view courseKey, std::size_t limit = 100);
  ModernCourseScoreSourceBatchOutcome
  ListModernCourseScoreSourcesAfter(int afterResultId, std::size_t limit = 256);
  result_persistence::AcknowledgeOutcome
  AcknowledgePendingModernCourseScore(std::string_view attemptId,
                                      int modernResultId);
  std::vector<LegacyCourseResultSummary>
  ListLegacyCourseSummaries(const CourseReplayLookup &lookup,
                            std::size_t limit = 100);
  ModernReplayFileMutationOutcome
  MarkModernReplayFileUserDeleted(ModernReplayOwnerKind owner,
                                  std::string_view attemptId,
                                  const ModernReplayFileReference &expected);
  ModernReplayFileInventoryOutcome ListModernReplayFileReferences();
  ModernReplayFileInventoryOutcome
  ListUserDeletedModernReplayFileReferences();
  ModernIrSnapshotReadOutcome
  LoadModernIrSubmissionSnapshot(std::string_view attemptId);
  result_persistence::PendingReadOutcome
  LoadPendingModernChartScore(std::string_view attemptId);
  result_persistence::PendingBatchOutcome
  ListPendingModernChartScores(std::size_t limit = 256);
  result_persistence::AcknowledgeOutcome
  AcknowledgePendingModernChartScore(std::string_view attemptId,
                                     int modernResultId);
  result_persistence::RecoveryMarkOutcome
  RecordPendingModernChartScoreRecoveryAttempt(
      std::string_view attemptId, result_persistence::RecoveryAttemptKind kind);
  ir::IrOutboxInsertOutcome
  EnqueueReadyIrOutboxDraft(const ir::IrOutboxDraft &draft, bool userIntent);
  ir::IrManualBatchEnqueueOutcome
  EnqueueReadyIrOutboxDrafts(std::span<const ir::IrOutboxDraft> drafts,
                             std::string_view requestOrigin, bool userIntent,
                             std::int64_t nowMs);
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
  ir::IrOutboxMutationOutcome
  ApplyIrOutboxDeliveries(std::span<const ir::IrOutboxDeliveryUpdate> updates);
  ir::IrReceiptReadOutcome
  LoadIrSubmissionReceipt(std::string_view providerId,
                          std::string_view serverOrigin,
                          std::string_view attemptId);
  ir::IrOutboxMutationOutcome
  ClearIrSubmissionReceipts(std::string_view providerId,
                            std::string_view serverOrigin);
  ir::IrReconciliationReadOutcome
  LoadIrReconciliationCandidates(std::string_view providerId,
                                 std::string_view serverOrigin,
                                 std::optional<int> beforeModernChartResultId =
                                     std::nullopt,
                                 std::size_t limit =
                                     ir::kDefaultIrUploadSourcePageRows);
  ir::IrUploadRecordReadOutcome
  ListIrUploadRecords(std::string_view providerId,
                      std::string_view serverOrigin,
                      std::optional<int> beforeModernChartResultId =
                          std::nullopt,
                      std::size_t limit =
                          ir::kDefaultIrUploadSourcePageRows);
  ir::IrUploadRecordReadOutcome ListIrUploadRecordsForChart(
      std::string_view providerId, std::string_view serverOrigin,
      std::string_view chartSha256,
      std::size_t limit = kDefaultModernChartHistoryRows);
  ir::IrUploadCandidateReadOutcome
  ListIrUploadCandidates(std::string_view providerId,
                         std::string_view serverOrigin,
                         std::optional<int> beforeModernChartResultId =
                             std::nullopt,
                         std::size_t limit =
                             ir::kDefaultIrUploadSourcePageRows);
  ir::IrRemoteSnapshotApplyOutcome
  ApplyIrRemoteSnapshot(const ir::IrRemoteSnapshotMutation &mutation);
  // Record sync uses these two phases so score projection can complete after
  // the mirror is durable but before receipts and outbox work are settled.
  ir::IrRemoteSnapshotApplyOutcome
  ReplaceIrRemoteScoreMirror(const ir::IrRemoteSnapshotMutation &mutation);
  ir::IrRemoteSnapshotApplyOutcome
  FinalizeIrRemoteSnapshot(const ir::IrRemoteSnapshotMutation &mutation,
                           std::int64_t expectedSyncGeneration);
  ir::IrRemoteScoreReadOutcome
  ListIrRemoteScores(std::string_view providerId,
                     std::string_view serverOrigin);
  ir::IrRemoteScoreMirrorStateOutcome
  LoadIrRemoteScoreMirrorState(std::string_view providerId,
                               std::string_view serverOrigin);
  ir::IrRemoteScoreReadOutcome ListIrRemoteScoresForChart(
      std::string_view providerId, std::string_view serverOrigin,
      std::string_view chartMd5, std::string_view chartSha256);
  ir::IrRemoteScoreLookupOutcome
  LoadIrRemoteScore(std::string_view providerId, std::string_view serverOrigin,
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
  ClearIrAccountDataSnapshot(const std::filesystem::path &snapshotDatabasePath,
                             std::string &errorMessage);
private:
  struct Impl;
  ModernReplayFileInventoryOutcome
  ListModernReplayFileReferences(bool userDeletedOnly);
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePathLocked() const;
  bool EnsureSessionDatabaseLocked();
  void ShutdownLocked();
  std::unique_ptr<Impl> impl_;
};
