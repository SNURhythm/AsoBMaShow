#pragma once

#include "../CourseIdentity.h"
#include "../ScoreProvenance.h"
#include "ChartRepository.h"
#include "ScoreRepositoryModels.h"
#include "../sqlite3.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct CoursePlaySession;

namespace result_persistence {
struct PendingChartScoreWrite;

enum class ProjectionStatus {
  Inserted,
  AlreadyPresent,
  StorageFailure,
  IntegrityConflict,
};

struct ProjectionOutcome {
  ProjectionStatus status = ProjectionStatus::StorageFailure;
  std::string diagnostic;
};
} // namespace result_persistence

class ScoreRepository {
public:
  static constexpr int kCurrentSchemaVersion = 9;

  class [[nodiscard]] PreparedScoreQueryDatabase {
  public:
    PreparedScoreQueryDatabase(const ScoreRepository &,
                               ChartRepository::Session &);
    // Transitional native-handle constructor used by chart query internals.
    PreparedScoreQueryDatabase(const ScoreRepository &, sqlite3 *chartDatabase);
    ~PreparedScoreQueryDatabase();
    PreparedScoreQueryDatabase(PreparedScoreQueryDatabase &&) = delete;
    PreparedScoreQueryDatabase &
    operator=(PreparedScoreQueryDatabase &&) noexcept = delete;

    PreparedScoreQueryDatabase(const PreparedScoreQueryDatabase &) = delete;
    PreparedScoreQueryDatabase &
    operator=(const PreparedScoreQueryDatabase &) = delete;

    [[nodiscard]] const std::optional<std::string> &error() const;

  private:
    struct State;

    std::unique_ptr<State> state_;
  };

  ScoreRepository();
  explicit ScoreRepository(std::filesystem::path databasePath);
  ~ScoreRepository();
  ScoreRepository(const ScoreRepository &) = delete;
  ScoreRepository &operator=(const ScoreRepository &) = delete;

  void SetDatabasePath(std::filesystem::path databasePath);
  void SetChartDatabasePath(std::filesystem::path chartDatabasePath);
  [[nodiscard]] std::filesystem::path GetDatabasePath() const;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePath() const;
  [[nodiscard]] PreparedScoreQueryDatabase
  PrepareScoreQueryDatabase(ChartRepository::Session &chartSession) const;
  [[nodiscard]] PreparedScoreQueryDatabase
  PrepareScoreQueryDatabase(sqlite3 *chartDatabase) const;
  bool BindDatabasePath(std::filesystem::path databasePath,
                        std::string &errorMessage);
  [[nodiscard]] static bool HasActiveReads();
  [[nodiscard]] static bool HasActiveWrites();
  bool EnsureSchema();
  void Shutdown();
  // Compatibility API for standalone migration and test helpers.
  sqlite3 *Connect();
  void Close(sqlite3 *db);
  bool CreateScoreTable(sqlite3 *db);
  bool
  InsertScore(sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
              const RhythmState &state,
              const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  bool SaveScore(const bms_parser::ChartMeta &chartMeta,
                 const RhythmState &state,
                 const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  result_persistence::ProjectionOutcome
  SaveProjectedScore(const result_persistence::PendingChartScoreWrite &pending);
  bool CreateCourseScoreTable(sqlite3 *db);
  bool InsertCourseScore(
      sqlite3 *db, const CoursePlaySession &session, const RhythmState &state,
      int completedCharts, int totalCharts,
      const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  bool SaveCourseScore(
      const CoursePlaySession &session, const RhythmState &state,
      int completedCharts, int totalCharts,
      const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  std::optional<ScoreBestSnapshot> LoadBestScore(
      const bms_parser::ChartMeta &chartMeta,
      const std::optional<std::string> &beforeCreatedAt = std::nullopt,
      const std::optional<std::string> &excludeAttemptId = std::nullopt);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScore(const CoursePlaySession &session);
  CourseScoreRecoveryResult RecoverCourseRecords(
      std::span<const course_identity::Definition> definitions);
  ScoreClearRankCache LoadBestClearRanks();
  ScoreClearRankCache LoadBestClearRanks(ChartRepository::Session &chartSession,
                                         std::string_view schema);
  ScoreClearRankCache LoadBestClearRanks(sqlite3 *db, std::string_view schema);
  ScoreBestCache LoadBestScores();
  ScoreBestCache LoadBestScores(ChartRepository::Session &chartSession,
                                std::string_view schema);
  ScoreBestCache LoadBestScores(sqlite3 *db, std::string_view schema);
  [[nodiscard]] std::uint64_t GetRevision() const;

private:
  bool EnsureSchema(sqlite3 *db);
  bool CreateScoreTableOnConnection(sqlite3 *db);
  bool CreateCourseScoreTableOnConnection(sqlite3 *db);
  bool EnsureSchemaOnConnection(sqlite3 *db);
  bool InsertCourseScoreOnConnection(sqlite3 *db,
                                     const CoursePlaySession &session,
                                     const RhythmState &state,
                                     int completedCharts, int totalCharts,
                                     const ScoreProvenance &provenance,
                                     const std::string &provenanceJson);
  std::optional<ScoreBestSnapshot>
  LoadBestScoreOnConnection(sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
                            const std::optional<std::string> &beforeCreatedAt,
                            const std::optional<std::string> &excludeAttemptId);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScoreOnConnection(sqlite3 *db,
                                  const CoursePlaySession &session);
  CourseScoreRecoveryResult RecoverCourseRecordsOnConnection(
      sqlite3 *db, std::span<const course_identity::Definition> definitions);
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePathLocked() const;
  sqlite3 *EnsureSessionDatabaseLocked();
  void CloseSessionDatabaseLocked();

  mutable std::mutex sessionMutex_;
  std::filesystem::path databasePath_;
  std::filesystem::path chartDatabasePath_;
  sqlite3 *sessionDatabase_ = nullptr;
};
