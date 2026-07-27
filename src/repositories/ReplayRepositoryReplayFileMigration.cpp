#include "ReplayRepositoryReplayFileMigration.h"

#include "ReplayRepositoryInternal.h"
#include "SqliteRAII.h"
#include "../CourseConstraintUtils.h"
#include "../CourseIdentity.h"
#include "../FileChecksum.h"
#include "../ResultPersistenceModel.h"
#include "../ScoreProvenance.h"
#include "../replay/BeatorajaReplayCodec.h"
#include "../replay/BeatorajaReplayPath.h"
#include "../replay/LegacyReplayIdentity.h"
#include "../replay/ReplayFileStore.h"
#include "../scene/play/GameplayGaugeRules.h"
#include "../scene/play/GameplayScoreState.h"
#include "../path.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace replay_repository_detail {
namespace {

using MigrationStatus = ReplayMigrationOutcome::Status;

struct LegacyEvent {
  replay::LegacyPlaybackEvent event;
  int eventIndex = 0;
};

struct LegacyChart {
  std::int64_t id = 0;
  std::string chartPath;
  std::string chartMd5;
  std::string chartSha256;
  std::string chartTitle;
  std::string chartArtist;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = true;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<std::int64_t> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<std::int64_t> playOption2Seed;
  std::string assistOption = assist_options::kOff;
  std::string createdAt;
  std::int64_t playedAtUnixMillis = 0;
  std::optional<std::string> attemptId;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  std::string provenanceJson;
  std::vector<LegacyEvent> events;
  std::vector<replay::ReplayTouchSample> touchSamples;
  std::vector<replay::ReplayLaneCoverEvent> laneCoverEvents;
  result_persistence::PersistedChartResult result;
  replay::ReplayPlaybackData playback;
  replay::ReplayPathIdentity path;
  replay::ReplayFileMetadata file;
  bool courseStage = false;
  std::vector<ReplayMigrationClassicLongNote> classicLongNotes;
};

struct LegacyCourse {
  std::int64_t id = 0;
  int legacyCourseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::CourseDefault;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  int longNoteMode = 0;
  std::string requestedPlayOption = "NORMAL";
  std::string assistOption = assist_options::kOff;
  int finalScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string createdAt;
  std::int64_t playedAtUnixMillis = 0;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  std::string provenanceJson;
  std::vector<std::size_t> chartIndexes;
  std::vector<std::int64_t> restMicrosAfterStage;
  result_persistence::PersistedCourseResult result;
  replay::CourseReplayPlaybackData playback;
  replay::ReplayPathIdentity path;
  replay::ReplayFileMetadata file;
};

class Statement {
public:
  Statement(sqlite3 *database, const char *sql) : database_(database) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK) {
      statement_ = nullptr;
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  [[nodiscard]] sqlite3_stmt *get() const { return statement_; }
  [[nodiscard]] bool valid() const { return statement_ != nullptr; }
  [[nodiscard]] std::string error() const {
    return database_ == nullptr ? std::string("database is unavailable")
                                : sqlite3_errmsg(database_);
  }

private:
  sqlite3 *database_ = nullptr;
  sqlite3_stmt *statement_ = nullptr;
};

std::mutex topologyResolverMutex;
ReplayMigrationChartTopologyResolver topologyResolver;

std::string columnText(sqlite3_stmt *statement, int column) {
  const auto *value = sqlite3_column_text(statement, column);
  return value == nullptr ? std::string{}
                          : reinterpret_cast<const char *>(value);
}

bool bindText(sqlite3_stmt *statement, int column, std::string_view value) {
  return value.size() <=
             static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
         sqlite3_bind_text(statement, column, value.data(),
                           static_cast<int>(value.size()),
                           SQLITE_TRANSIENT) == SQLITE_OK;
}

bool execute(sqlite3 *database, std::string_view sql, std::string &diagnostic) {
  char *message = nullptr;
  const int result = sqlite3_exec(database, std::string(sql).c_str(), nullptr,
                                  nullptr, &message);
  if (result == SQLITE_OK) {
    return true;
  }
  diagnostic = message != nullptr ? message : sqlite3_errmsg(database);
  sqlite3_free(message);
  return false;
}

ReplayMigrationOutcome failure(MigrationStatus status, std::string diagnostic,
                               std::size_t chartFiles = 0,
                               std::size_t courseFiles = 0) {
  return {.status = status,
          .chartFiles = chartFiles,
          .courseFiles = courseFiles,
          .diagnostic = std::move(diagnostic)};
}

bool fault(const ReplayMigrationFaults &faults, std::string_view phase,
           std::int64_t publicId = 0) {
  return faults.failAt && faults.failAt(phase, publicId);
}

std::string lowerHex(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool normalizeResultProvenance(ScoreProvenance &provenance,
                               std::string &storedJson,
                               std::string &diagnostic) {
  ScoreProvenance canonicalCandidate = provenance;
  if (canonicalCandidate.schemaVersion <
      ScoreProvenance::kPolicyProofSchemaVersion) {
    canonicalCandidate.schemaVersion =
        ScoreProvenance::kPolicyProofSchemaVersion;
  }

  std::string validationError;
  if (const auto canonical =
          serializeValidatedScoreProvenance(canonicalCandidate,
                                            validationError)) {
    provenance = std::move(canonicalCandidate);
    storedJson = *canonical;
    return true;
  }

  int sourceSchemaVersion = -1;
  try {
    const auto source = nlohmann::ordered_json::parse(storedJson);
    sourceSchemaVersion = source.value("schemaVersion", -1);
  } catch (...) {
    diagnostic = "legacy result provenance JSON is malformed";
    return false;
  }
  if (sourceSchemaVersion < 1 ||
      sourceSchemaVersion >= ScoreProvenance::kPolicyProofSchemaVersion) {
    diagnostic = validationError.empty() ? "legacy result provenance is invalid"
                                         : std::move(validationError);
    return false;
  }

  provenance = ScoreProvenance::Legacy();
  const auto legacy =
      serializeValidatedScoreProvenance(provenance, validationError);
  if (!legacy.has_value()) {
    diagnostic = validationError.empty()
                     ? "could not normalize outdated result provenance"
                     : std::move(validationError);
    return false;
  }
  storedJson = *legacy;
  return true;
}

bool readLegacyRandomValues(sqlite3_stmt *statement, int column,
                            std::vector<int> &output,
                            std::string &diagnostic) {
  const int storage = sqlite3_column_type(statement, column);
  if (storage == SQLITE_NULL) {
    output.clear();
    return true;
  }
  if (storage != SQLITE_TEXT) {
    diagnostic = "legacy replay RANDOM values have invalid storage";
    return false;
  }

  const std::string_view source = sqliteColumnTextView(statement, column);
  if (source.empty()) {
    diagnostic = "legacy replay RANDOM values are malformed";
    return false;
  }

  std::vector<int> values;
  std::size_t begin = 0;
  while (begin < source.size()) {
    const std::size_t end = source.find(',', begin);
    const std::string_view token = source.substr(
        begin, end == std::string_view::npos ? source.size() - begin
                                             : end - begin);
    int value = 0;
    const auto [tail, error] =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || error != std::errc{} ||
        tail != token.data() + token.size() ||
        std::to_string(value) != token) {
      diagnostic = "legacy replay RANDOM values are malformed";
      return false;
    }
    values.push_back(value);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
    if (begin == source.size()) {
      diagnostic = "legacy replay RANDOM values are malformed";
      return false;
    }
  }
  output = std::move(values);
  return true;
}

bool supportedKeyMode(int keyMode) noexcept {
  switch (keyMode) {
  case 5:
  case 7:
  case 9:
  case 10:
  case 14:
  case 24:
  case 48:
    return true;
  default:
    return false;
  }
}

GaugeProfile gaugeProfileFromInt(int value) {
  return static_cast<GaugeProfile>(value);
}

bool validGaugeTypeValue(int value) noexcept {
  return value >= 0 && value < static_cast<int>(kGaugeTypeCount);
}

bool validGaugeAutoShiftValue(int value) noexcept {
  return value >= gaugeAutoShiftModeValue(GaugeAutoShiftMode::None) &&
         value <= gaugeAutoShiftModeValue(GaugeAutoShiftMode::BestClear);
}

bool validGaugeProfileValue(int value) noexcept {
  return value >= static_cast<int>(GaugeProfile::Standard) &&
         value <= static_cast<int>(GaugeProfile::Standard24Keys);
}

bool readInteger64(sqlite3_stmt *statement, int column, std::int64_t &value,
                   std::string_view field, std::string &diagnostic) {
  if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
    diagnostic = "legacy replay " + std::string(field) +
                 " must use integer SQLite storage";
    return false;
  }
  value = sqlite3_column_int64(statement, column);
  return true;
}

bool readInteger(sqlite3_stmt *statement, int column, int &value,
                 std::string_view field, std::string &diagnostic) {
  std::int64_t stored = 0;
  if (!readInteger64(statement, column, stored, field, diagnostic) ||
      stored < std::numeric_limits<int>::min() ||
      stored > std::numeric_limits<int>::max()) {
    if (diagnostic.empty()) {
      diagnostic =
          "legacy replay " + std::string(field) + " exceeds integer range";
    }
    return false;
  }
  value = static_cast<int>(stored);
  return true;
}

bool readUnsignedInteger(sqlite3_stmt *statement, int column,
                         unsigned int &value, std::string_view field,
                         std::string &diagnostic) {
  std::int64_t stored = 0;
  if (!readInteger64(statement, column, stored, field, diagnostic) ||
      stored < 0 ||
      stored > static_cast<std::int64_t>(
                   std::numeric_limits<unsigned int>::max())) {
    if (diagnostic.empty()) {
      diagnostic = "legacy replay " + std::string(field) +
                   " exceeds unsigned integer range";
    }
    return false;
  }
  value = static_cast<unsigned int>(stored);
  return true;
}

bool readFiniteFloat(sqlite3_stmt *statement, int column, float &value,
                     std::string_view field, std::string &diagnostic) {
  const int storageType = sqlite3_column_type(statement, column);
  if (storageType != SQLITE_INTEGER && storageType != SQLITE_FLOAT) {
    diagnostic = "legacy replay " + std::string(field) +
                 " must use numeric SQLite storage";
    return false;
  }
  const double stored = sqlite3_column_double(statement, column);
  constexpr double floatLimit = std::numeric_limits<float>::max();
  if (!std::isfinite(stored) || stored < -floatLimit || stored > floatLimit) {
    diagnostic =
        "legacy replay " + std::string(field) + " is not a finite float";
    return false;
  }
  value = static_cast<float>(stored);
  return true;
}

template <typename Range, typename Projection>
bool validateIndexedTimestampOrder(const Range &records,
                                   Projection songTimeMicros,
                                   std::string_view track,
                                   std::string &diagnostic) {
  if (std::ranges::is_sorted(records, {}, songTimeMicros)) {
    return true;
  }
  diagnostic = "legacy replay " + std::string(track) +
               " timestamps decrease in indexed order";
  return false;
}

