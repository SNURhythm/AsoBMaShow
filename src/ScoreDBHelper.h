#pragma once

#include "ScoreProvenance.h"
#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"
#include "sqlite3.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
using CourseScoreRankMap =
    std::unordered_map<std::string, int, TransparentStringHash,
                       std::equal_to<>>;
using ScoreBestMap = std::unordered_map<std::string, ScoreBestByLongNoteMode,
                                        TransparentStringHash, std::equal_to<>>;

struct ScoreClearRankCache {
  ScoreRankMap rankBySha256;
  CourseScoreRankMap rankByCourseId;

  [[nodiscard]] int bestRankFor(const bms_parser::ChartMeta &chartMeta,
                                int selectedLongNoteMode = 0) const;
  [[nodiscard]] int bestRankForHash(const std::string &sha256,
                                    int longNoteMode = 0) const;
  [[nodiscard]] int bestRankForStoredKey(std::string_view sha256,
                                         int longNoteMode = 0) const;
  [[nodiscard]] int bestCourseRankForId(int courseId) const;
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

class ScoreDBHelper {
public:
  static constexpr int kCurrentSchemaVersion = 5;

  class [[nodiscard]] PreparedScoreQueryDatabase {
  public:
    PreparedScoreQueryDatabase(const ScoreDBHelper &, sqlite3 *chartDatabase);
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

  ScoreDBHelper() = default;
  explicit ScoreDBHelper(std::filesystem::path databasePath);
  ~ScoreDBHelper();
  ScoreDBHelper(const ScoreDBHelper &) = delete;
  ScoreDBHelper &operator=(const ScoreDBHelper &) = delete;

  static ScoreDBHelper &GetInstance();

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
  // Compatibility API for standalone helpers. The runtime singleton keeps its
  // connection private so raw handles cannot outlive a profile switch.
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
      const std::optional<std::string> &beforeCreatedAt = std::nullopt);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScore(const CoursePlaySession &session);
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
  bool InsertScoreOnConnection(sqlite3 *db,
                               const bms_parser::ChartMeta &chartMeta,
                               const RhythmState &state,
                               const ScoreProvenance &provenance,
                               const std::string &provenanceJson);
  bool InsertCourseScoreOnConnection(
      sqlite3 *db, const CoursePlaySession &session, const RhythmState &state,
      int completedCharts, int totalCharts, const ScoreProvenance &provenance,
      const std::string &provenanceJson);
  std::optional<ScoreBestSnapshot> LoadBestScoreOnConnection(
      sqlite3 *db, const bms_parser::ChartMeta &chartMeta,
      const std::optional<std::string> &beforeCreatedAt);
  std::optional<ScoreBestSnapshot>
  LoadBestCourseScoreOnConnection(sqlite3 *db,
                                  const CoursePlaySession &session);
  [[nodiscard]] std::filesystem::path
  GetResolvedDatabasePathLocked() const;
  sqlite3 *EnsureSessionDatabaseLocked();
  void CloseSessionDatabaseLocked();

  mutable std::mutex sessionMutex_;
  std::filesystem::path databasePath_;
  sqlite3 *sessionDatabase_ = nullptr;
};
