#include "ReplayRepositoryReplayFileMigration.h"

#include "ReplayRepositoryInternal.h"
#include "SqliteRAII.h"
#include "../CourseConstraintUtils.h"
#include "../CourseIdentity.h"
#include "../ResultPersistenceModel.h"
#include "../ScoreProvenance.h"
#include "../replay/BeatorajaReplayCodec.h"
#include "../replay/BeatorajaReplayPath.h"
#include "../replay/ReplayFileStore.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
  std::string validationError;
  if (const auto canonical =
          serializeValidatedScoreProvenance(provenance, validationError)) {
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
      sourceSchemaVersion >= ScoreProvenance::kSchemaVersion) {
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

std::vector<int> parseIntegers(std::string_view source) {
  std::vector<int> values;
  std::size_t begin = 0;
  while (begin <= source.size()) {
    const std::size_t end = source.find(',', begin);
    const std::string token(source.substr(begin, end == std::string_view::npos
                                                     ? source.size() - begin
                                                     : end - begin));
    if (!token.empty()) {
      char *tail = nullptr;
      const long value = std::strtol(token.c_str(), &tail, 10);
      if (tail != token.c_str() && *tail == '\0' &&
          value >= std::numeric_limits<int>::min() &&
          value <= std::numeric_limits<int>::max()) {
        values.push_back(static_cast<int>(value));
      }
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return values;
}

int inferredKeyMode(const LegacyChart &chart) {
  int maximumLane = -1;
  for (const auto &entry : chart.events) {
    if (entry.event.action == replay::LegacyPlaybackAction::Press ||
        entry.event.action == replay::LegacyPlaybackAction::Release) {
      maximumLane = std::max(maximumLane, entry.event.lane);
    }
  }
  return maximumLane > 7 ? 14 : 7;
}

GaugeProfile gaugeProfileFromInt(int value) {
  if (value < static_cast<int>(GaugeProfile::Standard) ||
      value > static_cast<int>(GaugeProfile::Standard24Keys)) {
    return GaugeProfile::Standard;
  }
  return static_cast<GaugeProfile>(value);
}

std::optional<replay::LogicalControl> legacyControl(int lane, int keyMode,
                                                    bool &scratch) {
  scratch = false;
  if (keyMode == 7) {
    if (lane >= 0 && lane < 7) {
      return replay::LogicalControl{
          .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = lane};
    }
    if (lane == 7 || lane == 8) {
      scratch = true;
      return replay::LogicalControl{
          .kind = replay::LogicalControlKind::ScratchClockwise,
          .player = 1,
          .lane = -1};
    }
    return std::nullopt;
  }
  if (lane >= 0 && lane < 7) {
    return replay::LogicalControl{
        .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = lane};
  }
  if (lane == 7 || lane == 8) {
    scratch = true;
    return replay::LogicalControl{
        .kind = replay::LogicalControlKind::ScratchClockwise,
        .player = 1,
        .lane = -1};
  }
  if (lane >= 9 && lane < 16) {
    return replay::LogicalControl{.kind = replay::LogicalControlKind::Lane,
                                  .player = 2,
                                  .lane = lane - 9};
  }
  if (lane == 16 || lane == 17) {
    scratch = true;
    return replay::LogicalControl{
        .kind = replay::LogicalControlKind::ScratchClockwise,
        .player = 2,
        .lane = -1};
  }
  return std::nullopt;
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
    entry.eventIndex = sqlite3_column_int(statement.get(), 0);
    entry.event.action = static_cast<replay::LegacyPlaybackAction>(
        std::clamp(sqlite3_column_int(statement.get(), 1), 0, 5));
    entry.event.lane = sqlite3_column_int(statement.get(), 2);
    entry.event.noteTimeMicros = sqlite3_column_int64(statement.get(), 3);
    entry.event.songTimeMicros = sqlite3_column_int64(statement.get(), 4);
    entry.event.judgeTimeMicros = sqlite3_column_int64(statement.get(), 5);
    const int judgement = sqlite3_column_int(statement.get(), 6);
    entry.event.judgement = judgement >= PGreat && judgement <= None
                                ? static_cast<Judgement>(judgement)
                                : None;
    entry.event.diffMicros = sqlite3_column_int64(statement.get(), 7);
    entry.event.gauge =
        static_cast<float>(sqlite3_column_double(statement.get(), 8));
    entry.event.gaugeType =
        gaugeTypeAtIndex(sqlite3_column_int(statement.get(), 9));
    entry.event.combo = sqlite3_column_int(statement.get(), 10);
    entry.event.score = sqlite3_column_int(statement.get(), 11);
    chart.events.push_back(std::move(entry));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }

  Statement touch(
      database,
      "SELECT action,finger_id,song_time_micros,x,y FROM "
      "replay_touch_samples WHERE replay_id=? ORDER BY sample_index,id");
  if (!touch.valid() ||
      sqlite3_bind_int64(touch.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = touch.error();
    return false;
  }
  while ((result = sqlite3_step(touch.get())) == SQLITE_ROW) {
    chart.touchSamples.push_back(
        {.action = static_cast<replay::ReplayTouchAction>(
             std::clamp(sqlite3_column_int(touch.get(), 0), 0, 3)),
         .fingerId = sqlite3_column_int64(touch.get(), 1),
         .songTimeMicros = sqlite3_column_int64(touch.get(), 2),
         .x = static_cast<float>(sqlite3_column_double(touch.get(), 3)),
         .y = static_cast<float>(sqlite3_column_double(touch.get(), 4))});
  }
  if (result != SQLITE_DONE) {
    diagnostic = touch.error();
    return false;
  }
  std::ranges::stable_sort(chart.touchSamples, {},
                           &replay::ReplayTouchSample::songTimeMicros);

  Statement cover(
      database,
      "SELECT song_time_micros,note_start_position_percent,"
      "reset_visible_time_reference FROM replay_lane_cover_events WHERE "
      "replay_id=? ORDER BY event_index,id");
  if (!cover.valid() ||
      sqlite3_bind_int64(cover.get(), 1, chart.id) != SQLITE_OK) {
    diagnostic = cover.error();
    return false;
  }
  while ((result = sqlite3_step(cover.get())) == SQLITE_ROW) {
    chart.laneCoverEvents.push_back(
        {.songTimeMicros = sqlite3_column_int64(cover.get(), 0),
         .noteStartPositionPercent = sqlite3_column_int(cover.get(), 1),
         .resetVisibleTimeReference = sqlite3_column_int(cover.get(), 2) != 0});
  }
  if (result != SQLITE_DONE) {
    diagnostic = cover.error();
    return false;
  }
  std::ranges::stable_sort(chart.laneCoverEvents, {},
                           &replay::ReplayLaneCoverEvent::songTimeMicros);
  return true;
}

bool readPendingResult(sqlite3 *database, LegacyChart &chart,
                       std::string &diagnostic) {
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
  persisted.keyMode = inferredKeyMode(chart);
  persisted.playedAtUnixMillis = chart.playedAtUnixMillis;
  if (result == SQLITE_ROW) {
    persisted.attemptId = columnText(statement.get(), 0);
    persisted.score.chartPath = columnText(statement.get(), 1);
    persisted.score.chartMd5 = lowerHex(columnText(statement.get(), 2));
    persisted.score.chartSha256 = lowerHex(columnText(statement.get(), 3));
    persisted.score.chartTitle = columnText(statement.get(), 4);
    persisted.score.chartArtist = columnText(statement.get(), 5);
    persisted.score.longNoteMode = sqlite3_column_int(statement.get(), 6);
    persisted.score.score = sqlite3_column_int(statement.get(), 7);
    persisted.score.maxScore = sqlite3_column_int(statement.get(), 8);
    persisted.score.maxCombo = sqlite3_column_int(statement.get(), 9);
    persisted.score.comboBreak = sqlite3_column_int(statement.get(), 10);
    persisted.score.pGreat = sqlite3_column_int(statement.get(), 11);
    persisted.score.great = sqlite3_column_int(statement.get(), 12);
    persisted.score.good = sqlite3_column_int(statement.get(), 13);
    persisted.score.bad = sqlite3_column_int(statement.get(), 14);
    persisted.score.poor = sqlite3_column_int(statement.get(), 15);
    persisted.score.kPoor = sqlite3_column_int(statement.get(), 16);
    persisted.score.fast = sqlite3_column_int(statement.get(), 17);
    persisted.score.slow = sqlite3_column_int(statement.get(), 18);
    persisted.score.finalGauge =
        static_cast<float>(sqlite3_column_double(statement.get(), 19));
    persisted.score.clearType = sqlite3_column_int(statement.get(), 20);
    std::string provenanceError;
    const std::string pendingProvenance = columnText(statement.get(), 23);
    auto provenance =
        deserializeScoreProvenance(pendingProvenance, provenanceError);
    if (!provenance.has_value() || pendingProvenance != chart.provenanceJson) {
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
  } else if (result == SQLITE_DONE) {
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
    int previousScore = 0;
    for (const auto &entry : chart.events) {
      const auto &event = entry.event;
      const int scoreDelta = event.score - previousScore;
      previousScore = event.score;
      if (event.action == replay::LegacyPlaybackAction::Gauge ||
          event.action == replay::LegacyPlaybackAction::Mine ||
          event.judgement == None) {
        continue;
      }
      bool countsInResult = true;
      switch (event.judgement) {
      case PGreat:
        countsInResult = scoreDelta == 2;
        score.pGreat += countsInResult ? 1 : 0;
        break;
      case Great:
        countsInResult = scoreDelta == 1;
        score.great += countsInResult ? 1 : 0;
        break;
      case Good:
        ++score.good;
        break;
      case Bad:
        ++score.bad;
        ++score.comboBreak;
        break;
      case Poor:
        ++score.poor;
        ++score.comboBreak;
        break;
      case Kpoor:
        ++score.kPoor;
        break;
      case None:
      case JudgementCount:
        countsInResult = false;
        break;
      }
      if (countsInResult && event.diffMicros < 0) {
        ++score.fast;
      } else if (countsInResult && event.diffMicros > 0) {
        ++score.slow;
      }
    }
    const int judged = score.pGreat + score.great + score.good + score.bad +
                       score.poor + score.kPoor;
    const std::int64_t minimumMaxScore =
        std::max({2LL, static_cast<std::int64_t>(score.score),
                  static_cast<std::int64_t>(score.maxCombo) * 2,
                  static_cast<std::int64_t>(judged) * 2});
    if (minimumMaxScore > std::numeric_limits<int>::max() - 1LL) {
      diagnostic = "legacy replay result counters exceed the supported range";
      return false;
    }
    score.maxScore = static_cast<int>(minimumMaxScore);
    if ((score.maxScore % 2) != 0) {
      ++score.maxScore;
    }
  } else {
    diagnostic = statement.error();
    return false;
  }

  for (const auto &entry : chart.events) {
    if (std::isfinite(entry.event.gauge)) {
      persisted.adoptedGaugeHistory.push_back(entry.event.gauge);
    }
  }
  if (!normalizeResultProvenance(persisted.score.provenance,
                                 chart.provenanceJson, diagnostic)) {
    return false;
  }
  persisted.resultFingerprint =
      result_persistence::resultFingerprint(persisted);
  if (!result_persistence::validatePersistedChartResult(persisted,
                                                        diagnostic)) {
    return false;
  }
  return true;
}

bool buildPlayback(LegacyChart &chart, std::string &diagnostic) {
  auto &setup = chart.playback.setup;
  setup.chartMd5 = chart.chartMd5;
  setup.chartSha256 = chart.chartSha256;
  setup.keyMode = chart.result.keyMode;
  setup.longNoteMode = std::clamp(chart.longNoteMode, 0, 2);
  setup.hasUndefinedLongNotes = true;
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
  setup.startingGaugePercent =
      static_cast<float>(chart.provenance.startingGaugePercent.value_or(20));
  setup.clubMode = chart.provenance.clubMode;
  for (const auto &cover : chart.laneCoverEvents) {
    if (cover.songTimeMicros <= 0) {
      setup.initialLaneCoverPercent = cover.noteStartPositionPercent;
      setup.laneCoverEnabled = setup.initialLaneCoverPercent > 0;
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
    bool scratch = false;
    const auto control =
        legacyControl(entry.event.lane, setup.keyMode, scratch);
    if (!control.has_value() ||
        entry.event.songTimeMicros < replay::kMinimumReplaySongTimeMicros) {
      continue;
    }
    scratchBestEffort |= scratch;
    candidates.push_back({
        .songTimeMicros = entry.event.songTimeMicros,
        .control = *control,
        .pressed = action == replay::LegacyPlaybackAction::Press,
    });
  }
  std::ranges::stable_sort(candidates, {},
                           &replay::InputTransition::songTimeMicros);
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
  if (chart.playback.input.empty()) {
    chart.playback.input = {
        {.songTimeMicros = 0,
         .control = {.kind = replay::LogicalControlKind::Lane,
                     .player = 1,
                     .lane = 0},
         .pressed = true},
        {.songTimeMicros = 1,
         .control = {.kind = replay::LogicalControlKind::Lane,
                     .player = 1,
                     .lane = 0},
         .pressed = false},
    };
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
    chart.id = sqlite3_column_int64(statement.get(), 0);
    if (chart.id <= 0 || chart.id > std::numeric_limits<int>::max()) {
      diagnostic = "legacy replay public ID is invalid";
      return false;
    }
    chart.chartPath = columnText(statement.get(), 1);
    chart.chartMd5 = columnText(statement.get(), 2);
    chart.chartSha256 = columnText(statement.get(), 3);
    chart.chartTitle = columnText(statement.get(), 4);
    chart.chartArtist = columnText(statement.get(), 5);
    chart.longNoteMode = sqlite3_column_int(statement.get(), 6);
    chart.initialGaugeType =
        gaugeTypeAtIndex(sqlite3_column_int(statement.get(), 7));
    chart.gaugeAutoShift =
        gaugeAutoShiftModeFromValue(sqlite3_column_int(statement.get(), 8));
    chart.finalScore = sqlite3_column_int(statement.get(), 9);
    chart.maxCombo = sqlite3_column_int(statement.get(), 10);
    chart.finalGauge =
        static_cast<float>(sqlite3_column_double(statement.get(), 11));
    chart.clearType = sqlite3_column_int(statement.get(), 12);
    if (sqlite3_column_type(statement.get(), 13) != SQLITE_NULL) {
      chart.randomSeed =
          static_cast<unsigned int>(sqlite3_column_int64(statement.get(), 13));
    }
    if (sqlite3_column_type(statement.get(), 14) == SQLITE_TEXT) {
      chart.randomPrng = columnText(statement.get(), 14);
    }
    if (sqlite3_column_type(statement.get(), 15) == SQLITE_TEXT) {
      chart.randomValues = parseIntegers(columnText(statement.get(), 15));
    }
    if (sqlite3_column_type(statement.get(), 16) == SQLITE_TEXT) {
      chart.playOption = columnText(statement.get(), 16);
    }
    if (sqlite3_column_type(statement.get(), 17) != SQLITE_NULL) {
      chart.playOptionSeed = sqlite3_column_int64(statement.get(), 17);
    }
    if (sqlite3_column_type(statement.get(), 18) == SQLITE_TEXT) {
      chart.playOption2 = columnText(statement.get(), 18);
    }
    if (sqlite3_column_type(statement.get(), 19) != SQLITE_NULL) {
      chart.playOption2Seed = sqlite3_column_int64(statement.get(), 19);
    }
    chart.assistOption = columnText(statement.get(), 20);
    chart.createdAt = columnText(statement.get(), 21);
    chart.provenanceJson = columnText(statement.get(), 24);
    std::string provenanceError;
    auto provenance =
        deserializeScoreProvenance(chart.provenanceJson, provenanceError);
    if (!provenance.has_value() ||
        provenance->ruleset.version !=
            sqlite3_column_int(statement.get(), 22) ||
        static_cast<int>(provenance->eligibility) !=
            sqlite3_column_int(statement.get(), 23)) {
      diagnostic = provenanceError.empty()
                       ? "legacy replay provenance columns disagree"
                       : std::move(provenanceError);
      return false;
    }
    chart.provenance = std::move(*provenance);
    if (sqlite3_column_type(statement.get(), 25) == SQLITE_TEXT) {
      chart.attemptId = columnText(statement.get(), 25);
    }
    chart.playedAtUnixMillis = sqlite3_column_int64(statement.get(), 26);
    charts.push_back(std::move(chart));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  for (auto &chart : charts) {
    if (!readEvents(database, chart, diagnostic) ||
        !readPendingResult(database, chart, diagnostic) ||
        !buildPlayback(chart, diagnostic)) {
      return false;
    }
  }
  return true;
}

bool readCourseStages(sqlite3 *database, LegacyCourse &course,
                      const std::map<std::int64_t, std::size_t> &chartById,
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
    const int stageIndex = sqlite3_column_int(statement.get(), 0);
    const std::int64_t replayId = sqlite3_column_int64(statement.get(), 1);
    const std::int64_t restMicros = sqlite3_column_int64(statement.get(), 2);
    const auto chart = chartById.find(replayId);
    if (stageIndex != expectedIndex || chart == chartById.end() ||
        restMicros < 0) {
      diagnostic = "legacy course stage order or replay link is invalid";
      return false;
    }
    course.chartIndexes.push_back(chart->second);
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

  std::vector<course_identity::ChartIdentity> identities;
  identities.reserve(course.chartIndexes.size());
  replay::CoursePathInput pathInput;
  pathInput.longNoteMode = std::clamp(course.longNoteMode, 0, 2);
  pathInput.hasUndefinedLongNotes = true;
  pathInput.beatorajaConstraintIds =
      beatorajaCourseConstraintIds(course.constraintJson);
  for (std::size_t stageIndex = 0; stageIndex < course.chartIndexes.size();
       ++stageIndex) {
    const LegacyChart &chart = charts[course.chartIndexes[stageIndex]];
    identities.push_back({.sha256 = chart.chartSha256, .md5 = chart.chartMd5});
    pathInput.stageSha256.push_back(chart.chartSha256);
    persisted.stages.push_back(
        {.stageIndex = static_cast<int>(stageIndex),
         .score = chart.result.score,
         .keyMode = chart.result.keyMode,
         .adoptedGaugeHistory = chart.result.adoptedGaugeHistory,
         .judgementTiming = chart.result.judgementTiming});
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

bool readCourses(sqlite3 *database, const std::vector<LegacyChart> &charts,
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
    course.id = sqlite3_column_int64(statement.get(), 0);
    if (course.id <= 0 || course.id > std::numeric_limits<int>::max()) {
      diagnostic = "legacy course public ID is invalid";
      return false;
    }
    course.legacyCourseId = sqlite3_column_int(statement.get(), 1);
    course.courseKey = columnText(statement.get(), 2);
    course.courseName = columnText(statement.get(), 3);
    course.courseGroupName = columnText(statement.get(), 4);
    course.constraintJson = columnText(statement.get(), 5);
    course.initialGaugeType =
        gaugeTypeAtIndex(sqlite3_column_int(statement.get(), 6));
    course.gaugeProfile =
        gaugeProfileFromInt(sqlite3_column_int(statement.get(), 7));
    course.gaugeAutoShift =
        gaugeAutoShiftModeFromValue(sqlite3_column_int(statement.get(), 8));
    course.longNoteMode = sqlite3_column_int(statement.get(), 9);
    course.requestedPlayOption = columnText(statement.get(), 10);
    course.assistOption = columnText(statement.get(), 11);
    course.finalScore = sqlite3_column_int(statement.get(), 12);
    course.maxCombo = sqlite3_column_int(statement.get(), 13);
    course.finalGauge =
        static_cast<float>(sqlite3_column_double(statement.get(), 14));
    course.clearType = sqlite3_column_int(statement.get(), 15);
    course.completedCharts = sqlite3_column_int(statement.get(), 16);
    course.totalCharts = sqlite3_column_int(statement.get(), 17);
    course.createdAt = columnText(statement.get(), 18);
    course.provenanceJson = columnText(statement.get(), 21);
    std::string provenanceError;
    auto provenance =
        deserializeScoreProvenance(course.provenanceJson, provenanceError);
    if (!provenance.has_value() ||
        provenance->ruleset.version !=
            sqlite3_column_int(statement.get(), 19) ||
        static_cast<int>(provenance->eligibility) !=
            sqlite3_column_int(statement.get(), 20)) {
      diagnostic = provenanceError.empty()
                       ? "legacy course provenance columns disagree"
                       : std::move(provenanceError);
      return false;
    }
    course.provenance = std::move(*provenance);
    course.playedAtUnixMillis = sqlite3_column_int64(statement.get(), 22);
    courses.push_back(std::move(course));
  }
  if (result != SQLITE_DONE) {
    diagnostic = statement.error();
    return false;
  }
  for (auto &course : courses) {
    if (!readCourseStages(database, course, chartById, diagnostic) ||
        !buildCourse(course, charts, diagnostic)) {
      return false;
    }
  }
  return true;
}

void assignPaths(std::vector<LegacyChart> &charts,
                 std::vector<LegacyCourse> &courses, std::string &diagnostic) {
  std::vector<LegacyChart *> order;
  order.reserve(charts.size());
  for (auto &chart : charts) {
    order.push_back(&chart);
  }
  std::ranges::sort(
      order, [](const LegacyChart *left, const LegacyChart *right) {
        return std::tie(left->path.stem, left->playedAtUnixMillis, left->id) <
               std::tie(right->path.stem, right->playedAtUnixMillis, right->id);
      });
  std::string previous;
  std::int64_t index = -1;
  for (LegacyChart *chart : order) {
    index = chart->path.stem == previous ? index + 1 : 0;
    previous = chart->path.stem;
    const auto path = replay::pathForStem(chart->path.stem, index, diagnostic);
    if (!path.has_value()) {
      return;
    }
    chart->path = *path;
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
  index = -1;
  for (LegacyCourse *course : courseOrder) {
    index = course->path.stem == previous ? index + 1 : 0;
    previous = course->path.stem;
    const auto path = replay::pathForStem(course->path.stem, index, diagnostic);
    if (!path.has_value()) {
      return;
    }
    course->path = *path;
  }
}

bool finalizeFiles(std::vector<LegacyChart> &charts,
                   std::vector<LegacyCourse> &courses,
                   const replay::BeatorajaReplayCodec &codec,
                   replay::ReplayFileStore &store,
                   const ReplayMigrationFaults &faults,
                   std::string &diagnostic) {
  for (auto &chart : charts) {
    if (fault(faults, "encode", chart.id)) {
      diagnostic = "injected replay encode failure";
      return false;
    }
    const auto bytes =
        codec.encodeChart(chart.playback, chart.playedAtUnixMillis, diagnostic);
    if (!bytes.has_value()) {
      return false;
    }
    const auto finalized =
        store.finalize(chart.path, *bytes, codec,
                       {.stageSha256 = {chart.chartSha256}, .course = false},
                       "migration-chart-" + std::to_string(chart.id));
    if (!finalized.metadata.has_value()) {
      diagnostic = finalized.diagnostic.empty()
                       ? "could not finalize migrated replay file"
                       : finalized.diagnostic;
      return false;
    }
    chart.file = *finalized.metadata;
  }
  for (auto &course : courses) {
    if (fault(faults, "encode", course.id)) {
      diagnostic = "injected course replay encode failure";
      return false;
    }
    const auto bytes = codec.encodeCourse(
        course.playback, course.playedAtUnixMillis, diagnostic);
    if (!bytes.has_value()) {
      return false;
    }
    std::vector<std::string> stageSha256;
    stageSha256.reserve(course.playback.stages.size());
    for (const auto &stage : course.playback.stages) {
      stageSha256.push_back(stage.setup.chartSha256);
    }
    const auto finalized =
        store.finalize(course.path, *bytes, codec,
                       {.stageSha256 = std::move(stageSha256), .course = true},
                       "migration-course-" + std::to_string(course.id));
    if (!finalized.metadata.has_value()) {
      diagnostic = finalized.diagnostic.empty()
                       ? "could not finalize migrated course replay file"
                       : finalized.diagnostic;
      return false;
    }
    course.file = *finalized.metadata;
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

bool insertChart(sqlite3 *database, const LegacyChart &chart,
                 std::string &diagnostic) {
  Statement result(
      database,
      "INSERT INTO chart_results(id,attempt_id,chart_path,chart_md5,"
      "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
      "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
      "fast,slow,final_gauge,clear_type,gauge_history_json,"
      "judgement_timing_json,provenance_json,result_fingerprint,"
      "played_at_unix_ms,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
      "?,?,?,?,?,?,?,?,NULL,?,?,?,?)");
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
      bindText(result.get(), column++, history) &&
      bindText(result.get(), column++, chart.provenanceJson) &&
      bindText(result.get(), column++, persisted.resultFingerprint) &&
      sqlite3_bind_int64(result.get(), column++,
                         persisted.playedAtUnixMillis) == SQLITE_OK &&
      bindText(result.get(), column++, chart.createdAt);
  if (!okay || column != 29 || sqlite3_step(result.get()) != SQLITE_DONE) {
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
      "provenance_json,result_fingerprint,played_at_unix_ms,created_at) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  if (!result.valid()) {
    diagnostic = result.error();
    return false;
  }
  const auto &persisted = course.result;
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
      bindText(result.get(), column++, persisted.resultFingerprint) &&
      sqlite3_bind_int64(result.get(), column++,
                         persisted.playedAtUnixMillis) == SQLITE_OK &&
      bindText(result.get(), column++, course.createdAt);
  if (!okay || column != 26 || sqlite3_step(result.get()) != SQLITE_DONE) {
    diagnostic = result.error();
    return false;
  }

  Statement stage(
      database,
      "INSERT INTO course_result_stages(course_result_id,stage_index,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,key_mode,"
      "long_note_mode,score,max_score,max_combo,combo_break,p_great,great,"
      "good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
      "gauge_history_json,judgement_timing_json,provenance_json) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
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
        bindText(stage.get(), column++, history) &&
        sqlite3_bind_null(stage.get(), column++) == SQLITE_OK &&
        bindText(stage.get(), column++, provenance);
    if (!okay || column != 27 || sqlite3_step(stage.get()) != SQLITE_DONE) {
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
      "recovery_attempts,last_recovery_at) SELECT attempt_id,replay_id,"
      "chart_path,chart_md5,chart_sha256,chart_title,chart_artist,ln_mode,"
      "score,max_score,max_combo,combo_break,pgreat,great,good,bad,poor,"
      "kpoor,fast,slow,final_gauge,clear_type,ruleset_version,eligibility,"
      "provenance_json,created_at,recovery_attempts,last_recovery_at FROM "
      "legacy_v10_pending_chart_score_writes;"
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
    replay::ReplayFileStore &fileStore, ReplayMigrationFaults faults) {
  if (database == nullptr || profileRoot.empty()) {
    return failure(MigrationStatus::StorageFailure,
                   "replay migration database or profile root is missing");
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
  if (userVersion == 11) {
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
  if (!readCharts(database, charts, diagnostic) ||
      !readCourses(database, charts, courses, diagnostic)) {
    return failure(MigrationStatus::InvalidLegacyData,
                   diagnostic.empty() ? "legacy replay rows are invalid"
                                      : std::move(diagnostic));
  }
  assignPaths(charts, courses, diagnostic);
  if (!diagnostic.empty()) {
    return failure(MigrationStatus::InvalidLegacyData, std::move(diagnostic));
  }
  if (!finalizeFiles(charts, courses, codec, fileStore, faults, diagnostic)) {
    return failure(MigrationStatus::FileFailure,
                   diagnostic.empty() ? "could not finalize replay files"
                                      : std::move(diagnostic),
                   charts.size(), courses.size());
  }

  if (fault(faults, "pre-cutover-revalidation")) {
    return failure(MigrationStatus::StorageFailure,
                   "injected pre-cutover validation failure", charts.size(),
                   courses.size());
  }
  for (const auto &chart : charts) {
    const auto inspected = fileStore.inspect(chart.file);
    if (inspected.state != replay::ReplayFileState::Available ||
        !inspected.metadata.has_value() || *inspected.metadata != chart.file) {
      return failure(MigrationStatus::FileFailure,
                     inspected.diagnostic.empty()
                         ? "migrated replay changed before cutover"
                         : inspected.diagnostic,
                     charts.size());
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
                     charts.size(), courses.size());
    }
  }

  std::string transactionError;
  if (fault(faults, "begin")) {
    return failure(MigrationStatus::StorageFailure,
                   "injected migration transaction failure", charts.size(),
                   courses.size());
  }
  SqliteTransactionHandle transaction(database, "BEGIN IMMEDIATE TRANSACTION",
                                      transactionError);
  if (!transaction.active()) {
    return failure(MigrationStatus::StorageFailure,
                   "could not start replay migration cutover: " +
                       transactionError,
                   charts.size(), courses.size());
  }
  if (!renameLegacyTables(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure, std::move(diagnostic),
                   charts.size(), courses.size());
  }
  if (fault(faults, "schema-create") ||
      !CreateCompactReplaySchema11OnConnection(database)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not create compact replay schema", charts.size(),
                   courses.size());
  }
  for (const auto &chart : charts) {
    if (fault(faults, "copy-chart", chart.id) ||
        !insertChart(database, chart, diagnostic)) {
      return failure(MigrationStatus::StorageFailure,
                     diagnostic.empty() ? "could not copy chart result"
                                        : std::move(diagnostic),
                     charts.size(), courses.size());
    }
  }
  for (const auto &course : courses) {
    if (fault(faults, "copy-course", course.id) ||
        !insertCourse(database, course, diagnostic)) {
      return failure(MigrationStatus::StorageFailure,
                     diagnostic.empty() ? "could not copy course result"
                                        : std::move(diagnostic),
                     charts.size(), courses.size());
    }
  }
  if (fault(faults, "copy-durable-work") ||
      !copyDurableWork(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "could not copy durable work"
                                      : std::move(diagnostic),
                   charts.size(), courses.size());
  }
  std::size_t courseStageCount = 0;
  for (const auto &course : courses) {
    courseStageCount += course.result.stages.size();
  }
  if (fault(faults, "count-verification") ||
      !verifyCounts(database, charts.size(), courses.size(), courseStageCount,
                    diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "migrated counts are invalid"
                                      : std::move(diagnostic),
                   charts.size(), courses.size());
  }
  if (fault(faults, "foreign-key-verification") ||
      !foreignKeysClean(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "migrated foreign keys are invalid"
                                      : std::move(diagnostic),
                   charts.size(), courses.size());
  }
  if (fault(faults, "legacy-drop") || !dropLegacyTables(database, diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   diagnostic.empty() ? "could not drop legacy replay rows"
                                      : std::move(diagnostic),
                   charts.size(), courses.size());
  }
  if (fault(faults, "version-update") ||
      !execute(database, "PRAGMA user_version=11", diagnostic)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not advance replay schema version", charts.size(),
                   courses.size());
  }
  if (fault(faults, "commit") || !transaction.commit(transactionError)) {
    return failure(MigrationStatus::StorageFailure,
                   "could not commit replay schema cutover", charts.size(),
                   courses.size());
  }
  return {.status = MigrationStatus::Migrated,
          .chartFiles = charts.size(),
          .courseFiles = courses.size()};
}

} // namespace replay_repository_detail
