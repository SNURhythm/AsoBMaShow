#pragma once

#include "../CourseIdentity.h"
#include "../ReplayData.h"
#include "../ResultPersistenceModel.h"
#include "ScoreRepository.h"
#include "../bms_parser.hpp"
#include "../sqlite3.h"

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
};

struct CourseReplayLookup {
  std::string courseKey;
  int legacyCourseId = 0;
};

class ReplayRepository {
public:
  static constexpr int kCurrentSchemaVersion = 5;

  ReplayRepository() = default;
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
  // Compatibility API for standalone migration and test helpers.
  sqlite3 *Connect();
  void Close(sqlite3 *db);
  bool CreateReplayTables(sqlite3 *db);
  std::optional<int> SaveReplay(const ReplayData &replay);
  std::optional<int> SaveCourseReplay(const CourseReplayData &replay);
  result_persistence::StageOutcome StageChartResult(
      const result_persistence::ChartResultAttempt &attempt);
  result_persistence::PendingReadOutcome
  LoadPendingChartScore(std::string_view attemptId);
  result_persistence::PendingBatchOutcome
  ListPendingChartScores(std::size_t limit = 256);
  result_persistence::AcknowledgeOutcome
  AcknowledgePendingChartScore(std::string_view attemptId, int replayId);
  result_persistence::RecoveryMarkOutcome
  RecordPendingChartScoreRecoveryAttempt(
      std::string_view attemptId,
      result_persistence::RecoveryAttemptKind kind);
  // Pass limit <= 0 to return all matching rows.
  std::vector<ReplaySummary> ListReplays(const bms_parser::ChartMeta &chartMeta,
                                         int limit = 100);
  std::vector<ReplaySummary>
  ListCourseReplays(const CourseReplayLookup &lookup, int limit = 100);
  std::optional<ReplayData> LoadReplay(int replayId,
                                       const bms_parser::ChartMeta &chartMeta);
  std::optional<CourseReplayData> LoadCourseReplay(int replayId);
  bool RecoverCourseRecords(
      std::span<const course_identity::Definition> definitions,
      std::span<const CourseScoreEvidence> scoreEvidence,
      std::string &errorMessage);
  // Standalone-helper compatibility path for caller-owned transactions.
  bool RecoverCourseRecords(
      sqlite3 *db,
      std::span<const course_identity::Definition> definitions,
      std::span<const CourseScoreEvidence> scoreEvidence,
      std::string &errorMessage);
  std::optional<ReplayData>
  LoadLatestReplay(const bms_parser::ChartMeta &chartMeta);

private:
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePathLocked() const;
  bool EnsureSessionDatabaseLocked();
  void ShutdownLocked();
  bool CreateReplayTablesOnConnection(sqlite3 *db);
  std::optional<int>
  SaveReplayOnConnection(sqlite3 *db, const ReplayData &replay,
                         const std::string &provenanceJson);
  std::optional<int> SaveCourseReplayOnConnection(
      sqlite3 *db, const CourseReplayData &replay,
      const std::string &courseProvenanceJson,
      const std::vector<std::string> &stageProvenanceJson);
  std::vector<ReplaySummary>
  ListReplaysOnConnection(sqlite3 *db,
                          const bms_parser::ChartMeta &chartMeta, int limit);
  std::vector<ReplaySummary>
  ListCourseReplaysOnConnection(sqlite3 *db,
                                const CourseReplayLookup &lookup, int limit);
  std::optional<ReplayData>
  LoadReplayOnConnection(sqlite3 *db, int replayId,
                         const bms_parser::ChartMeta &chartMeta);
  std::optional<CourseReplayData> LoadCourseReplayOnConnection(sqlite3 *db,
                                                               int replayId);
  bool RecoverCourseRecordsOnConnection(
      sqlite3 *db, std::span<const course_identity::Definition> definitions,
      std::span<const CourseScoreEvidence> scoreEvidence,
      std::string &errorMessage);
  std::optional<ReplayData>
  LoadLatestReplayOnConnection(sqlite3 *db,
                               const bms_parser::ChartMeta &chartMeta);

  mutable std::mutex sessionMutex_;
  std::filesystem::path databasePath_;
  sqlite3 *sessionDatabase_ = nullptr;
};