bool readEvents(sqlite3 *database, LegacyChart &chart,
                std::string &diagnostic) {
  Statement statement(
      database,
      "SELECT event_index,action,lane,note_time_micros,song_time_micros,"
      "judge_time_micros,judgement,diff_micros,gauge,gauge_type,combo,score "
      "FROM replay_events WHERE replay_id=? ORDER BY event_index,id");
  if (!statement.valid() ||
      sqlite3_bind_int64(statement.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = statement.error();
    return false;
  }
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    LegacyEvent entry;
    int action = 0;
    int judgement = 0;
    int gaugeType = 0;
    if (!readInteger(statement.get(), 0, entry.eventIndex, "event index",
                     diagnostic) ||
        !readInteger(statement.get(), 1, action, "event action", diagnostic) ||
        !readInteger(statement.get(), 2, entry.event.lane, "event lane",
                     diagnostic) ||
        !readInteger64(statement.get(), 3, entry.event.noteTimeMicros,
                       "event note timestamp", diagnostic) ||
        !readInteger64(statement.get(), 4, entry.event.songTimeMicros,
                       "event song timestamp", diagnostic) ||
        !readInteger64(statement.get(), 5, entry.event.judgeTimeMicros,
                       "event judge timestamp", diagnostic) ||
        !readInteger(statement.get(), 6, judgement, "event judgement",
                     diagnostic) ||
        !readInteger64(statement.get(), 7, entry.event.diffMicros,
                       "event timing difference", diagnostic) ||
        !readInteger(statement.get(), 9, gaugeType, "event gauge type",
                     diagnostic) ||
        !readInteger(statement.get(), 10, entry.event.combo, "event combo",
                     diagnostic) ||
        !readInteger(statement.get(), 11, entry.event.score, "event score",
                     diagnostic)) {
      return false;
    }
    if (action < static_cast<int>(replay::LegacyPlaybackAction::Press) ||
        action > static_cast<int>(replay::LegacyPlaybackAction::MultiBad) ||
        judgement < PGreat || judgement >= JudgementCount ||
        !validGaugeTypeValue(gaugeType)) {
      diagnostic = "legacy replay event enum value is invalid";
      return false;
    }
    entry.event.action = static_cast<replay::LegacyPlaybackAction>(action);
    entry.event.judgement = static_cast<Judgement>(judgement);
    if (!readFiniteFloat(statement.get(), 8, entry.event.gauge,
                         "event gauge", diagnostic)) {
      return false;
    }
    entry.event.gaugeType = gaugeTypeAtIndex(gaugeType);
    chart.events.push_back(std::move(entry));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  if (!validateIndexedTimestampOrder(
          chart.events,
          [](const LegacyEvent &entry) { return entry.event.songTimeMicros; },
          "event", diagnostic)) {
    return false;
  }

  Statement touch(
      database,
      "SELECT sample_index,action,finger_id,song_time_micros,x,y FROM "
      "replay_touch_samples WHERE replay_id=? ORDER BY sample_index,id");
  if (!touch.valid() ||
      sqlite3_bind_int64(touch.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = touch.error();
    return false;
  }
  while ((result = sqlite3_step(touch.get())) == SQLITE_ROW) {
    int sampleIndex = 0;
    int action = 0;
    std::int64_t fingerId = 0;
    std::int64_t songTimeMicros = 0;
    if (!readInteger(touch.get(), 0, sampleIndex, "touch sample index",
                     diagnostic) ||
        !readInteger(touch.get(), 1, action, "touch action", diagnostic) ||
        !readInteger64(touch.get(), 2, fingerId, "touch finger ID",
                       diagnostic) ||
        !readInteger64(touch.get(), 3, songTimeMicros,
                       "touch song timestamp", diagnostic)) {
      return false;
    }
    if (action < static_cast<int>(replay::ReplayTouchAction::Down) ||
        action > static_cast<int>(replay::ReplayTouchAction::Cancel)) {
      diagnostic = "legacy replay touch action is invalid";
      return false;
    }
    float x = 0.0F;
    float y = 0.0F;
    if (!readFiniteFloat(touch.get(), 4, x, "touch x coordinate",
                         diagnostic) ||
        !readFiniteFloat(touch.get(), 5, y, "touch y coordinate",
                         diagnostic)) {
      return false;
    }
    chart.touchSamples.push_back(
        {.action = static_cast<replay::ReplayTouchAction>(action),
         .fingerId = fingerId,
         .songTimeMicros = songTimeMicros,
         .x = x,
         .y = y});
  }
  if (result != SQLITE_DONE) {
    diagnostic = touch.error();
    return false;
  }
  if (!validateIndexedTimestampOrder(chart.touchSamples,
                                     &replay::ReplayTouchSample::songTimeMicros,
                                     "touch sample", diagnostic)) {
    return false;
  }

  Statement cover(
      database,
      "SELECT event_index,song_time_micros,note_start_position_percent,"
      "reset_visible_time_reference FROM replay_lane_cover_events WHERE "
      "replay_id=? ORDER BY event_index,id");
  if (!cover.valid() ||
      sqlite3_bind_int64(cover.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = cover.error();
    return false;
  }
  while ((result = sqlite3_step(cover.get())) == SQLITE_ROW) {
    int eventIndex = 0;
    std::int64_t songTimeMicros = 0;
    int noteStartPositionPercent = 0;
    int resetVisibleTimeReference = 0;
    if (!readInteger(cover.get(), 0, eventIndex, "lane-cover event index",
                     diagnostic) ||
        !readInteger64(cover.get(), 1, songTimeMicros,
                       "lane-cover song timestamp", diagnostic) ||
        !readInteger(cover.get(), 2, noteStartPositionPercent,
                     "lane-cover position", diagnostic) ||
        !readInteger(cover.get(), 3, resetVisibleTimeReference,
                     "lane-cover reset flag", diagnostic)) {
      return false;
    }
    chart.laneCoverEvents.push_back(
        {.songTimeMicros = songTimeMicros,
         .noteStartPositionPercent = noteStartPositionPercent,
         .resetVisibleTimeReference = resetVisibleTimeReference != 0});
  }
  if (result != SQLITE_DONE) {
    diagnostic = cover.error();
    return false;
  }
  return validateIndexedTimestampOrder(
      chart.laneCoverEvents, &replay::ReplayLaneCoverEvent::songTimeMicros,
      "lane-cover event", diagnostic);
}

std::vector<bool> countedLegacyResultEvents(const LegacyChart &chart) {
  std::vector<bool> counted(chart.events.size(), false);
  for (std::size_t index = 0; index < chart.events.size(); ++index) {
    const auto &event = chart.events[index].event;
    if (event.action == replay::LegacyPlaybackAction::Gauge ||
        event.action == replay::LegacyPlaybackAction::Mine ||
        event.judgement == None) {
      continue;
    }
    if (event.action != replay::LegacyPlaybackAction::Press) {
      counted[index] = event.judgement != JudgementCount;
      continue;
    }

    const auto classic =
        std::ranges::find_if(chart.classicLongNotes, [&](const auto &longNote) {
          return longNote.lane == event.lane &&
                 longNote.headTimeMicros == event.noteTimeMicros;
        });
    if (classic == chart.classicLongNotes.end() || event.judgement == Kpoor) {
      counted[index] = event.judgement != JudgementCount;
      continue;
    }

    const bool hasRecordedTailResult =
        std::ranges::any_of(chart.events, [&](const LegacyEvent &candidate) {
          return candidate.event.lane == classic->lane &&
                 candidate.event.noteTimeMicros == classic->tailTimeMicros &&
                 candidate.event.judgement != None &&
                 (candidate.event.action ==
                      replay::LegacyPlaybackAction::Release ||
                  candidate.event.action == replay::LegacyPlaybackAction::Miss);
        });
    counted[index] = event.judgement == Bad && !hasRecordedTailResult;
  }
  return counted;
}

struct LegacyJudgementTimingFacts {
  result_persistence::ChartJudgementTiming timing;
  int fast = 0;
  int slow = 0;
};

LegacyJudgementTimingFacts legacyJudgementTimingFacts(
    const LegacyChart &chart, const std::vector<bool> &countedEvents) {
  LegacyJudgementTimingFacts result;
  for (std::size_t index = 0; index < chart.events.size(); ++index) {
    const auto &event = chart.events[index].event;
    if (!countedEvents[index] || event.judgement == Kpoor ||
        event.judgement == None || event.judgement == JudgementCount) {
      continue;
    }
    auto &timing = result.timing.byJudgement[static_cast<std::size_t>(
        event.judgement)];
    if (event.diffMicros < 0) {
      ++result.fast;
      ++timing.fast;
    } else if (event.diffMicros > 0) {
      ++result.slow;
      ++timing.slow;
    }
  }
  return result;
}

bool readPendingResult(sqlite3 *database, LegacyChart &chart, int keyMode,
                       std::optional<int> chartTotalNotes,
                       std::string &diagnostic) {
  const std::vector<bool> countedEvents = countedLegacyResultEvents(chart);
  const LegacyJudgementTimingFacts timingFacts =
      legacyJudgementTimingFacts(chart, countedEvents);
  Statement statement(
      database,
      "SELECT attempt_id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,score,max_score,max_combo,combo_break,pgreat,"
      "great,good,bad,poor,kpoor,fast,slow,final_gauge,clear_type,"
      "ruleset_version,eligibility,provenance_json FROM "
      "pending_chart_score_writes WHERE replay_id=?");
  if (!statement.valid() ||
      sqlite3_bind_int64(statement.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = statement.error();
    return false;
  }
  const int result = sqlite3_step(statement.get());
  auto &persisted = chart.result;
  persisted.resultId = static_cast<int>(chart.id);
  persisted.attemptId = chart.attemptId;
  persisted.keyMode = keyMode;
  persisted.playedAtUnixMillis = chart.playedAtUnixMillis;
  if (result == SQLITE_ROW) {
    persisted.attemptId = columnText(statement.get(), 0);
    persisted.score.chartPath = columnText(statement.get(), 1);
    persisted.score.chartMd5 = lowerHex(columnText(statement.get(), 2));
    persisted.score.chartSha256 = lowerHex(columnText(statement.get(), 3));
    persisted.score.chartTitle = columnText(statement.get(), 4);
    persisted.score.chartArtist = columnText(statement.get(), 5);
    int rulesetVersion = 0;
    int eligibility = 0;
    if (!readInteger(statement.get(), 6, persisted.score.longNoteMode,
                     "pending score long-note mode", diagnostic) ||
        !readInteger(statement.get(), 7, persisted.score.score,
                     "pending score value", diagnostic) ||
        !readInteger(statement.get(), 8, persisted.score.maxScore,
                     "pending score maximum", diagnostic) ||
        !readInteger(statement.get(), 9, persisted.score.maxCombo,
                     "pending score maximum combo", diagnostic) ||
        !readInteger(statement.get(), 10, persisted.score.comboBreak,
                     "pending score combo breaks", diagnostic) ||
        !readInteger(statement.get(), 11, persisted.score.pGreat,
                     "pending score PGREAT count", diagnostic) ||
        !readInteger(statement.get(), 12, persisted.score.great,
                     "pending score GREAT count", diagnostic) ||
        !readInteger(statement.get(), 13, persisted.score.good,
                     "pending score GOOD count", diagnostic) ||
        !readInteger(statement.get(), 14, persisted.score.bad,
                     "pending score BAD count", diagnostic) ||
        !readInteger(statement.get(), 15, persisted.score.poor,
                     "pending score POOR count", diagnostic) ||
        !readInteger(statement.get(), 16, persisted.score.kPoor,
                     "pending score KPOOR count", diagnostic) ||
        !readInteger(statement.get(), 17, persisted.score.fast,
                     "pending score fast count", diagnostic) ||
        !readInteger(statement.get(), 18, persisted.score.slow,
                     "pending score slow count", diagnostic) ||
        !readInteger(statement.get(), 20, persisted.score.clearType,
                     "pending score clear type", diagnostic) ||
        !readInteger(statement.get(), 21, rulesetVersion,
                     "pending score ruleset version", diagnostic) ||
        !readInteger(statement.get(), 22, eligibility,
                     "pending score eligibility", diagnostic)) {
      return false;
    }
    if (!replay::isCanonicalLegacyDigest(persisted.score.chartSha256, 64) ||
        !replay::isCanonicalLegacyDigest(persisted.score.chartMd5, 32) ||
        persisted.score.longNoteMode < 0 || persisted.score.longNoteMode > 3) {
      diagnostic = "pending score chart identity or long-note mode is invalid";
      return false;
    }
    if (persisted.score.chartSha256 != chart.chartSha256 ||
        persisted.score.chartMd5 != chart.chartMd5 ||
        persisted.score.longNoteMode != chart.longNoteMode) {
      diagnostic =
          "pending score chart identity or long-note mode differs from replay";
      return false;
    }
    if (!readFiniteFloat(statement.get(), 19, persisted.score.finalGauge,
                         "pending score final gauge", diagnostic)) {
      return false;
    }
    std::string provenanceError;
    const std::string pendingProvenance = columnText(statement.get(), 23);
    auto provenance =
        deserializeScoreProvenance(pendingProvenance, provenanceError);
    if (!provenance.has_value() || pendingProvenance != chart.provenanceJson ||
        provenance->ruleset.version != rulesetVersion ||
        static_cast<int>(provenance->eligibility) != eligibility) {
      diagnostic = provenanceError.empty()
                       ? "pending score provenance differs from replay"
                       : std::move(provenanceError);
      return false;
    }
    persisted.score.provenance = std::move(*provenance);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
      diagnostic = "legacy replay has duplicate pending score rows";
      return false;
    }
    if (persisted.score.fast != timingFacts.fast ||
        persisted.score.slow != timingFacts.slow) {
      diagnostic =
          "pending score fast/slow totals differ from replay judgements";
      return false;
    }
    persisted.judgementTiming = timingFacts.timing;
  } else if (result == SQLITE_DONE) {
    if (!chartTotalNotes.has_value() || *chartTotalNotes <= 0 ||
        *chartTotalNotes > std::numeric_limits<int>::max() / 2) {
      diagnostic =
          "chart note metadata is required to reconstruct a legacy result";
      return false;
    }
    auto &score = persisted.score;
    score.chartPath = chart.chartPath;
    score.chartMd5 = chart.chartMd5;
    score.chartSha256 = chart.chartSha256;
    score.chartTitle = chart.chartTitle;
    score.chartArtist = chart.chartArtist;
    score.longNoteMode = chart.longNoteMode;
    score.score = chart.finalScore;
    score.maxCombo = chart.maxCombo;
    score.finalGauge = chart.finalGauge;
    score.clearType = chart.clearType;
    score.provenance = chart.provenance;
    for (std::size_t index = 0; index < chart.events.size(); ++index) {
      const auto &event = chart.events[index].event;
      const bool countsInResult = countedEvents[index];
      switch (event.judgement) {
      case PGreat:
        score.pGreat += countsInResult ? 1 : 0;
        break;
      case Great:
        score.great += countsInResult ? 1 : 0;
        break;
      case Good:
        score.good += countsInResult ? 1 : 0;
        break;
      case Bad:
        score.bad += countsInResult ? 1 : 0;
        score.comboBreak += countsInResult ? 1 : 0;
        break;
      case Poor:
        score.poor += countsInResult ? 1 : 0;
        score.comboBreak += countsInResult ? 1 : 0;
        break;
      case Kpoor:
        score.kPoor += countsInResult ? 1 : 0;
        break;
      case None:
      case JudgementCount:
        break;
      }
    }
    score.fast = timingFacts.fast;
    score.slow = timingFacts.slow;
    score.maxScore = *chartTotalNotes * 2;
    persisted.judgementTiming = timingFacts.timing;
  } else {
    diagnostic = statement.error();
    return false;
  }

  std::vector<bool> gaugeHistoryEvents = countedEvents;
  for (std::size_t index = 0; index < chart.events.size(); ++index) {
    const auto &event = chart.events[index].event;
    gaugeHistoryEvents[index] =
        gaugeHistoryEvents[index] ||
        event.action == replay::LegacyPlaybackAction::Gauge ||
        event.action == replay::LegacyPlaybackAction::Mine;
    if (gaugeHistoryEvents[index]) {
      persisted.adoptedGaugeType = event.gaugeType;
    }
  }
  const bool hasGaugeHistoryEvent = std::ranges::any_of(
      gaugeHistoryEvents, [](bool counted) { return counted; });
  if (!hasGaugeHistoryEvent) {
    persisted.adoptedGaugeType = persisted.score.provenance.gaugeType;
  } else {
    for (std::size_t index = 0; index < chart.events.size(); ++index) {
      const auto &event = chart.events[index].event;
      if (gaugeHistoryEvents[index] &&
          event.gaugeType == persisted.adoptedGaugeType &&
          std::isfinite(event.gauge)) {
        persisted.adoptedGaugeHistory.push_back(event.gauge);
      }
    }
  }
  if (!normalizeResultProvenance(persisted.score.provenance,
                                 chart.provenanceJson, diagnostic)) {
    return false;
  }
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  return true;
}

bool buildPlayback(LegacyChart &chart, std::string &diagnostic) {
  auto &setup = chart.playback.setup;
  setup.chartMd5 = chart.chartMd5;
  setup.chartSha256 = chart.chartSha256;
  setup.keyMode = chart.result.keyMode;
  setup.longNoteMode = chart.longNoteMode;
  setup.hasUndefinedLongNotes = chart.hasUndefinedLongNotes;
  setup.randomSeed = chart.randomSeed;
  setup.randomPrng = chart.randomPrng;
  setup.randomValues = chart.randomValues;
  setup.playOption = chart.playOption.value_or("NORMAL");
  setup.playOptionSeed = chart.playOptionSeed;
  setup.playOption2 = chart.playOption2.value_or("NORMAL");
  setup.playOption2Seed = chart.playOption2Seed;
  setup.assistOption = chart.assistOption;
  setup.initialGaugeType = chart.initialGaugeType;
  setup.gaugeProfile = chart.provenance.gaugeProfile;
  setup.gaugeAutoShift = chart.gaugeAutoShift;
  setup.gaugeAutoShiftLowerBound = chart.provenance.gaugeAutoShiftLowerBound;
  setup.playbackRulesetId = chart.provenance.ruleset.id;
  setup.playbackRulesetRevision = chart.provenance.ruleset.version;
  setup.playbackRatePercent = chart.provenance.playback.percent;
  setup.playbackMode = chart.provenance.playback.mode;
  bms_parser::ChartMeta chartMeta;
  chartMeta.MD5 = chart.chartMd5;
  chartMeta.SHA256 = chart.chartSha256;
  if (const auto *stage =
          score_provenance::uniqueStageForChart(chart.provenance, chartMeta);
      stage != nullptr) {
    setup.candidateSelection = stage->candidateSelection;
  }
  setup.judgeWindowScalePercent = chart.provenance.judgeWindowScalePercent;
  bms_parser::ChartMeta gaugeMeta;
  gaugeMeta.KeyMode = setup.keyMode;
  const GameplayRuleset gaugeRuleset =
      gameplayRulesetFromId(setup.playbackRulesetId)
          .value_or(GameplayRuleset::Beatoraja);
  GameplayScoreState startingGauge({
      .gaugeRules = compileGameplayGaugeRules(
          gaugeRuleset, gaugeMeta, setup.gaugeProfile),
      .keyMode = setup.keyMode,
  });
  startingGauge.configureGauge(
      setup.initialGaugeType, setup.gaugeAutoShift, setup.gaugeProfile,
      setup.gaugeAutoShiftLowerBound);
  if (chart.provenance.startingGaugePercent.has_value()) {
    startingGauge.setStartingGaugePercent(
        *chart.provenance.startingGaugePercent);
  }
  setup.startingGaugePercent = startingGauge.currentGauge;
  setup.startingGaugeState = startingGauge.gaugeSnapshot();
  setup.clubMode = chart.provenance.clubMode;
  setup.initialLaneCoverPercent = 0;
  setup.laneCoverEnabled = false;
  for (const auto &cover : chart.laneCoverEvents) {
    if (cover.songTimeMicros <= 0) {
      setup.initialLaneCoverPercent = cover.noteStartPositionPercent;
      setup.laneCoverEnabled =
          setup.initialLaneCoverPercent.value_or(0) > 0;
    }
  }

  std::vector<replay::InputTransition> candidates;
  candidates.reserve(chart.events.size());
  bool scratchBestEffort = false;
  for (const auto &entry : chart.events) {
    const auto action = entry.event.action;
    if (action != replay::LegacyPlaybackAction::Press &&
        action != replay::LegacyPlaybackAction::Release) {
      continue;
    }
    const auto control =
        legacyReplayControlForPhysicalLane(entry.event.lane, setup.keyMode);
    if (!control.has_value()) {
      diagnostic = "legacy replay input physical lane " +
                   std::to_string(entry.event.lane) +
                   " cannot be mapped for resolved key mode " +
                   std::to_string(setup.keyMode);
      return false;
    }
    if (entry.event.songTimeMicros < replay::kMinimumReplaySongTimeMicros) {
      continue;
    }
    scratchBestEffort |=
        control->kind == replay::LogicalControlKind::ScratchClockwise ||
        control->kind == replay::LogicalControlKind::ScratchCounterClockwise;
    candidates.push_back({
        .songTimeMicros = entry.event.songTimeMicros,
        .control = *control,
        .pressed = action == replay::LegacyPlaybackAction::Press,
    });
  }
  std::map<std::tuple<int, int, int>, bool> states;
  for (const auto &transition : candidates) {
    const auto key =
        std::tuple(static_cast<int>(transition.control.kind),
                   transition.control.player, transition.control.lane);
    const bool pressed = transition.pressed;
    if (states[key] == pressed) {
      continue;
    }
    states[key] = pressed;
    chart.playback.input.push_back(transition);
  }
  chart.playback.touchSamples = chart.touchSamples;
  chart.playback.laneCoverEvents = chart.laneCoverEvents;
  replay::LegacyPlaybackTrack legacy;
  legacy.stockScratchDirectionBestEffort = scratchBestEffort;
  legacy.events.reserve(chart.events.size());
  for (const auto &entry : chart.events) {
    legacy.events.push_back(entry.event);
  }
  chart.playback.legacy = std::move(legacy);

  const auto stem = replay::chartStem(chart.chartSha256, setup.longNoteMode,
                                      setup.hasUndefinedLongNotes, diagnostic);
  if (!stem.has_value()) {
    return false;
  }
  chart.path.stem = *stem;
  return true;
}

bool readCharts(sqlite3 *database, std::vector<LegacyChart> &charts,
                const ReplayMigrationChartMetadataResolver &resolveMetadata,
                std::string &diagnostic) {
  Statement statement(
      database,
      "SELECT id,COALESCE(chart_path,''),lower(trim(COALESCE(chart_md5,''))),"
      "lower(trim(COALESCE(chart_sha256,''))),COALESCE(chart_title,''),"
      "COALESCE(chart_artist,''),ln_mode,gauge_type,gauge_auto_shift,"
      "final_score,max_combo,final_gauge,clear_type,random_seed,random_prng,"
      "random_values,play_option,play_option_seed,play_option2,"
      "play_option2_seed,COALESCE(assist_option,'OFF'),created_at,"
      "ruleset_version,eligibility,provenance_json,attempt_id,"
      "CASE WHEN typeof(created_at)='text' THEN "
      "COALESCE(CAST(strftime('%s',created_at) AS INTEGER)*1000,0) ELSE 0 END "
      "FROM replays ORDER BY id");
  if (!statement.valid()) {
    diagnostic = statement.error();
    return false;
  }
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    LegacyChart chart;
    int initialGaugeType = 0;
    int gaugeAutoShift = 0;
    int rulesetVersion = 0;
    int eligibility = 0;
    if (!readInteger64(statement.get(), 0, chart.id, "chart public ID",
                       diagnostic) ||
        !readInteger(statement.get(), 6, chart.longNoteMode,
                     "chart long-note mode", diagnostic) ||
        !readInteger(statement.get(), 7, initialGaugeType,
                     "chart initial gauge type", diagnostic) ||
        !readInteger(statement.get(), 8, gaugeAutoShift,
                     "chart gauge auto-shift", diagnostic) ||
        !readInteger(statement.get(), 9, chart.finalScore,
                     "chart final score", diagnostic) ||
        !readInteger(statement.get(), 10, chart.maxCombo,
                     "chart maximum combo", diagnostic) ||
        !readInteger(statement.get(), 12, chart.clearType,
                     "chart clear type", diagnostic) ||
        !readInteger(statement.get(), 22, rulesetVersion,
                     "chart ruleset version", diagnostic) ||
        !readInteger(statement.get(), 23, eligibility,
                     "chart eligibility", diagnostic) ||
        !readInteger64(statement.get(), 26, chart.playedAtUnixMillis,
                       "chart play time", diagnostic)) {
      return false;
    }
    if (chart.id <= 0 || chart.id > std::numeric_limits<int>::max()) {
      diagnostic = "legacy replay public ID is invalid";
      return false;
    }
    chart.chartPath = columnText(statement.get(), 1);
    chart.chartMd5 = lowerHex(columnText(statement.get(), 2));
    chart.chartSha256 = lowerHex(columnText(statement.get(), 3));
    chart.chartTitle = columnText(statement.get(), 4);
    chart.chartArtist = columnText(statement.get(), 5);
    if (!replay::isCanonicalLegacyDigest(chart.chartSha256, 64) ||
        !replay::isCanonicalLegacyDigest(chart.chartMd5, 32) ||
        chart.longNoteMode < 0 || chart.longNoteMode > 3) {
      diagnostic = "legacy replay chart identity or long-note mode is invalid";
      return false;
    }
    if (!validGaugeTypeValue(initialGaugeType) ||
        !validGaugeAutoShiftValue(gaugeAutoShift)) {
      diagnostic = "legacy replay gauge configuration is invalid";
      return false;
    }
    chart.initialGaugeType = gaugeTypeAtIndex(initialGaugeType);
    chart.gaugeAutoShift = gaugeAutoShiftModeFromValue(gaugeAutoShift);
    if (!readFiniteFloat(statement.get(), 11, chart.finalGauge,
                         "chart final gauge", diagnostic)) {
      return false;
    }
    if (sqlite3_column_type(statement.get(), 13) != SQLITE_NULL) {
      unsigned int randomSeed = 0;
      if (!readUnsignedInteger(statement.get(), 13, randomSeed,
                               "chart random seed", diagnostic)) {
        return false;
      }
      chart.randomSeed = randomSeed;
    }
    if (sqlite3_column_type(statement.get(), 14) == SQLITE_TEXT) {
      chart.randomPrng = columnText(statement.get(), 14);
    }
    if (!readLegacyRandomValues(statement.get(), 15, chart.randomValues,
                                diagnostic)) {
      return false;
    }
    if (sqlite3_column_type(statement.get(), 16) == SQLITE_TEXT) {
      chart.playOption = columnText(statement.get(), 16);
    }
    if (sqlite3_column_type(statement.get(), 17) != SQLITE_NULL) {
      std::int64_t playOptionSeed = 0;
      if (!readInteger64(statement.get(), 17, playOptionSeed,
                         "chart play-option seed", diagnostic)) {
        return false;
      }
      chart.playOptionSeed = playOptionSeed;
    }
    if (sqlite3_column_type(statement.get(), 18) == SQLITE_TEXT) {
      chart.playOption2 = columnText(statement.get(), 18);
    }
    if (sqlite3_column_type(statement.get(), 19) != SQLITE_NULL) {
      std::int64_t playOption2Seed = 0;
      if (!readInteger64(statement.get(), 19, playOption2Seed,
                         "chart second play-option seed", diagnostic)) {
        return false;
      }
      chart.playOption2Seed = playOption2Seed;
    }
    chart.assistOption = columnText(statement.get(), 20);
    chart.createdAt = columnText(statement.get(), 21);
    chart.provenanceJson = columnText(statement.get(), 24);
    std::string provenanceError;
    auto provenance =
        deserializeScoreProvenance(chart.provenanceJson, provenanceError);
    if (!provenance.has_value() ||
        provenance->ruleset.version != rulesetVersion ||
        static_cast<int>(provenance->eligibility) != eligibility) {
      diagnostic = provenanceError.empty()
                       ? "legacy replay provenance columns disagree"
                       : std::move(provenanceError);
      return false;
    }
    chart.provenance = std::move(*provenance);
    if (sqlite3_column_type(statement.get(), 25) == SQLITE_TEXT) {
      chart.attemptId = columnText(statement.get(), 25);
    }
    charts.push_back(std::move(chart));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  for (auto &chart : charts) {
    if (!readEvents(database, chart, diagnostic)) {
      return false;
    }
    const auto resolved =
        resolveMetadata
            ? resolveMetadata({.chartPath = chart.chartPath,
                                           .chartMd5 = chart.chartMd5,
                               .chartSha256 = chart.chartSha256,
                               .longNoteMode = chart.longNoteMode,
                               .randomSeed = chart.randomSeed,
                               .randomPrng = chart.randomPrng,
                               .randomValues = chart.randomValues,
                               .playOption = chart.playOption,
                               .playOptionSeed = chart.playOptionSeed,
                               .playOption2 = chart.playOption2,
                               .playOption2Seed = chart.playOption2Seed})
                        : std::nullopt;
    if (!resolved.has_value()) {
      diagnostic =
          "authoritative chart metadata is required to resolve key mode";
      return false;
    }
    if (!resolved->resultEventTopologyComplete) {
      diagnostic =
          "authoritative chart topology is required to migrate long-note "
          "result events";
      return false;
    }
    chart.hasUndefinedLongNotes = resolved->hasUndefinedLongNotes;
    chart.classicLongNotes = resolved->classicLongNotes;
    if (!readPendingResult(database, chart, resolved->keyMode,
                           resolved->totalNotes, diagnostic) ||
        !buildPlayback(chart, diagnostic)) {
      return false;
    }
  }
  return true;
}

bool readCourseStages(sqlite3 *database, LegacyCourse &course,
                      const std::map<std::int64_t, std::size_t> &chartById,
                      std::vector<LegacyChart> &charts,
                      std::string &diagnostic) {
  Statement statement(
      database,
      "SELECT stage_index,replay_id,rest_micros_after_stage FROM "
      "course_replay_stages WHERE course_replay_id=? ORDER BY stage_index,id");
  if (!statement.valid() ||
      sqlite3_bind_int64(statement.get(), 1, course.id) != SQLITE_OK) {
    diagnostic = statement.error();
    return false;
  }
  int result = SQLITE_OK;
  int expectedIndex = 0;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    int stageIndex = 0;
    std::int64_t replayId = 0;
    std::int64_t restMicros = 0;
    if (!readInteger(statement.get(), 0, stageIndex, "course stage index",
                     diagnostic) ||
        !readInteger64(statement.get(), 1, replayId,
                       "course stage replay ID", diagnostic) ||
        !readInteger64(statement.get(), 2, restMicros,
                       "course stage rest duration", diagnostic)) {
      return false;
    }
    const auto chart = chartById.find(replayId);
    if (stageIndex != expectedIndex || chart == chartById.end() ||
        restMicros < 0) {
      diagnostic = "legacy course stage order or replay link is invalid";
      return false;
    }
    course.chartIndexes.push_back(chart->second);
    charts[chart->second].courseStage = true;
    course.restMicrosAfterStage.push_back(restMicros);
    ++expectedIndex;
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  if (expectedIndex != course.completedCharts || expectedIndex <= 0 ||
      course.completedCharts > course.totalCharts) {
    diagnostic = "legacy course completed stage count is invalid";
    return false;
  }
  return true;
}

bool buildCourse(LegacyCourse &course, const std::vector<LegacyChart> &charts,
                 std::string &diagnostic) {
  auto &persisted = course.result;
  persisted.resultId = static_cast<int>(course.id);
  persisted.courseKey = course.courseKey;
  persisted.legacyCourseId = course.legacyCourseId;
  persisted.courseName = course.courseName;
  persisted.courseGroupName = course.courseGroupName;
  persisted.constraintJson = course.constraintJson;
  persisted.completedCharts = course.completedCharts;
  persisted.totalCharts = course.totalCharts;
  persisted.requestedPlayOption = course.requestedPlayOption;
  persisted.assistOption = course.assistOption;
  persisted.initialGaugeType = course.initialGaugeType;
  persisted.gaugeProfile = course.gaugeProfile;
  persisted.gaugeAutoShift = course.gaugeAutoShift;
  persisted.gaugeAutoShiftLowerBound =
      course.provenance.gaugeAutoShiftLowerBound;
  persisted.longNoteMode = course.longNoteMode;
  persisted.finalScore = course.finalScore;
  persisted.maxCombo = course.maxCombo;
  persisted.finalGauge = course.finalGauge;
  persisted.clearType = course.clearType;
  persisted.provenance = course.provenance;
  persisted.playedAtUnixMillis = course.playedAtUnixMillis;
  persisted.entryFacts.resize(static_cast<std::size_t>(course.totalCharts));

  std::vector<course_identity::ChartIdentity> identities;
  identities.reserve(course.chartIndexes.size());
  replay::CoursePathInput pathInput;
  pathInput.longNoteMode = course.longNoteMode;
  pathInput.hasUndefinedLongNotes = false;
  pathInput.beatorajaConstraintIds =
      beatorajaCourseConstraintIds(course.constraintJson);
  for (std::size_t stageIndex = 0; stageIndex < course.chartIndexes.size();
       ++stageIndex) {
    const LegacyChart &chart = charts[course.chartIndexes[stageIndex]];
    identities.push_back({.sha256 = chart.chartSha256, .md5 = chart.chartMd5});
    pathInput.stageSha256.push_back(chart.chartSha256);
    pathInput.hasUndefinedLongNotes |= chart.hasUndefinedLongNotes;
    persisted.stages.push_back(
        {.stageIndex = static_cast<int>(stageIndex),
         .score = chart.result.score,
         .keyMode = chart.result.keyMode,
         .adoptedGaugeType = chart.result.adoptedGaugeType,
         .adoptedGaugeHistory = chart.result.adoptedGaugeHistory,
         .judgementTiming = chart.result.judgementTiming});
    persisted.entryFacts[stageIndex].totalNotes =
        chart.result.score.maxScore / 2;
    persisted.maxScore += chart.result.score.maxScore;
    course.playback.stages.push_back(chart.playback);
    course.playback.restMicrosAfterStage.push_back(
        course.restMicrosAfterStage[stageIndex]);
  }
  if (!normalizeResultProvenance(persisted.provenance, course.provenanceJson,
                                 diagnostic)) {
    return false;
  }
  if (persisted.courseKey.empty()) {
    persisted.courseKey =
        course_identity::makeCourseKey(identities, course.constraintJson);
    course.courseKey = persisted.courseKey;
  }
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  if (!result_persistence::validatePersistedCourseResult(persisted,
                                                         diagnostic)) {
    return false;
  }
  const auto stem = replay::courseStem(pathInput, diagnostic);
  if (!stem.has_value()) {
    return false;
  }
  course.path.stem = *stem;
  return true;
}

bool readCourses(sqlite3 *database, std::vector<LegacyChart> &charts,
                 std::vector<LegacyCourse> &courses, std::string &diagnostic) {
  std::map<std::int64_t, std::size_t> chartById;
  for (std::size_t index = 0; index < charts.size(); ++index) {
    chartById.emplace(charts[index].id, index);
  }
  Statement statement(
      database,
      "SELECT id,course_id,COALESCE(course_key,''),COALESCE(course_name,''),"
      "COALESCE(course_group_name,''),COALESCE(constraint_json,''),"
      "gauge_type,gauge_profile,gauge_auto_shift,ln_mode,"
      "COALESCE(requested_play_option,'NORMAL'),"
      "COALESCE(assist_option,'OFF'),final_score,max_combo,final_gauge,"
      "clear_type,completed_charts,total_charts,created_at,ruleset_version,"
      "eligibility,provenance_json,CASE WHEN typeof(created_at)='text' THEN "
      "COALESCE(CAST(strftime('%s',created_at) AS INTEGER)*1000,0) ELSE 0 END "
      "FROM course_replays ORDER BY id");
  if (!statement.valid()) {
    diagnostic = statement.error();
    return false;
  }
  int result = SQLITE_OK;
  while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
    LegacyCourse course;
    int initialGaugeType = 0;
    int gaugeProfile = 0;
    int gaugeAutoShift = 0;
    int rulesetVersion = 0;
    int eligibility = 0;
    if (!readInteger64(statement.get(), 0, course.id, "course public ID",
                       diagnostic) ||
        !readInteger(statement.get(), 1, course.legacyCourseId,
                     "course legacy ID", diagnostic) ||
        !readInteger(statement.get(), 6, initialGaugeType,
                     "course initial gauge type", diagnostic) ||
        !readInteger(statement.get(), 7, gaugeProfile,
                     "course gauge profile", diagnostic) ||
        !readInteger(statement.get(), 8, gaugeAutoShift,
                     "course gauge auto-shift", diagnostic) ||
        !readInteger(statement.get(), 9, course.longNoteMode,
                     "course long-note mode", diagnostic) ||
        !readInteger(statement.get(), 12, course.finalScore,
                     "course final score", diagnostic) ||
        !readInteger(statement.get(), 13, course.maxCombo,
                     "course maximum combo", diagnostic) ||
        !readInteger(statement.get(), 15, course.clearType,
                     "course clear type", diagnostic) ||
        !readInteger(statement.get(), 16, course.completedCharts,
                     "course completed chart count", diagnostic) ||
        !readInteger(statement.get(), 17, course.totalCharts,
                     "course total chart count", diagnostic) ||
        !readInteger(statement.get(), 19, rulesetVersion,
                     "course ruleset version", diagnostic) ||
        !readInteger(statement.get(), 20, eligibility,
                     "course eligibility", diagnostic) ||
        !readInteger64(statement.get(), 22, course.playedAtUnixMillis,
                       "course play time", diagnostic)) {
      return false;
    }
    if (course.id <= 0 || course.id > std::numeric_limits<int>::max()) {
      diagnostic = "legacy course public ID is invalid";
      return false;
    }
    course.courseKey = columnText(statement.get(), 2);
    course.courseName = columnText(statement.get(), 3);
    course.courseGroupName = columnText(statement.get(), 4);
    course.constraintJson = columnText(statement.get(), 5);
    if (!validGaugeTypeValue(initialGaugeType) ||
        !validGaugeProfileValue(gaugeProfile) ||
        !validGaugeAutoShiftValue(gaugeAutoShift)) {
      diagnostic = "legacy course gauge configuration is invalid";
      return false;
    }
    course.initialGaugeType = gaugeTypeAtIndex(initialGaugeType);
    course.gaugeProfile = gaugeProfileFromInt(gaugeProfile);
    course.gaugeAutoShift = gaugeAutoShiftModeFromValue(gaugeAutoShift);
    if (course.longNoteMode < 0 || course.longNoteMode > 3) {
      diagnostic = "legacy course long-note mode is invalid";
      return false;
    }
    course.requestedPlayOption = columnText(statement.get(), 10);
    course.assistOption = columnText(statement.get(), 11);
    if (!readFiniteFloat(statement.get(), 14, course.finalGauge,
                         "course final gauge", diagnostic)) {
      return false;
    }
    course.createdAt = columnText(statement.get(), 18);
    course.provenanceJson = columnText(statement.get(), 21);
    std::string provenanceError;
    auto provenance =
        deserializeScoreProvenance(course.provenanceJson, provenanceError);
    if (!provenance.has_value() ||
        provenance->ruleset.version != rulesetVersion ||
        static_cast<int>(provenance->eligibility) != eligibility) {
      diagnostic = provenanceError.empty()
                       ? "legacy course provenance columns disagree"
                       : std::move(provenanceError);
      return false;
    }
    course.provenance = std::move(*provenance);
    courses.push_back(std::move(course));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  for (auto &course : courses) {
    if (!readCourseStages(database, course, chartById, charts, diagnostic) ||
        !buildCourse(course, charts, diagnostic)) {
      return false;
    }
  }
  return true;
}

bool validateStandaloneChartResults(const std::vector<LegacyChart> &charts,
                                    std::string &diagnostic) {
  for (const auto &chart : charts) {
    if (!chart.courseStage && !result_persistence::validatePersistedChartResult(
                                  chart.result, diagnostic)) {
      return false;
    }
  }
  return true;
}

bool advanceHistoryIndex(std::int64_t &historyIndex, std::string &diagnostic) {
  if (historyIndex == std::numeric_limits<std::int64_t>::max()) {
    diagnostic = "Replay history index is exhausted";
    return false;
  }
  ++historyIndex;
  return true;
}

bool finalizeEncodedAtAvailablePath(
    std::string_view stem, std::int64_t &historyIndex,
    std::span<const std::byte> encoded,
    const replay::ExpectedReplayIdentity &expected, std::string_view token,
    const replay::BeatorajaReplayCodec &codec, replay::ReplayFileStore &store,
    std::set<std::filesystem::path> &assignedPaths,
    replay::ReplayPathIdentity &path, replay::ReplayFileMetadata &file,
    std::string &diagnostic) {
  file_checksum::Sha256 hash;
  hash.update(encoded);
  const std::string expectedHash = hash.finalHex();
  while (true) {
    const auto candidate = replay::pathForStem(stem, historyIndex, diagnostic);
    if (!candidate.has_value()) {
      return false;
    }
    if (assignedPaths.contains(candidate->relativePath)) {
      if (!advanceHistoryIndex(historyIndex, diagnostic)) {
        return false;
      }
      continue;
    }
    const replay::ReplayFileMetadata expectedMetadata{
        .relativePath = candidate->relativePath,
        .sha256 = expectedHash,
        .compressedSize = static_cast<std::uint64_t>(encoded.size()),
        .codecVersion = replay::BeatorajaReplayCodec::kCodecVersion,
    };
    const auto inspection = store.inspect(expectedMetadata);
    if (inspection.state == replay::ReplayFileState::Corrupt) {
      if (!advanceHistoryIndex(historyIndex, diagnostic)) {
        return false;
      }
      continue;
    }
    if (inspection.state == replay::ReplayFileState::Unsafe ||
        inspection.state == replay::ReplayFileState::IoFailure) {
      diagnostic = inspection.diagnostic.empty()
                       ? "Could not inspect migrated replay destination"
                       : inspection.diagnostic;
      return false;
    }
    const auto finalized =
        store.finalize(*candidate, encoded, codec, expected, token);
    if (!finalized.metadata.has_value()) {
      diagnostic = finalized.diagnostic.empty()
                       ? "Could not finalize migrated replay file"
                       : finalized.diagnostic;
      return false;
    }
    path = *candidate;
    file = *finalized.metadata;
    assignedPaths.insert(candidate->relativePath);
    return true;
  }
}

bool finalizeFiles(std::vector<LegacyChart> &charts,
                   std::vector<LegacyCourse> &courses,
                   const replay::BeatorajaReplayCodec &codec,
                   replay::ReplayFileStore &store,
                   const ReplayMigrationFaults &faults,
                   std::string &diagnostic) {
  std::set<std::filesystem::path> assignedPaths;
  std::vector<LegacyChart *> chartOrder;
  chartOrder.reserve(charts.size());
  for (auto &chart : charts) {
    if (!chart.courseStage) {
    chartOrder.push_back(&chart);
  }
  }
  std::ranges::sort(
      chartOrder, [](const LegacyChart *left, const LegacyChart *right) {
        return std::tie(left->path.stem, left->playedAtUnixMillis, left->id) <
               std::tie(right->path.stem, right->playedAtUnixMillis, right->id);
      });
  std::string previous;
  std::int64_t historyIndex = -1;
  for (LegacyChart *chart : chartOrder) {
    if (fault(faults, "encode", chart->id)) {
      diagnostic = "injected replay encode failure";
      return false;
    }
    const auto bytes = codec.encodeChart(chart->playback,
                                         chart->playedAtUnixMillis, diagnostic);
    if (!bytes.has_value()) {
      return false;
    }
    if (chart->path.stem == previous) {
      if (!advanceHistoryIndex(historyIndex, diagnostic)) {
        return false;
      }
    } else {
      historyIndex = 0;
    }
    previous = chart->path.stem;
    if (!finalizeEncodedAtAvailablePath(
            chart->path.stem, historyIndex, *bytes,
            {.stageSha256 = {chart->chartSha256},
             .stageLongNoteModes = {chart->playback.setup.longNoteMode},
             .course = false},
            "migration-chart-" + std::to_string(chart->id), codec, store,
            assignedPaths, chart->path, chart->file, diagnostic)) {
      return false;
    }
  }

  std::vector<LegacyCourse *> courseOrder;
  courseOrder.reserve(courses.size());
  for (auto &course : courses) {
    courseOrder.push_back(&course);
  }
  std::ranges::sort(
      courseOrder, [](const LegacyCourse *left, const LegacyCourse *right) {
        return std::tie(left->path.stem, left->playedAtUnixMillis, left->id) <
               std::tie(right->path.stem, right->playedAtUnixMillis, right->id);
      });
  previous.clear();
  historyIndex = -1;
  for (LegacyCourse *course : courseOrder) {
    if (fault(faults, "encode", course->id)) {
      diagnostic = "injected course replay encode failure";
      return false;
    }
    const auto bytes = codec.encodeCourse(
        course->playback, course->playedAtUnixMillis, diagnostic);
    if (!bytes.has_value()) {
      return false;
    }
    std::vector<std::string> stageSha256;
    std::vector<int> stageLongNoteModes;
    stageSha256.reserve(course->playback.stages.size());
    stageLongNoteModes.reserve(course->playback.stages.size());
    for (const auto &stage : course->playback.stages) {
      stageSha256.push_back(stage.setup.chartSha256);
      stageLongNoteModes.push_back(stage.setup.longNoteMode);
    }
    if (course->path.stem == previous) {
      if (!advanceHistoryIndex(historyIndex, diagnostic)) {
        return false;
      }
    } else {
      historyIndex = 0;
    }
    previous = course->path.stem;
    if (!finalizeEncodedAtAvailablePath(
            course->path.stem, historyIndex, *bytes,
            {.stageSha256 = std::move(stageSha256),
             .stageLongNoteModes = std::move(stageLongNoteModes),
             .course = true},
            "migration-course-" + std::to_string(course->id), codec, store,
            assignedPaths, course->path, course->file, diagnostic)) {
      return false;
    }
  }
  return true;
}

bool renameLegacyTables(sqlite3 *database, std::string &diagnostic) {
  constexpr std::string_view sql =
      "ALTER TABLE replay_events RENAME TO legacy_v10_replay_events;"
      "ALTER TABLE replay_touch_samples RENAME TO "
      "legacy_v10_replay_touch_samples;"
      "ALTER TABLE replay_lane_cover_events RENAME TO "
      "legacy_v10_replay_lane_cover_events;"
      "ALTER TABLE course_replay_stages RENAME TO "
      "legacy_v10_course_replay_stages;"
      "ALTER TABLE pending_chart_score_writes RENAME TO "
      "legacy_v10_pending_chart_score_writes;"
      "ALTER TABLE ir_submission_receipts RENAME TO "
      "legacy_v10_ir_submission_receipts;"
      "ALTER TABLE ir_outbox RENAME TO legacy_v10_ir_outbox;"
      "ALTER TABLE ir_remote_scores RENAME TO legacy_v10_ir_remote_scores;"
      "ALTER TABLE course_replays RENAME TO legacy_v10_course_replays;"
      "ALTER TABLE replays RENAME TO legacy_v10_replays;"
      "DROP INDEX idx_replays_attempt_id;"
      "DROP INDEX idx_replays_chart_sha256;"
      "DROP INDEX idx_replays_chart_md5;"
      "DROP INDEX idx_replays_chart_path;"
      "DROP INDEX idx_replay_events_replay_order;"
      "DROP INDEX idx_replay_touch_samples_replay_order;"
      "DROP INDEX idx_replay_lane_cover_events_replay_order;"
      "DROP INDEX idx_course_replays_course;"
      "DROP INDEX idx_course_replays_key_id;"
      "DROP INDEX idx_course_replay_stages_course_order;"
      "DROP INDEX idx_course_replay_stages_replay;"
      "DROP INDEX idx_pending_chart_score_created;"
      "DROP INDEX idx_ir_outbox_due;"
      "DROP INDEX idx_ir_outbox_attempt;"
      "DROP INDEX idx_ir_submission_receipts_attempt;"
      "DROP INDEX idx_ir_submission_receipts_remote_score;"
      "DROP INDEX idx_ir_remote_scores_chart_sha256;"
      "DROP INDEX idx_ir_remote_scores_remote_chart_id;";
  return execute(database, sql, diagnostic);
}

std::string
judgementTimingJson(const result_persistence::ChartJudgementTiming &timing) {
  nlohmann::ordered_json value = nlohmann::ordered_json::array();
  for (const auto &count : timing.byJudgement) {
    value.push_back(nlohmann::ordered_json::array({count.fast, count.slow}));
  }
  return value.dump();
}

bool insertChart(sqlite3 *database, const LegacyChart &chart,
                 std::string &diagnostic) {
  Statement result(
      database,
      "INSERT INTO chart_results(id,attempt_id,chart_path,chart_md5,"
      "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
      "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
      "fast,slow,final_gauge,clear_type,adopted_gauge_type,gauge_history_json,"
      "judgement_timing_json,provenance_json,result_fingerprint,"
      "played_at_unix_ms,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
      "?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,"
      "?25,?26,?27,?28,?29,?30)");
  if (!result.valid()) {
    diagnostic = result.error();
    return false;
  }
  const auto &persisted = chart.result;
  const auto &score = persisted.score;
  int column = 1;
  bool okay = sqlite3_bind_int64(result.get(), column++, chart.id) == SQLITE_OK;
  if (persisted.attemptId.has_value()) {
    okay = okay && bindText(result.get(), column++, *persisted.attemptId);
  } else {
    okay = okay && sqlite3_bind_null(result.get(), column++) == SQLITE_OK;
  }
  okay = okay && bindText(result.get(), column++, score.chartPath) &&
         bindText(result.get(), column++, score.chartMd5) &&
         bindText(result.get(), column++, score.chartSha256) &&
         bindText(result.get(), column++, score.chartTitle) &&
         bindText(result.get(), column++, score.chartArtist);
  const int values[] = {persisted.keyMode, score.longNoteMode, score.score,
                        score.maxScore,    score.maxCombo,     score.comboBreak,
                        score.pGreat,      score.great,        score.good,
                        score.bad,         score.poor,         score.kPoor,
                        score.fast,        score.slow};
  for (const int value : values) {
    okay = okay && sqlite3_bind_int(result.get(), column++, value) == SQLITE_OK;
  }
  const std::string history =
      nlohmann::ordered_json(persisted.adoptedGaugeHistory).dump();
  okay =
      okay &&
      sqlite3_bind_double(result.get(), column++, score.finalGauge) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, score.clearType) == SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++,
                       gaugeTypeIndex(persisted.adoptedGaugeType)) ==
          SQLITE_OK &&
      bindText(result.get(), column++, history);
  if (persisted.judgementTiming.has_value()) {
    okay = okay && bindText(result.get(), column++,
                            judgementTimingJson(*persisted.judgementTiming));
  } else {
    okay = okay && sqlite3_bind_null(result.get(), column++) == SQLITE_OK;
  }
  okay = okay && bindText(result.get(), column++, chart.provenanceJson) &&
      bindText(result.get(), column++, persisted.resultFingerprint) &&
      sqlite3_bind_int64(result.get(), column++,
                         persisted.playedAtUnixMillis) == SQLITE_OK &&
      bindText(result.get(), column++, chart.createdAt);
  if (!okay || column != 31 || sqlite3_step(result.get()) != SQLITE_DONE) {
    diagnostic = result.error();
    return false;
  }

  Statement file(
      database,
      "INSERT INTO replay_files(chart_result_id,course_result_id,stem,"
      "history_index,relative_path,content_sha256,compressed_size,"
      "codec_version) VALUES(?,NULL,?,?,?,?,?,?)");
  const std::string relative = chart.file.relativePath.generic_string();
  if (!file.valid() ||
      sqlite3_bind_int64(file.get(), 1, chart.id) != SQLITE_OK ||
      !bindText(file.get(), 2, chart.path.stem) ||
      sqlite3_bind_int64(file.get(), 3, chart.path.historyIndex) != SQLITE_OK ||
      !bindText(file.get(), 4, relative) ||
      !bindText(file.get(), 5, chart.file.sha256) ||
      sqlite3_bind_int64(
          file.get(), 6,
          static_cast<sqlite3_int64>(chart.file.compressedSize)) != SQLITE_OK ||
      sqlite3_bind_int(file.get(), 7, chart.file.codecVersion) != SQLITE_OK ||
      sqlite3_step(file.get()) != SQLITE_DONE) {
    diagnostic = file.error();
    return false;
  }
  return true;
}

bool insertCourse(sqlite3 *database, const LegacyCourse &course,
                  std::string &diagnostic) {
  Statement result(
      database,
      "INSERT INTO course_results(id,attempt_id,course_key,legacy_course_id,"
      "course_name,course_group_name,constraint_json,completed_charts,"
      "total_charts,requested_play_option,assist_option,initial_gauge_type,"
      "gauge_profile,gauge_auto_shift,gauge_auto_shift_lower_bound,"
      "long_note_mode,final_score,max_score,max_combo,final_gauge,clear_type,"
      "provenance_json,entry_facts_json,result_fingerprint,played_at_unix_ms,"
      "created_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!result.valid()) {
    diagnostic = result.error();
    return false;
  }
  const auto &persisted = course.result;
  nlohmann::ordered_json entryFacts = nlohmann::ordered_json::array();
  for (const auto &facts : persisted.entryFacts) {
    entryFacts.push_back(nlohmann::ordered_json::array(
        {facts.totalNotes, facts.playLengthMicros}));
  }
  const std::string serializedEntryFacts = entryFacts.dump();
  int column = 1;
  bool okay =
      sqlite3_bind_int64(result.get(), column++, course.id) == SQLITE_OK &&
      sqlite3_bind_null(result.get(), column++) == SQLITE_OK &&
      bindText(result.get(), column++, persisted.courseKey) &&
      sqlite3_bind_int(result.get(), column++, persisted.legacyCourseId) ==
          SQLITE_OK &&
      bindText(result.get(), column++, persisted.courseName) &&
      bindText(result.get(), column++, persisted.courseGroupName) &&
      bindText(result.get(), column++, persisted.constraintJson) &&
      sqlite3_bind_int(result.get(), column++, persisted.completedCharts) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.totalCharts) ==
          SQLITE_OK &&
      bindText(result.get(), column++, persisted.requestedPlayOption) &&
      bindText(result.get(), column++, persisted.assistOption) &&
      sqlite3_bind_int(result.get(), column++,
                       gaugeTypeIndex(persisted.initialGaugeType)) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++,
                       static_cast<int>(persisted.gaugeProfile)) == SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++,
                       gaugeAutoShiftModeValue(persisted.gaugeAutoShift)) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++,
                       gaugeTypeIndex(persisted.gaugeAutoShiftLowerBound)) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.longNoteMode) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.finalScore) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.maxScore) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.maxCombo) ==
          SQLITE_OK &&
      sqlite3_bind_double(result.get(), column++, persisted.finalGauge) ==
          SQLITE_OK &&
      sqlite3_bind_int(result.get(), column++, persisted.clearType) ==
          SQLITE_OK &&
      bindText(result.get(), column++, course.provenanceJson) &&
      bindText(result.get(), column++, serializedEntryFacts) &&
      bindText(result.get(), column++, persisted.resultFingerprint) &&
      sqlite3_bind_int64(result.get(), column++,
                         persisted.playedAtUnixMillis) == SQLITE_OK &&
      bindText(result.get(), column++, course.createdAt);
  if (!okay || column != 27 || sqlite3_step(result.get()) != SQLITE_DONE) {
    diagnostic = result.error();
    return false;
  }

  Statement stage(
      database,
      "INSERT INTO course_result_stages(course_result_id,stage_index,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,key_mode,"
      "long_note_mode,score,max_score,max_combo,combo_break,p_great,great,"
      "good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
      "adopted_gauge_type,gauge_history_json,judgement_timing_json,"
      "provenance_json) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!stage.valid()) {
    diagnostic = stage.error();
    return false;
  }
  for (const auto &persistedStage : persisted.stages) {
    sqlite3_reset(stage.get());
    sqlite3_clear_bindings(stage.get());
    const auto &score = persistedStage.score;
    const std::string history =
        nlohmann::ordered_json(persistedStage.adoptedGaugeHistory).dump();
    const std::string provenance = serializeScoreProvenance(score.provenance);
    column = 1;
    okay = sqlite3_bind_int64(stage.get(), column++, course.id) == SQLITE_OK &&
           sqlite3_bind_int(stage.get(), column++, persistedStage.stageIndex) ==
               SQLITE_OK &&
           bindText(stage.get(), column++, score.chartPath) &&
           bindText(stage.get(), column++, score.chartMd5) &&
           bindText(stage.get(), column++, score.chartSha256) &&
           bindText(stage.get(), column++, score.chartTitle) &&
           bindText(stage.get(), column++, score.chartArtist);
    const int values[] = {persistedStage.keyMode,
                          score.longNoteMode,
                          score.score,
                          score.maxScore,
                          score.maxCombo,
                          score.comboBreak,
                          score.pGreat,
                          score.great,
                          score.good,
                          score.bad,
                          score.poor,
                          score.kPoor,
                          score.fast,
                          score.slow};
    for (const int value : values) {
      okay =
          okay && sqlite3_bind_int(stage.get(), column++, value) == SQLITE_OK;
    }
    okay =
        okay &&
        sqlite3_bind_double(stage.get(), column++, score.finalGauge) ==
            SQLITE_OK &&
        sqlite3_bind_int(stage.get(), column++, score.clearType) == SQLITE_OK &&
        sqlite3_bind_int(stage.get(), column++,
                         gaugeTypeIndex(persistedStage.adoptedGaugeType)) ==
            SQLITE_OK &&
        bindText(stage.get(), column++, history);
    if (persistedStage.judgementTiming.has_value()) {
      okay = okay &&
             bindText(stage.get(), column++,
                      judgementTimingJson(*persistedStage.judgementTiming));
    } else {
      okay = okay && sqlite3_bind_null(stage.get(), column++) == SQLITE_OK;
    }
    okay = okay && bindText(stage.get(), column++, provenance);
    if (!okay || column != 28 || sqlite3_step(stage.get()) != SQLITE_DONE) {
      diagnostic = stage.error();
      return false;
    }
  }

  Statement file(
      database,
      "INSERT INTO replay_files(chart_result_id,course_result_id,stem,"
      "history_index,relative_path,content_sha256,compressed_size,"
      "codec_version) VALUES(NULL,?,?,?,?,?,?,?)");
  const std::string relative = course.file.relativePath.generic_string();
  if (!file.valid() ||
      sqlite3_bind_int64(file.get(), 1, course.id) != SQLITE_OK ||
      !bindText(file.get(), 2, course.path.stem) ||
      sqlite3_bind_int64(file.get(), 3, course.path.historyIndex) !=
          SQLITE_OK ||
      !bindText(file.get(), 4, relative) ||
      !bindText(file.get(), 5, course.file.sha256) ||
      sqlite3_bind_int64(file.get(), 6,
                         static_cast<sqlite3_int64>(
                             course.file.compressedSize)) != SQLITE_OK ||
      sqlite3_bind_int(file.get(), 7, course.file.codecVersion) != SQLITE_OK ||
      sqlite3_step(file.get()) != SQLITE_DONE) {
    diagnostic = file.error();
    return false;
  }
  return true;
}

