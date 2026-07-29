#pragma once

#include "../CourseIdentity.h"
#include "../ModernResult.h"
#include "../ScoreProvenance.h"
#include "../ir/IrRemoteScoreModels.h"
#include "ChartRepository.h"
#include "ScoreRepositoryModels.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct CoursePlaySession;

namespace result_persistence {
struct PendingChartScoreWrite;

struct PendingCourseScoreWrite {
  std::string attemptId;
  int modernResultId = 0;
  std::string createdAt;
  ModernCourseResult result;

  bool operator==(const PendingCourseScoreWrite &) const = default;
};

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
  static constexpr int kCurrentSchemaVersion = 11;

  class [[nodiscard]] PreparedScoreQueryDatabase {
  public:
    PreparedScoreQueryDatabase(const ScoreRepository &,
                               ChartRepository::Session &);
    ~PreparedScoreQueryDatabase();
    PreparedScoreQueryDatabase(PreparedScoreQueryDatabase &&) = delete;
    PreparedScoreQueryDatabase &
    operator=(PreparedScoreQueryDatabase &&) noexcept = delete;

    PreparedScoreQueryDatabase(const PreparedScoreQueryDatabase &) = delete;
    PreparedScoreQueryDatabase &
    operator=(const PreparedScoreQueryDatabase &) = delete;

    [[nodiscard]] const std::optional<std::string> &error() const;

  private:
    friend class ScoreRepository;
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
  bool BindDatabasePath(std::filesystem::path databasePath,
                        std::string &errorMessage);
  [[nodiscard]] static bool HasActiveReads();
  [[nodiscard]] static bool HasActiveWrites();
  bool EnsureSchema();
  void Shutdown();
  bool SaveScore(const bms_parser::ChartMeta &chartMeta,
                 const RhythmState &state,
                 const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  result_persistence::ProjectionOutcome
  SaveProjectedScore(const result_persistence::PendingChartScoreWrite &pending);
  result_persistence::ProjectionOutcome SaveProjectedCourseScore(
      const result_persistence::PendingCourseScoreWrite &pending);
  [[nodiscard]] ImportedIrScoreProjectionOutcome ReplaceImportedIrScores(
      std::string_view providerId, std::string_view serverOrigin,
      std::int64_t syncGeneration, std::span<const ir::IrRemoteScore> scores);
  [[nodiscard]] ImportedIrScoreProjectionOutcome
  ClearImportedIrScores(std::string_view providerId);
  static bool ClearImportedIrScoresSnapshot(
      const std::filesystem::path &snapshotDatabasePath,
      std::string &errorMessage);
  [[nodiscard]] bool ImportedIrScoresAreCurrent(std::string_view providerId,
                                                std::string_view serverOrigin,
                                                std::int64_t syncGeneration,
                                                std::size_t scoreCount);
  bool SaveCourseScore(
      const CoursePlaySession &session, const RhythmState &state,
      int completedCharts, int totalCharts,
      const ScoreProvenance &provenance = ScoreProvenance::Legacy());
  std::optional<ScoreBestSnapshot> LoadBestScore(
      const bms_parser::ChartMeta &chartMeta,
      const std::optional<std::string> &beforeCreatedAt = std::nullopt,
      const std::optional<std::string> &excludeAttemptId = std::nullopt,
      int selectedLongNoteMode = 0);
  std::optional<ScoreBestSnapshot>
  LoadBestScoreForRuleset(const bms_parser::ChartMeta &chartMeta,
      const RulesetDescriptor &requiredRuleset,
      int selectedLongNoteMode = 0);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScore(const CoursePlaySession &session);
  CourseScoreRecoveryResult RecoverCourseRecords(
      std::span<const course_identity::Definition> definitions);
  ScoreClearRankCache LoadBestClearRanks();
  ScoreClearRankCache LoadLocalBestClearRanks();
  ScoreClearRankCache
  LoadLocalBestClearRanks(ChartRepository::Session &chartSession,
                          std::string_view schema);
  ScoreClearRankCache LoadBestClearRanks(ChartRepository::Session &chartSession,
                                         std::string_view schema);
  ScoreBestCache LoadBestScores();
  ScoreBestCache LoadBestScores(ChartRepository::Session &chartSession,
                                std::string_view schema);
  [[nodiscard]] std::uint64_t GetRevision() const;

private:
  struct Impl;
  std::unique_ptr<PreparedScoreQueryDatabase::State>
  PrepareScoreQueryState(ChartRepository::Session &chartSession) const;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePathLocked() const;
  bool EnsureSessionDatabaseLocked();
  void CloseSessionDatabaseLocked();
  std::unique_ptr<Impl> impl_;
};
