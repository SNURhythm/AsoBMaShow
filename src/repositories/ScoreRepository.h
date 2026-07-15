#pragma once

#include "../CourseIdentity.h"
#include "../ScoreProvenance.h"
#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"
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

struct TransparentStringHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
};

struct ScoreRankByLongNoteMode {
  std::array<int, 4> ranks{kNoClearTypeRank, kNoClearTypeRank, kNoClearTypeRank,
                           kNoClearTypeRank};

  [[nodiscard]] int bestRankForMode(int lnMode) const;
};

struct ScoreBestSnapshot {
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  std::string createdAt;
};

struct ScoreBestByLongNoteMode {
  std::array<std::optional<ScoreBestSnapshot>, 4> snapshots{};

  [[nodiscard]] std::optional<ScoreBestSnapshot> bestForMode(int lnMode) const;
};

using ScoreRankMap = std::unordered_map<std::string, ScoreRankByLongNoteMode,
                                        TransparentStringHash, std::equal_to<>>;
struct CourseScoreRankByLongNoteMode {
  std::array<int, 4> ranks{kNoClearTypeRank, kNoClearTypeRank, kNoClearTypeRank,
                           kNoClearTypeRank};
  int wildcardRank = kNoClearTypeRank;

  [[nodiscard]] int bestRankForMode(int lnMode) const;
};

using CourseScoreRankMap =
    std::unordered_map<std::string, CourseScoreRankByLongNoteMode,
                       TransparentStringHash, std::equal_to<>>;
using LegacyCourseScoreRankMap =
    std::unordered_map<int, CourseScoreRankByLongNoteMode>;
using ScoreBestMap = std::unordered_map<std::string, ScoreBestByLongNoteMode,
                                        TransparentStringHash, std::equal_to<>>;

struct ScoreClearRankCache {
  ScoreRankMap rankBySha256;
  CourseScoreRankMap rankByCourseKey;
  LegacyCourseScoreRankMap rankByLegacyCourseId;

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta,
                                int selectedLongNoteMode = 0) const;
  [[nodiscard]] int bestRankForHash(const std::string &sha256,
                                    int longNoteMode = 0) const;
  [[nodiscard]] int bestRankForStoredKey(std::string_view sha256,
                                         int longNoteMode = 0) const;
  [[nodiscard]] int bestCourseRankFor(std::string_view courseKey,
                                      int legacyCourseId, int lnMode) const;
};

struct CourseScoreEvidence {
  int legacyCourseId = 0;
  int totalCharts = 0;
  std::string courseName;
  std::string courseGroupName;
  std::string canonicalConstraintPayload;
  std::string courseKey;

  bool operator==(const CourseScoreEvidence &) const = default;
};

struct CourseScoreRecoveryResult {
  std::string errorMessage;
  std::vector<CourseScoreEvidence> evidence;

  [[nodiscard]] bool ok() const noexcept { return errorMessage.empty(); }
};

struct ScoreBestCache {
  ScoreBestMap scoreBySha256;

  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestFor(const bms_parser::ChartMeta &chartMeta,
          int selectedLongNoteMode = 0) const;
  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestForHash(const std::string &sha256, int longNoteMode = 0) const;
  [[nodiscard]] std::optional<ScoreBestSnapshot>
  bestForStoredKey(std::string_view sha256, int longNoteMode = 0) const;
};

[[nodiscard]] int
scoreLongNoteModeForClearLamp(const bms_parser::ChartMeta &chartMeta,
                              int selectedLongNoteMode = 0);
[[nodiscard]] int scoreLongNoteModeForClearLamp(int chartLongNoteMode,
                                                int totalLongNotes,
                                                int totalBackSpinNotes,
                                                int selectedLongNoteMode = 0);

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

  ScoreRepository() = default;
  explicit ScoreRepository(std::filesystem::path databasePath);
  ~ScoreRepository();
  ScoreRepository(const ScoreRepository &) = delete;
  ScoreRepository &operator=(const ScoreRepository &) = delete;

  void SetDatabasePath(std::filesystem::path databasePath);
  [[nodiscard]] std::filesystem::path GetDatabasePath() const;
  [[nodiscard]] std::filesystem::path GetResolvedDatabasePath() const;
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
  ScoreClearRankCache LoadBestClearRanks(sqlite3 *db, std::string_view schema);
  ScoreBestCache LoadBestScores();
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
  sqlite3 *sessionDatabase_ = nullptr;
};