bool copyDurableWork(sqlite3 *database, std::string &diagnostic) {
  constexpr std::string_view sql =
      "INSERT INTO pending_chart_score_writes("
      "attempt_id,result_id,chart_path,chart_md5,chart_sha256,chart_title,"
      "chart_artist,ln_mode,score,max_score,max_combo,combo_break,pgreat,"
      "great,good,bad,poor,kpoor,fast,slow,final_gauge,clear_type,"
      "ruleset_version,eligibility,provenance_json,created_at,"
      "recovery_attempts,last_recovery_at) SELECT p.attempt_id,p.replay_id,"
      "p.chart_path,p.chart_md5,p.chart_sha256,p.chart_title,p.chart_artist,"
      "p.ln_mode,p.score,p.max_score,p.max_combo,p.combo_break,p.pgreat,"
      "p.great,p.good,p.bad,p.poor,p.kpoor,p.fast,p.slow,p.final_gauge,"
      "p.clear_type,CAST(json_extract(r.provenance_json,'$.ruleset.version') "
      "AS INTEGER),CASE json_extract(r.provenance_json,'$.eligibility') "
      "WHEN 'verified' THEN 0 WHEN 'modified' THEN 1 ELSE 2 END,"
      "r.provenance_json,p.created_at,p.recovery_attempts,p.last_recovery_at "
      "FROM legacy_v10_pending_chart_score_writes p JOIN chart_results r "
      "ON r.id=p.replay_id;"
      "INSERT INTO ir_outbox SELECT * FROM legacy_v10_ir_outbox;"
      "INSERT INTO ir_submission_receipts("
      "id,provider_id,server_origin,result_id,attempt_id,chart_md5,"
      "chart_sha256,remote_user_id,remote_chart_id,remote_score_id,"
      "confirmation_source,observed_in_snapshot,confirmed_at_ms) SELECT "
      "id,provider_id,server_origin,replay_id,attempt_id,chart_md5,"
      "chart_sha256,remote_user_id,remote_chart_id,remote_score_id,"
      "confirmation_source,observed_in_snapshot,confirmed_at_ms FROM "
      "legacy_v10_ir_submission_receipts;"
      "INSERT INTO ir_remote_scores SELECT * FROM "
      "legacy_v10_ir_remote_scores;";
  return execute(database, sql, diagnostic);
}

bool verifyCounts(sqlite3 *database, std::size_t chartCount,
                  std::size_t courseCount, std::size_t courseStageCount,
                  std::string &diagnostic) {
  Statement statement(
      database, "SELECT (SELECT count(*) FROM chart_results),"
                "(SELECT count(*) FROM course_results),"
                "(SELECT count(*) FROM replay_files),"
                "(SELECT count(*) FROM course_result_stages),"
                "(SELECT count(*) FROM legacy_v10_pending_chart_score_writes),"
                "(SELECT count(*) FROM pending_chart_score_writes),"
                "(SELECT count(*) FROM legacy_v10_ir_outbox),"
                "(SELECT count(*) FROM ir_outbox),"
                "(SELECT count(*) FROM legacy_v10_ir_submission_receipts),"
                "(SELECT count(*) FROM ir_submission_receipts),"
                "(SELECT count(*) FROM legacy_v10_ir_remote_scores),"
                "(SELECT count(*) FROM ir_remote_scores)");
  if (!statement.valid() || sqlite3_step(statement.get()) != SQLITE_ROW) {
    diagnostic = statement.error();
    return false;
  }
  if (sqlite3_column_int64(statement.get(), 0) !=
          static_cast<sqlite3_int64>(chartCount) ||
      sqlite3_column_int64(statement.get(), 1) !=
          static_cast<sqlite3_int64>(courseCount) ||
      sqlite3_column_int64(statement.get(), 2) !=
          static_cast<sqlite3_int64>(chartCount + courseCount) ||
      sqlite3_column_int64(statement.get(), 3) !=
          static_cast<sqlite3_int64>(courseStageCount)) {
    diagnostic = "migrated result or replay file count differs";
    return false;
  }
  for (int column = 4; column < 12; column += 2) {
    if (sqlite3_column_int64(statement.get(), column) !=
        sqlite3_column_int64(statement.get(), column + 1)) {
      diagnostic = "migrated durable work count differs";
      return false;
    }
  }
  return sqlite3_step(statement.get()) == SQLITE_DONE;
}

bool foreignKeysClean(sqlite3 *database, std::string &diagnostic) {
  Statement statement(database, "PRAGMA foreign_key_check");
  if (!statement.valid()) {
    diagnostic = statement.error();
    return false;
  }
  const int result = sqlite3_step(statement.get());
  if (result == SQLITE_DONE) {
    return true;
  }
  diagnostic = result == SQLITE_ROW ? "migrated foreign keys are invalid"
                                    : statement.error();
  return false;
}

bool dropLegacyTables(sqlite3 *database, std::string &diagnostic) {
  constexpr std::string_view sql =
      "DROP TABLE legacy_v10_replay_events;"
      "DROP TABLE legacy_v10_replay_touch_samples;"
      "DROP TABLE legacy_v10_replay_lane_cover_events;"
      "DROP TABLE legacy_v10_course_replay_stages;"
      "DROP TABLE legacy_v10_pending_chart_score_writes;"
      "DROP TABLE legacy_v10_ir_submission_receipts;"
      "DROP TABLE legacy_v10_ir_outbox;"
      "DROP TABLE legacy_v10_ir_remote_scores;"
      "DROP TABLE legacy_v10_course_replays;"
      "DROP TABLE legacy_v10_replays;";
  return execute(database, sql, diagnostic);
}

} // namespace

void setReplayMigrationChartTopologyResolver(
    ReplayMigrationChartTopologyResolver resolver) {
  std::lock_guard lock(topologyResolverMutex);
  topologyResolver = std::move(resolver);
}

std::optional<replay::LogicalControl>
legacyReplayControlForPhysicalLane(int physicalLane, int keyMode) noexcept {
  const auto lane = [](int player, int logicalLane) {
    return replay::LogicalControl{.kind = replay::LogicalControlKind::Lane,
                                  .player = player,
                                  .lane = logicalLane};
  };
  const auto scratch = [](int player) {
    return replay::LogicalControl{
        .kind = replay::LogicalControlKind::ScratchClockwise,
        .player = player,
        .lane = -1};
  };
  if (physicalLane < 0) {
    return std::nullopt;
  }
  switch (keyMode) {
  case 5:
    if (physicalLane < 5) {
      return lane(1, physicalLane);
    }
    return physicalLane == 7 ? std::optional<replay::LogicalControl>(scratch(1))
               : std::nullopt;
  case 7:
    if (physicalLane < 7) {
      return lane(1, physicalLane);
    }
    return physicalLane == 7 ? std::optional<replay::LogicalControl>(scratch(1))
               : std::nullopt;
  case 9:
    return physicalLane < 9
               ? std::optional<replay::LogicalControl>(lane(1, physicalLane))
               : std::nullopt;
  case 10:
    if (physicalLane < 5) {
      return lane(1, physicalLane);
    }
    if (physicalLane == 7) {
      return scratch(1);
    }
    if (physicalLane >= 8 && physicalLane < 13) {
      return lane(2, physicalLane - 8);
    }
    return physicalLane == 15
               ? std::optional<replay::LogicalControl>(scratch(2))
               : std::nullopt;
  case 14:
    if (physicalLane < 7) {
      return lane(1, physicalLane);
    }
    if (physicalLane == 7) {
      return scratch(1);
    }
    if (physicalLane >= 8 && physicalLane < 15) {
      return lane(2, physicalLane - 8);
    }
    return physicalLane == 15
               ? std::optional<replay::LogicalControl>(scratch(2))
               : std::nullopt;
  case 24:
    return physicalLane < 26
               ? std::optional<replay::LogicalControl>(lane(1, physicalLane))
               : std::nullopt;
  case 48:
    if (physicalLane < 26) {
      return lane(1, physicalLane);
    }
    return physicalLane < 52 ? std::optional<replay::LogicalControl>(
                     lane(2, physicalLane - 26))
               : std::nullopt;
  default:
    return std::nullopt;
  }
}

ReplayMigrationChartMetadataResolver makeChartDatabaseReplayMetadataResolver(
    const std::filesystem::path &chartDatabasePath) {
  if (chartDatabasePath.empty()) {
    return {};
  }
  return [chartDatabasePath](const ReplayMigrationChartIdentity &identity)
             -> std::optional<ReplayMigrationChartMetadata> {
    sqlite3 *rawDatabase = nullptr;
    const std::string pathText = fspath_to_utf8(chartDatabasePath);
    const int openResult = sqlite3_open_v2(
        pathText.c_str(), &rawDatabase,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, nullptr);
    SqliteConnectionHandle database(rawDatabase);
    if (openResult != SQLITE_OK || !database) {
      return std::nullopt;
    }

    Statement statement(
        database.get(),
        "SELECT path,lower(trim(md5)),lower(trim(sha256)),keys,ln_mode,"
        "total_long_notes,total_backspin_notes,total_notes FROM chart_meta "
        "WHERE path=?1 "
        "OR (?2<>'' AND lower(trim(md5))=?2) OR "
        "(?3<>'' AND lower(trim(sha256))=?3)");
    if (!statement.valid() ||
        !bindText(statement.get(), 1, identity.chartPath) ||
        !bindText(statement.get(), 2, identity.chartMd5) ||
        !bindText(statement.get(), 3, identity.chartSha256)) {
      return std::nullopt;
    }

    std::optional<ReplayMigrationChartMetadata> resolvedComplete;
    std::optional<ReplayMigrationChartMetadata> resolvedIncomplete;
    bool incompleteConflict = false;
    int stepResult = SQLITE_OK;
    while ((stepResult = sqlite3_step(statement.get())) == SQLITE_ROW) {
      const std::string rowPath = columnText(statement.get(), 0);
      const std::string rowMd5 = columnText(statement.get(), 1);
      const std::string rowSha256 = columnText(statement.get(), 2);
      const bool identityMatches =
          !identity.chartSha256.empty()
              ? rowSha256 == identity.chartSha256 &&
                    (identity.chartMd5.empty() || rowMd5 == identity.chartMd5)
          : !identity.chartMd5.empty() ? rowMd5 == identity.chartMd5
                    : rowPath == identity.chartPath;
      if (!identityMatches) {
        continue;
      }
      int keyMode = 0;
      int longNoteMode = 0;
      int totalLongNotes = 0;
      int totalBackspinNotes = 0;
      int totalNotes = 0;
      std::string numericDiagnostic;
      if (!readInteger(statement.get(), 3, keyMode, "chart key mode",
                       numericDiagnostic) ||
          !readInteger(statement.get(), 4, longNoteMode,
                       "chart long-note mode", numericDiagnostic) ||
          !readInteger(statement.get(), 5, totalLongNotes,
                       "chart long-note count", numericDiagnostic) ||
          !readInteger(statement.get(), 6, totalBackspinNotes,
                       "chart backspin count", numericDiagnostic) ||
          !readInteger(statement.get(), 7, totalNotes, "chart note count",
                       numericDiagnostic)) {
        return std::nullopt;
      }
      if (!supportedKeyMode(keyMode) || longNoteMode < 0 || longNoteMode > 3 ||
          totalLongNotes < 0 || totalBackspinNotes < 0 || totalNotes <= 0 ||
          totalNotes > std::numeric_limits<int>::max() / 2) {
        return std::nullopt;
      }
      ReplayMigrationChartMetadata metadata{
          .keyMode = keyMode,
          .hasUndefinedLongNotes =
              (totalLongNotes > 0 || totalBackspinNotes > 0) &&
              longNoteMode == 0,
          .totalNotes = totalNotes,
          .resultEventTopologyComplete =
              totalLongNotes == 0 && totalBackspinNotes == 0,
      };
      if (totalLongNotes > 0 || totalBackspinNotes > 0) {
        ReplayMigrationChartTopologyResolver resolveTopology;
        {
          std::lock_guard lock(topologyResolverMutex);
          resolveTopology = topologyResolver;
        }
        ReplayMigrationChartIdentity topologyIdentity = identity;
        topologyIdentity.chartPath = rowPath;
        const auto classicLongNotes =
            resolveTopology ? resolveTopology(topologyIdentity) : std::nullopt;
        metadata.resultEventTopologyComplete = classicLongNotes.has_value();
        if (classicLongNotes.has_value()) {
          metadata.classicLongNotes = *classicLongNotes;
        }
      }
      if (metadata.resultEventTopologyComplete) {
        if (resolvedComplete.has_value() && *resolvedComplete != metadata) {
          return std::nullopt;
        }
        resolvedComplete = std::move(metadata);
      } else {
        if (resolvedIncomplete.has_value() &&
            *resolvedIncomplete != metadata) {
          incompleteConflict = true;
        } else {
          resolvedIncomplete = std::move(metadata);
        }
      }
    }
    if (stepResult != SQLITE_DONE) {
      return std::nullopt;
    }
    if (resolvedComplete.has_value()) {
      return resolvedComplete;
    }
    return incompleteConflict ? std::nullopt : resolvedIncomplete;
  };
}

bool compactReplaySchemaHasNoLegacyPayloadTables(sqlite3 *database) {
  if (database == nullptr) {
    return false;
  }
  Statement statement(
      database,
      "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
      "('replay_events','replay_touch_samples','replay_lane_cover_events')");
  return statement.valid() && sqlite3_step(statement.get()) == SQLITE_ROW &&
         sqlite3_column_int64(statement.get(), 0) == 0 &&
         sqlite3_step(statement.get()) == SQLITE_DONE;
}

ReplayMigrationOutcome migrateReplaySchema10To11(
    sqlite3 *database, const std::filesystem::path &profileRoot,
    const replay::BeatorajaReplayCodec &codec,
    replay::ReplayFileStore &fileStore, ReplayMigrationFaults faults,
    ReplayMigrationChartMetadataResolver resolveMetadata) {
  if (database == nullptr || profileRoot.empty()) {
    return failure(MigrationStatus::StorageFailure,
                   "replay migration database or profile root is missing");
  }

  // Acquire the migration lock before classifying the schema version. A
  // second repository may have observed v10 while another migrator was still
  // running; after it obtains this lock, the database may already be current.
  std::string transactionError;
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return failure(MigrationStatus::StorageFailure,
                   "could not lock replay migration source: " +
                       transactionError);
  }
  int userVersion = -1;
  {
    Statement version(database, "PRAGMA user_version");
    if (!version.valid() || sqlite3_step(version.get()) != SQLITE_ROW) {
      return failure(MigrationStatus::StorageFailure,
                     "could not read replay schema version");
    }
    userVersion = sqlite3_column_int(version.get(), 0);
    if (sqlite3_step(version.get()) != SQLITE_DONE) {
      return failure(MigrationStatus::StorageFailure,
                     "replay schema version query is malformed");
    }
  }
  if (userVersion == 11 || userVersion == 12 || userVersion == 13 ||
      userVersion == 14) {
    return {.status = MigrationStatus::AlreadyCurrent};
  }
  if (userVersion != 10) {
    return failure(MigrationStatus::InvalidLegacyData,
                   "only replay schema version 10 can be migrated");
  }

  if (fault(faults, "legacy-read")) {
    return failure(MigrationStatus::InvalidLegacyData,
                   "injected legacy replay read failure");
  }
  std::vector<LegacyChart> charts;
  std::vector<LegacyCourse> courses;
  std::string diagnostic;
  if (!readCharts(database, charts, resolveMetadata, diagnostic) ||
      !readCourses(database, charts, courses, diagnostic) ||
      !validateStandaloneChartResults(charts, diagnostic)) {
    return failure(MigrationStatus::InvalidLegacyData,
                   diagnostic.empty() ? "legacy replay rows are invalid"
                                      : std::move(diagnostic));
  }
  const std::size_t standaloneChartCount =
      static_cast<std::size_t>(std::ranges::count_if(
          charts, [](const LegacyChart &chart) { return !chart.courseStage; }));
  if (!finalizeFiles(charts, courses, codec, fileStore, faults, diagnostic)) {
    return failure(MigrationStatus::FileFailure,
                   diagnostic.empty() ? "could not finalize replay files"
                                      : std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }

  if (fault(faults, "pre-cutover-revalidation")) {
    return failure(MigrationStatus::StorageFailure,
                   "injected pre-cutover validation failure",
                   standaloneChartCount, courses.size());
  }
  for (const auto &chart : charts) {
    if (chart.courseStage) {
      continue;
    }
    const auto inspected = fileStore.inspect(chart.file);
    if (inspected.state != replay::ReplayFileState::Available ||
        !inspected.metadata.has_value() || *inspected.metadata != chart.file) {
      return failure(MigrationStatus::FileFailure,
                     inspected.diagnostic.empty()
                         ? "migrated replay changed before cutover"
                         : inspected.diagnostic,
                     standaloneChartCount);
    }
  }
  for (const auto &course : courses) {
    const auto inspected = fileStore.inspect(course.file);
    if (inspected.state != replay::ReplayFileState::Available ||
        !inspected.metadata.has_value() || *inspected.metadata != course.file) {
      return failure(MigrationStatus::FileFailure,
                     inspected.diagnostic.empty()
                         ? "migrated course replay changed before cutover"
                         : inspected.diagnostic,
                     standaloneChartCount, courses.size());
    }
  }

  if (fault(faults, "begin")) {
    return failure(MigrationStatus::StorageFailure,
                   "injected migration transaction failure",
                   standaloneChartCount, courses.size());
  }
  if (!renameLegacyTables(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure, std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }
  if (fault(faults, "schema-create") ||
      !CreateCompactReplaySchema11OnConnection(database)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not create compact replay schema",
                   standaloneChartCount, courses.size());
  }
  for (const auto &chart : charts) {
    if (chart.courseStage) {
      continue;
    }
    if (fault(faults, "copy-chart", chart.id) ||
        !insertChart(database, chart, diagnostic)) {
      return failure(MigrationStatus::StorageFailure,
                     diagnostic.empty() ? "could not copy chart result"
                                        : std::move(diagnostic),
                     standaloneChartCount, courses.size());
    }
  }
  for (const auto &course : courses) {
    if (fault(faults, "copy-course", course.id) ||
        !insertCourse(database, course, diagnostic)) {
      return failure(MigrationStatus::StorageFailure,
                     diagnostic.empty() ? "could not copy course result"
                                        : std::move(diagnostic),
                     standaloneChartCount, courses.size());
    }
  }
  if (fault(faults, "copy-durable-work") ||
      !copyDurableWork(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "could not copy durable work"
                                      : std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }
  std::size_t courseStageCount = 0;
  for (const auto &course : courses) {
    courseStageCount += course.result.stages.size();
  }
  if (fault(faults, "count-verification") ||
      !verifyCounts(database, standaloneChartCount, courses.size(),
                    courseStageCount, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "migrated counts are invalid"
                                      : std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }
  if (fault(faults, "foreign-key-verification") ||
      !foreignKeysClean(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "migrated foreign keys are invalid"
                                      : std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }
  if (fault(faults, "legacy-drop") || !dropLegacyTables(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "could not drop legacy replay rows"
                                      : std::move(diagnostic),
                   standaloneChartCount, courses.size());
  }
  if (fault(faults, "version-update") ||
      !execute(database, "PRAGMA user_version=14", diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not advance replay schema version",
                   standaloneChartCount, courses.size());
  }
  if (fault(faults, "commit") || !transaction.commit(transactionError)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not commit replay schema cutover",
                   standaloneChartCount, courses.size());
  }
  return {.status = MigrationStatus::Migrated,
          .chartFiles = standaloneChartCount,
          .courseFiles = courses.size()};
}

} // namespace replay_repository_detail
