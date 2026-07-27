#include "ResultPersistenceCoordinator.h"

#include "CourseIdentity.h"
#include "FileChecksum.h"
#include "ir/IrSubmissionSnapshot.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/GzipCodec.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplayFileActionService.h"
#include "sqlite3.h"

#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace result_persistence;
using Json = nlohmann::ordered_json;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic<unsigned long long> sequence{0};
    const auto tick =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() /
        ("asobmashow-result-v11-" + std::string(label) + "-" +
         std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::int64_t scalar(const std::filesystem::path &databasePath,
                    std::string_view query) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return -1;
  }
  sqlite3_stmt *statement = nullptr;
  if (sqlite3_prepare_v2(database, std::string(query).c_str(), -1, &statement,
                         nullptr) != SQLITE_OK ||
      sqlite3_step(statement) != SQLITE_ROW) {
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return -1;
  }
  const std::int64_t value = sqlite3_column_int64(statement, 0);
  const bool complete = sqlite3_step(statement) == SQLITE_DONE;
  sqlite3_finalize(statement);
  sqlite3_close(database);
  return complete ? value : -1;
}

bool executeSql(const std::filesystem::path &databasePath,
                std::string_view query) {
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return false;
  }
  char *message = nullptr;
  const bool success = sqlite3_exec(database, std::string(query).c_str(),
                                    nullptr, nullptr, &message) == SQLITE_OK;
  sqlite3_free(message);
  sqlite3_close(database);
  return success;
}

std::vector<char> readBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool writeBytes(const std::filesystem::path &path,
                std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::optional<std::vector<std::byte>>
withUnsupportedAsoExtension(const std::filesystem::path &path,
                            std::span<const std::size_t> stageIndexes) {
  const auto source = readBytes(path);
  const auto compressed = std::span(
      reinterpret_cast<const std::byte *>(source.data()), source.size());
  std::string diagnostic;
  const auto jsonBytes = replay::gzipDecompressBounded(
      compressed, replay::ReplayCodecLimits{}.maxJsonBytes, diagnostic);
  if (!jsonBytes.has_value()) {
    return std::nullopt;
  }

  Json document;
  try {
    document = Json::parse(std::string(
        reinterpret_cast<const char *>(jsonBytes->data()), jsonBytes->size()));
    for (const std::size_t index : stageIndexes) {
      Json &stage = document.is_array() ? document.at(index) : document;
      stage.at("asobmashow").at("schemaVersion") = 999;
    }
  } catch (const Json::exception &) {
    return std::nullopt;
  }

  const std::string serialized = document.dump();
  return replay::gzipCompress(
      std::as_bytes(std::span(serialized.data(), serialized.size())),
      diagnostic);
}

std::map<std::string, std::int64_t>
tableRowCounts(const std::filesystem::path &databasePath) {
  std::map<std::string, std::int64_t> result;
  sqlite3 *database = nullptr;
  if (sqlite3_open_v2(databasePath.string().c_str(), &database,
                      SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return result;
  }
  sqlite3_stmt *tables = nullptr;
  if (sqlite3_prepare_v2(
          database,
          "SELECT name FROM sqlite_master WHERE type='table' AND "
          "name NOT LIKE 'sqlite_%' ORDER BY name",
          -1, &tables, nullptr) != SQLITE_OK) {
    sqlite3_close(database);
    return result;
  }
  while (sqlite3_step(tables) == SQLITE_ROW) {
    const auto *nameText = sqlite3_column_text(tables, 0);
    if (nameText == nullptr) {
      continue;
    }
    const std::string name(reinterpret_cast<const char *>(nameText));
    sqlite3_stmt *count = nullptr;
    const std::string query = "SELECT count(*) FROM \"" + name + "\"";
    if (sqlite3_prepare_v2(database, query.c_str(), -1, &count, nullptr) ==
            SQLITE_OK &&
        sqlite3_step(count) == SQLITE_ROW) {
      result.emplace(name, sqlite3_column_int64(count, 0));
    }
    sqlite3_finalize(count);
  }
  sqlite3_finalize(tables);
  sqlite3_close(database);
  return result;
}

CompletedChartAttempt validAttempt(std::string attemptId, int salt = 0) {
  CompletedChartAttempt attempt;
  auto &result = attempt.result;
  result.attemptId = std::move(attemptId);
  result.score.chartPath = "sample/song-" + std::to_string(salt) + ".bms";
  result.score.chartMd5 = repeated(salt == 0 ? 'b' : 'd', 32);
  result.score.chartSha256 = repeated(salt == 0 ? 'a' : 'e', 64);
  result.score.chartTitle = "Title";
  result.score.chartArtist = "Artist";
  result.score.longNoteMode = 1;
  result.score.score = 7;
  result.score.maxScore = 10;
  result.score.maxCombo = 4;
  result.score.comboBreak = 1;
  result.score.pGreat = 3;
  result.score.great = 1;
  result.score.good = 1;
  result.score.fast = 2;
  result.score.slow = 1;
  result.score.finalGauge = 82.5F;
  result.score.clearType = kClearTypeNormalClearRank;
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta.MD5 = result.score.chartMd5;
  provenance.chartMeta.SHA256 = result.score.chartSha256;
  provenance.chartMeta.Rank = 2;
  provenance.chartMeta.TotalNotes = 5;
  provenance.chartMeta.HasTotal = true;
  provenance.chartMeta.Total = 100.0;
  provenance.longNoteMode = result.score.longNoteMode;
  provenance.judgeRankSource = JudgeRankSource::Chart;
  provenance.sourceJudgeRank = provenance.chartMeta.Rank;
  provenance.effectiveJudgeWindows = {
      {Bad, {-330'000, 420'000}},   {PGreat, {-10'000, 10'000}},
      {Great, {-30'000, 30'000}},   {Good, {-75'000, 75'000}},
      {Kpoor, {-500'000, 150'000}},
  };
  provenance.totalNotes = provenance.chartMeta.TotalNotes;
  provenance.authoredGaugeTotal = provenance.chartMeta.Total;
  provenance.effectiveGaugeTotal = provenance.chartMeta.Total;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  provenance.ruleset = RulesetDescriptor::Current();
  result.score.provenance = makeScoreProvenance(provenance);
  result.keyMode = 7;
  result.adoptedGaugeType =
      salt == 0 ? GaugeType::Easy : GaugeType::Hard;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.judgementTiming = ChartJudgementTiming{};
  result.judgementTiming->byJudgement[PGreat] = {.fast = 1, .slow = 1};
  result.judgementTiming->byJudgement[Great] = {.fast = 1, .slow = 0};
  result.playedAtUnixMillis = 1'700'000'000'123LL + salt;
  result.resultFingerprint = resultFingerprint(result);

  auto &playback = attempt.replay;
  playback.setup.chartMd5 = result.score.chartMd5;
  playback.setup.chartSha256 = result.score.chartSha256;
  playback.setup.keyMode = result.keyMode;
  playback.setup.longNoteMode = result.score.longNoteMode;
  playback.setup.hasUndefinedLongNotes = false;
  playback.setup.playOption = "NORMAL";
  playback.setup.playOption2 = "NORMAL";
  const auto ruleset = RulesetDescriptor::Current();
  playback.setup.playbackRulesetId = ruleset.id;
  playback.setup.playbackRulesetRevision = ruleset.version;
  GaugeStateSnapshot startingGauge{
      .gaugeType = GaugeType::Normal,
      .selectedGaugeType = GaugeType::Normal,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 20.0F,
  };
  startingGauge.gaugeValues = {20.0F, 20.0F, 20.0F,
                               100.0F, 100.0F, 100.0F};
  playback.setup.startingGaugeState = startingGauge;
  playback.setup.initialLaneCoverPercent = 37;
  playback.setup.laneCoverEnabled = true;
  playback.input = {
      {.songTimeMicros = 1000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1500,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
      {.songTimeMicros = 2000,
       .control = {.kind = replay::LogicalControlKind::Start,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
  };
  playback.touchSamples = {{.action = replay::ReplayTouchAction::Down,
                            .fingerId = 7,
                            .songTimeMicros = 2100,
                            .x = 0.25F,
                            .y = 0.75F}};
  playback.laneCoverEvents = {{.songTimeMicros = 2200,
                               .noteStartPositionPercent = 41,
                               .resetVisibleTimeReference = true}};

  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  expect(snapshot.has_value(), "integration fixture snapshot captures");
  if (snapshot) {
    attempt.irSnapshot = *snapshot;
  }
  return attempt;
}

CompletedCourseAttempt validCourseAttempt(std::string attemptId) {
  const auto first = validAttempt("123e4567-e89b-42d3-a456-426614174010", 0);
  const auto second = validAttempt("123e4567-e89b-42d3-a456-426614174011", 1);
  CompletedCourseAttempt attempt;
  attempt.replay.stages = {first.replay, second.replay};
  attempt.replay.restMicrosAfterStage = {750'000, 0};

  auto &result = attempt.result;
  result.attemptId = std::move(attemptId);
  const std::vector<course_identity::ChartIdentity> identities{
      {.sha256 = first.result.score.chartSha256,
       .md5 = first.result.score.chartMd5},
      {.sha256 = second.result.score.chartSha256,
       .md5 = second.result.score.chartMd5},
  };
  result.constraintJson = R"(["no_speed","gauge_7k","cn"])";
  result.courseKey =
      course_identity::makeCourseKey(identities, result.constraintJson);
  result.legacyCourseId = 7;
  result.courseName = "Integration Course";
  result.courseGroupName = "Tests";
  result.completedCharts = 2;
  result.totalCharts = 2;
  result.requestedPlayOption = "RANDOM";
  result.assistOption = assist_options::kOff;
  result.initialGaugeType = GaugeType::Normal;
  result.gaugeProfile = GaugeProfile::Course7Keys;
  result.gaugeAutoShift = GaugeAutoShiftMode::None;
  result.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  result.longNoteMode = 2;
  result.maxCombo = 8;
  result.finalGauge = second.result.score.finalGauge;
  result.clearType = kClearTypeNormalClearRank;
  result.provenance = first.result.score.provenance;
  result.playedAtUnixMillis = 1'700'000'999'000LL;
  result.stages = {
      {.stageIndex = 0,
       .score = first.result.score,
       .keyMode = first.result.keyMode,
       .adoptedGaugeType = first.result.adoptedGaugeType,
       .adoptedGaugeHistory = first.result.adoptedGaugeHistory,
       .judgementTiming = first.result.judgementTiming},
      {.stageIndex = 1,
       .score = second.result.score,
       .keyMode = second.result.keyMode,
       .adoptedGaugeType = second.result.adoptedGaugeType,
       .adoptedGaugeHistory = second.result.adoptedGaugeHistory,
       .judgementTiming = second.result.judgementTiming},
  };
  result.entryFacts = {
      {.totalNotes = first.result.score.maxScore / 2,
       .playLengthMicros = 1'000'000},
      {.totalNotes = second.result.score.maxScore / 2,
       .playLengthMicros = 2'000'000},
  };
  result.finalScore = first.result.score.score + second.result.score.score;
  result.maxScore = first.result.score.maxScore + second.result.score.maxScore;
  result.resultFingerprint = resultFingerprint(result);
  return attempt;
}

ir::IrOutboxDraft draftFor(const CompletedChartAttempt &attempt) {
  return {
      .providerId = "tachi",
      .attemptId = *attempt.result.attemptId,
      .chartMd5 = attempt.result.score.chartMd5,
      .chartSha256 = attempt.result.score.chartSha256,
      .payloadJson = R"({"score":7})",
      .rulesetProof =
          {
              .rulesetId = "test-rules",
              .rulesetRevision = 1,
              .validationFingerprint = repeated('f', 64),
          },
      .createdAtUnixMillis = attempt.result.playedAtUnixMillis,
  };
}

struct Environment {
  explicit Environment(const std::filesystem::path &root,
                       replay::ReplayFileStoreFaults faults = {})
      : replayDatabase(root / "replay.db"), scoreDatabase(root / "score.db"),
        replayRepository(replayDatabase), scoreRepository(scoreDatabase),
        fileStore(root, std::move(faults)),
        coordinator(scoreRepository, replayRepository, fileStore, codec) {
    expect(replayRepository.EnsureSchema(), "replay schema initializes");
    expect(scoreRepository.EnsureSchema(), "score schema initializes");
  }

  std::filesystem::path replayDatabase;
  std::filesystem::path scoreDatabase;
  ReplayRepository replayRepository;
  ScoreRepository scoreRepository;
  replay::BeatorajaReplayCodec codec;
  replay::ReplayFileStore fileStore;
  Coordinator coordinator;
};

bool replaceReplayStagesWithStockFallback(
    Environment &environment, const ReplayFileReference &reference,
    std::string_view ownerColumn, int resultId,
    std::span<const std::size_t> stageIndexes) {
  const auto replayPath =
      environment.replayDatabase.parent_path() / reference.relativePath;
  const auto futureBytes =
      withUnsupportedAsoExtension(replayPath, stageIndexes);
  if (!futureBytes.has_value() || !writeBytes(replayPath, *futureBytes)) {
    return false;
  }
  std::string diagnostic;
  const auto futureHash = file_checksum::sha256File(
      replayPath, diagnostic, futureBytes->size());
  return futureHash.has_value() &&
         executeSql(environment.replayDatabase,
                    "UPDATE replay_files SET content_sha256='" +
                        *futureHash + "',compressed_size=" +
                        std::to_string(futureBytes->size()) + " WHERE " +
                        std::string(ownerColumn) + "=" +
                        std::to_string(resultId));
}

ReplayFileReference finalizedReference(Environment &environment,
                                       const CompletedChartAttempt &attempt) {
  const std::string &attemptId = *attempt.result.attemptId;
  const auto reserved = environment.replayRepository.reserveReplayFile(
      attemptId, attempt.replay.setup.chartSha256);
  expect(reserved.reservation.has_value(), "manual recovery file reserves");
  if (!reserved.reservation) {
    return {};
  }
  std::string diagnostic;
  const auto encoded = environment.codec.encodeChart(
      attempt.replay, attempt.result.playedAtUnixMillis, diagnostic);
  expect(encoded.has_value(), "manual recovery replay encodes");
  if (!encoded) {
    return {};
  }
  const replay::ReplayPathIdentity identity{
      .stem = reserved.reservation->stem,
      .historyIndex = reserved.reservation->historyIndex,
      .relativePath = reserved.reservation->relativePath,
  };
  const auto finalized = environment.fileStore.finalize(
      identity, *encoded, environment.codec,
      {.stageSha256 = {attempt.replay.setup.chartSha256},
       .stageLongNoteModes = {attempt.replay.setup.longNoteMode},
       .course = false},
      attemptId);
  expect(finalized.metadata.has_value(), "manual recovery replay finalizes");
  if (!finalized.metadata) {
    return {};
  }
  std::string ownershipDiagnostic;
  expect(environment.replayRepository.markReplayFileReservationFinalized(
             *reserved.reservation, *finalized.metadata,
             ownershipDiagnostic),
         "manual recovery replay records finalized ownership");
  return {
      .stem = identity.stem,
      .historyIndex = identity.historyIndex,
      .relativePath = finalized.metadata->relativePath,
      .contentSha256 = finalized.metadata->sha256,
      .compressedSize = finalized.metadata->compressedSize,
      .codecVersion = finalized.metadata->codecVersion,
  };
}

void testCompleteFileAndDatabasePipeline() {
  TemporaryDirectory temporary("complete");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174000");
  const auto draft = draftFor(attempt);
  const auto saved = environment.coordinator.persist(attempt, {&draft, 1});
  if (!saved.saved()) {
    std::cerr << "complete pipeline state=" << static_cast<int>(saved.state)
              << " diagnostic=" << saved.diagnostic << '\n';
  }
  expect(saved.saved() && saved.receipt && saved.receipt->resultId > 0,
         "real file and SQLite pipeline saves");
  if (!saved.receipt) {
    return;
  }

  const auto loaded =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  expect(loaded.status == ResultReadOutcome::Status::Loaded && loaded.record &&
             loaded.record->replayFile,
         "compact result loads with one replay-file reference");
  if (!loaded.record || !loaded.record->replayFile) {
    return;
  }
  const auto &reference = *loaded.record->replayFile;
  expect(reference.relativePath ==
                 std::filesystem::path("replay") /
                     (attempt.result.score.chartSha256 + ".brd") &&
             std::filesystem::is_regular_file(temporary.path() /
                                              reference.relativePath),
         "first play uses Beatoraja's replay/<sha256>.brd layout");
  const replay::ReplayFileMetadata metadata{
      .relativePath = reference.relativePath,
      .sha256 = reference.contentSha256,
      .compressedSize = reference.compressedSize,
      .codecVersion = reference.codecVersion,
  };
  const auto decoded = environment.fileStore.load(metadata, environment.codec);
  expect(decoded.chart &&
             decoded.chart->setup.chartSha256 ==
                 attempt.replay.setup.chartSha256 &&
             decoded.chart->input == attempt.replay.input &&
             decoded.chart->touchSamples == attempt.replay.touchSamples &&
             decoded.chart->laneCoverEvents == attempt.replay.laneCoverEvents,
         "persisted .brd round-trips stock and Aso extension events");
  const auto snapshot = environment.replayRepository.loadIrSubmissionSnapshot(
      *attempt.result.attemptId);
  expect(snapshot.snapshot == attempt.irSnapshot,
         "provider-neutral IR snapshot is independent of replay bytes");
  const auto outbox = environment.replayRepository.LoadIrOutbox(
      draft.providerId, draft.attemptId);
  expect(outbox.status == ir::IrOutboxReadStatus::Found && outbox.entry &&
             outbox.entry->localResultReady,
         "automatic IR draft activates only after local score projection");

  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM chart_results") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM pending_chart_score_writes") == 0 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "successful save leaves one compact result, one file reference, and "
         "one projected score");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM sqlite_master WHERE type='table' AND "
                "name IN ('replay_events','replay_touch_samples',"
                "'replay_lane_cover_events')") == 0,
         "fresh saves have no per-event SQLite tables");

  const auto retried = environment.coordinator.persist(attempt, {&draft, 1});
  expect(retried.saved() && retried.receipt &&
             retried.receipt->resultId == saved.receipt->resultId &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1,
         "retry is idempotent across reservation, file, result, and score");

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const LocalResultRecordId recordId{.resultId = saved.receipt->resultId};
  const auto available = actions.inspect(recordId);
  expect(available.availability == ReplayAvailability::Available &&
             available.sourcePath ==
                 temporary.path() / reference.relativePath &&
             available.suggestedFilename ==
                 reference.relativePath.filename().string(),
         "file actions expose the verified existing Beatoraja file");

  std::vector<char> originalBytes;
  {
    std::ifstream input(temporary.path() / reference.relativePath,
                        std::ios::binary);
    originalBytes.assign(std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>());
  }
  {
    std::ofstream corrupt(temporary.path() / reference.relativePath,
                          std::ios::binary | std::ios::trunc);
    corrupt.put('x');
  }
  expect(actions.inspect(recordId).availability == ReplayAvailability::Corrupt,
         "changed replay bytes are reported as corrupt");
  {
    std::ofstream restore(temporary.path() / reference.relativePath,
                          std::ios::binary | std::ios::trunc);
    restore.write(originalBytes.data(),
                  static_cast<std::streamsize>(originalBytes.size()));
  }
  expect(actions.inspect(recordId).availability ==
             ReplayAvailability::Available,
         "restoring identical bytes automatically restores availability");
  {
    std::ofstream corrupt(temporary.path() / reference.relativePath,
                          std::ios::binary | std::ios::trunc);
    corrupt.put('x');
  }
  const auto removed = actions.remove(recordId);
  expect(removed.availability == ReplayAvailability::Missing && removed.changed,
         "user can remove a corrupt but safely contained standalone replay "
         "file");
  const auto afterDelete =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  const auto snapshotAfterDelete =
      environment.replayRepository.loadIrSubmissionSnapshot(
          *attempt.result.attemptId);
  expect(afterDelete.status == ResultReadOutcome::Status::Loaded &&
             snapshotAfterDelete.snapshot == attempt.irSnapshot &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM ir_outbox") == 1 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "deleting .brd does not delete result, score, provenance, or IR "
         "snapshot");
}

void testOwnershipMarkerFailureCannotExposeFinalReplay() {
  TemporaryDirectory temporary("preinstall-ownership");
  Environment environment(temporary.path());
  expect(executeSql(environment.replayDatabase,
                    "CREATE TRIGGER fail_replay_ownership BEFORE UPDATE OF "
                    "finalized_content_sha256,finalized_compressed_size ON "
                    "replay_file_reservations BEGIN SELECT "
                    "RAISE(ABORT,'injected ownership marker failure'); END"),
         "pre-install ownership fixture rejects marker storage");
  const auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174150");
  const auto failed = environment.coordinator.persist(attempt);
  const auto finalPath =
      temporary.path() / "replay" / (attempt.result.score.chartSha256 + ".brd");
  expect(failed.state == SaveState::UnfinalizedReplay &&
             !std::filesystem::exists(finalPath) &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_file_reservations") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM chart_results") == 0,
         "ownership marker failure stops before the final BRD becomes visible");
}

void testFileActionsRejectReplayFromDifferentSavedProvenance() {
  TemporaryDirectory temporary("action-semantic-validation");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174030");
  const auto saved = environment.coordinator.persist(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "semantic action fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }

  auto incompatible = attempt.replay;
  incompatible.setup.initialGaugeType = GaugeType::Hard;
  incompatible.setup.startingGaugeState.reset();
  std::string diagnostic;
  const auto encoded = environment.codec.encodeChart(
      incompatible, attempt.result.playedAtUnixMillis, diagnostic);
  expect(encoded.has_value(),
         "different-context replay remains a valid BRD document");
  if (!encoded.has_value()) {
    return;
  }

  const auto loaded =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
         "semantic action fixture reference loads");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }
  const auto replayPath =
      temporary.path() / loaded.record->replayFile->relativePath;
  expect(writeBytes(replayPath, *encoded),
         "different-context BRD replaces the saved replay bytes");
  const auto sha256 =
      file_checksum::sha256File(replayPath, diagnostic, encoded->size());
  expect(sha256.has_value() &&
             executeSql(
                 environment.replayDatabase,
                 "UPDATE replay_files SET content_sha256='" + *sha256 +
                     "',compressed_size=" + std::to_string(encoded->size()) +
                     " WHERE chart_result_id=" +
                     std::to_string(saved.receipt->resultId)),
         "database digest is updated to trust the replacement bytes");
  if (!sha256.has_value()) {
    return;
  }

  const replay::ReplayFileMetadata replacementMetadata{
      .relativePath = loaded.record->replayFile->relativePath,
      .sha256 = *sha256,
      .compressedSize = encoded->size(),
      .codecVersion = loaded.record->replayFile->codecVersion,
  };
  expect(environment.fileStore.inspect(replacementMetadata).state ==
             replay::ReplayFileState::Available,
         "digest-only inspection accepts the internally valid replacement");

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::ChartResult,
      .resultId = saved.receipt->resultId,
  });
  expect(inspected.availability == ReplayAvailability::Corrupt &&
             !inspected.sourcePath.has_value(),
         "file actions reject a BRD whose setup disagrees with saved "
         "provenance");
}

void testReplayShareUsesVerifiedSnapshotAfterSourceReplacement() {
  TemporaryDirectory temporary("share-snapshot");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174090");
  const auto saved = environment.coordinator.persist(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "share snapshot fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }

  const auto loaded =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
         "share snapshot fixture exposes replay metadata");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }
  const auto &reference = *loaded.record->replayFile;
  const auto replayPath = temporary.path() / reference.relativePath;
  const auto original = readBytes(replayPath);

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  auto prepared = actions.prepareShare({.resultId = saved.receipt->resultId});
  expect(prepared.availability == ReplayAvailability::Available &&
             prepared.sourcePath.has_value() &&
             prepared.sourceLifetime != nullptr,
         "share preparation retains a verified private source");
  if (!prepared.sourcePath.has_value()) {
    return;
  }

  std::vector<std::byte> replacement(original.size(), std::byte{'x'});
  expect(writeBytes(replayPath, replacement),
         "share snapshot fixture replaces the live replay with equal-size "
         "bytes");
  expect(actions.inspect({.resultId = saved.receipt->resultId}).availability ==
             ReplayAvailability::Corrupt,
         "equal-size source replacement invalidates the live replay");
  expect(readBytes(*prepared.sourcePath) == original,
         "the path handed to replay export remains the verified original "
         "after live source replacement");
}

void testFileActionsRejectReplayUnderDifferentChartStem() {
  TemporaryDirectory temporary("action-stem-validation");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174031");
  const auto saved = environment.coordinator.persist(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "stem action fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }

  const auto loaded =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
         "stem action fixture reference loads");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }

  const std::string wrongStem = repeated('d', 64);
  const auto wrongRelativePath =
      std::filesystem::path("replay") / (wrongStem + ".brd");
  std::error_code renameError;
  std::filesystem::rename(temporary.path() /
                              loaded.record->replayFile->relativePath,
                          temporary.path() / wrongRelativePath, renameError);
  expect(!renameError &&
             executeSql(environment.replayDatabase,
                        "UPDATE replay_files SET stem='" + wrongStem +
                            "',relative_path='" +
                            wrongRelativePath.generic_string() +
                            "' WHERE chart_result_id=" +
                            std::to_string(saved.receipt->resultId)),
         "stem action fixture moves the valid replay under another canonical "
         "chart stem");
  if (renameError) {
    return;
  }

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::ChartResult,
      .resultId = saved.receipt->resultId,
  });
  expect(inspected.availability == ReplayAvailability::Corrupt &&
             !inspected.sourcePath.has_value(),
         "file actions reject a valid BRD stored under the wrong chart stem");
}

void testStockFallbackKeepsUndefinedLongNoteChartPathAvailable() {
  TemporaryDirectory temporary("stock-undefined-ln-chart");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174032");
  attempt.result.score.longNoteMode = 2;
  attempt.result.score.provenance.stages.front().longNoteMode = 2;
  attempt.replay.setup.longNoteMode = 2;
  attempt.replay.setup.hasUndefinedLongNotes = true;
  attempt.replay.setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  attempt.replay.setup.startingGaugePercent = 100.0F;
  attempt.replay.setup.startingGaugeState.reset();
  attempt.result.score.provenance.gaugeAutoShift =
      GaugeAutoShiftMode::BestClear;
  attempt.result.resultFingerprint = resultFingerprint(attempt.result);
  std::string snapshotDiagnostic;
  const auto snapshot =
      ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
  expect(snapshot.has_value(), "stock fallback chart snapshot captures");
  if (!snapshot.has_value()) {
    return;
  }
  attempt.irSnapshot = *snapshot;

  const auto saved = environment.coordinator.persist(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "undefined-LN CN chart persists under a Beatoraja C stem");
  if (!saved.receipt.has_value()) {
    return;
  }
  const auto loaded =
      environment.replayRepository.loadChartResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value() &&
             loaded.record->replayFile->stem ==
                 "C" + attempt.result.score.chartSha256,
         "undefined-LN CN chart owns the stock-compatible C stem");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }

  const auto &reference = *loaded.record->replayFile;
  const std::array chartStage{std::size_t{0}};
  const bool replaced = replaceReplayStagesWithStockFallback(
      environment, reference, "chart_result_id", saved.receipt->resultId,
      chartStage);
  expect(replaced,
         "chart fixture becomes an owned stock fallback BRD");
  if (!replaced) {
    return;
  }

  const auto playback = environment.replayRepository.loadChartReplayPlayback(
      saved.receipt->resultId);
  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::ChartResult,
      .resultId = saved.receipt->resultId,
  });
  expect(playback.status == ChartReplayPlaybackReadOutcome::Status::Loaded &&
             playback.playback.has_value() &&
             playback.playback->setup.longNoteMode == 2 &&
             playback.playback->setup.playbackRulesetId ==
                 RulesetDescriptor::Current().id &&
             playback.playback->setup.startingGaugeState.has_value() &&
             playback.playback->setup.startingGaugeState->gaugeType ==
                 GaugeType::Hazard &&
             playback.playback->setup.startingGaugeState
                     ->gaugeValues[gaugeTypeIndex(GaugeType::Normal)] ==
                 20.0F &&
             inspected.availability == ReplayAvailability::Available,
         "stock fallback inherits saved setup and deterministic best-clear "
         "state while remaining playable and shareable at its valid C stem");
}

void setCourseStageLongNoteContext(CompletedCourseAttempt &attempt,
                                   std::size_t index,
                                   bool hasUndefinedLongNotes) {
  attempt.replay.stages[index].setup.longNoteMode = 2;
  attempt.replay.stages[index].setup.hasUndefinedLongNotes =
      hasUndefinedLongNotes;
  auto &score = attempt.result.stages[index].score;
  score.longNoteMode = 2;
  score.provenance.stages.front().longNoteMode = 2;
}

void testCourseStockFallbackPreservesUnknownLongNotePathFact() {
  TemporaryDirectory temporary("stock-undefined-ln-course");
  Environment environment(temporary.path());
  auto attempt =
      validCourseAttempt("123e4567-e89b-42d3-a456-426614174033");
  setCourseStageLongNoteContext(attempt, 0, false);
  setCourseStageLongNoteContext(attempt, 1, true);
  attempt.result.resultFingerprint = resultFingerprint(attempt.result);

  const auto saved = environment.coordinator.persistCourse(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "course with one undefined-LN stage persists at a C stem");
  if (!saved.receipt.has_value()) {
    return;
  }
  const auto loaded =
      environment.replayRepository.loadCourseResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value() &&
             loaded.record->replayFile->stem.starts_with('C'),
         "undefined-LN course owns a stock-compatible C stem");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }

  const std::array futureStage{std::size_t{1}};
  const bool replaced = replaceReplayStagesWithStockFallback(
      environment, *loaded.record->replayFile, "course_result_id",
      saved.receipt->resultId, futureStage);
  expect(replaced,
         "the only undefined-LN course stage becomes stock fallback");
  if (!replaced) {
    return;
  }

  const auto playback = environment.replayRepository.loadCourseReplayPlayback(
      saved.receipt->resultId);
  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::CourseResult,
      .resultId = saved.receipt->resultId,
  });
  expect(playback.status == CourseReplayPlaybackReadOutcome::Status::Loaded &&
             playback.playback.has_value() &&
             inspected.availability == ReplayAvailability::Available,
         "unknown fallback stage keeps the valid prefixed course available");
}

void testStockFallbackAcceptsNoLongNoteResults() {
  {
    TemporaryDirectory temporary("stock-no-ln-chart");
    Environment environment(temporary.path());
    auto attempt =
        validAttempt("123e4567-e89b-42d3-a456-426614174035");
    attempt.result.score.longNoteMode = 0;
    attempt.replay.setup.longNoteMode = 0;
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    std::string snapshotDiagnostic;
    const auto snapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    expect(snapshot.has_value(), "no-LN chart snapshot captures");
    if (!snapshot.has_value()) {
      return;
    }
    attempt.irSnapshot = *snapshot;

    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "no-LN chart persists before fallback");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadChartResult(saved.receipt->resultId);
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      expect(false, "no-LN chart exposes its replay reference");
      return;
    }
    const std::array fallbackStage{std::size_t{0}};
    expect(replaceReplayStagesWithStockFallback(
               environment, *loaded.record->replayFile, "chart_result_id",
               saved.receipt->resultId, fallbackStage),
           "no-LN chart becomes a stock fallback");
    const auto playback = environment.replayRepository.loadChartReplayPlayback(
        saved.receipt->resultId);
    expect(playback.status ==
                   ChartReplayPlaybackReadOutcome::Status::Loaded &&
               playback.playback.has_value() &&
               playback.playback->setup.longNoteMode == 0,
           "stock mode zero preserves an application no-LN chart result");
  }

  {
    TemporaryDirectory temporary("stock-no-ln-course");
    Environment environment(temporary.path());
    auto attempt = validCourseAttempt(
        "123e4567-e89b-42d3-a456-426614174036");
    for (std::size_t index = 0; index < attempt.replay.stages.size(); ++index) {
      attempt.replay.stages[index].setup.longNoteMode = 0;
      attempt.result.stages[index].score.longNoteMode = 0;
    }
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    const auto saved = environment.coordinator.persistCourse(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "no-LN course persists before fallback");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadCourseResult(saved.receipt->resultId);
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      expect(false, "no-LN course exposes its replay reference");
      return;
    }
    const std::array fallbackStage{std::size_t{0}};
    expect(replaceReplayStagesWithStockFallback(
               environment, *loaded.record->replayFile, "course_result_id",
               saved.receipt->resultId, fallbackStage),
           "no-LN course becomes a mixed stock fallback");
    const auto playback =
        environment.replayRepository.loadCourseReplayPlayback(
            saved.receipt->resultId);
    expect(playback.status ==
                   CourseReplayPlaybackReadOutcome::Status::Loaded &&
               playback.playback.has_value() &&
               playback.playback->stages.front().setup.longNoteMode == 0,
           "stock mode zero preserves application no-LN course stages");
  }
}

void testManualAssignmentFallbackFailsClosed() {
  {
    TemporaryDirectory temporary("stock-manual-chart");
    Environment environment(temporary.path());
    auto attempt =
        validAttempt("123e4567-e89b-42d3-a456-426614174037");
    attempt.replay.setup.playOption = "ASSIGN:S2134567";
    attempt.result.score.provenance.player1.option = "ASSIGN:S2134567";
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    std::string snapshotDiagnostic;
    const auto snapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    expect(snapshot.has_value(), "manual chart snapshot captures");
    if (!snapshot.has_value()) {
      return;
    }
    attempt.irSnapshot = *snapshot;

    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "manual chart persists with its supported extension");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadChartResult(saved.receipt->resultId);
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      expect(false, "manual chart exposes its replay reference");
      return;
    }
    const std::array fallbackStage{std::size_t{0}};
    expect(replaceReplayStagesWithStockFallback(
               environment, *loaded.record->replayFile, "chart_result_id",
               saved.receipt->resultId, fallbackStage),
           "manual chart extension becomes unsupported");
    const auto playback = environment.replayRepository.loadChartReplayPlayback(
        saved.receipt->resultId);
    expect(playback.status ==
               ChartReplayPlaybackReadOutcome::Status::IntegrityConflict,
           "stock NORMAL cannot authenticate saved chart ASSIGN provenance");
  }

  {
    TemporaryDirectory temporary("stock-manual-course");
    Environment environment(temporary.path());
    auto attempt = validCourseAttempt(
        "123e4567-e89b-42d3-a456-426614174038");
    attempt.replay.stages[1].setup.playOption = "ASSIGN:S2134567";
    attempt.result.stages[1].score.provenance.player1.option =
        "ASSIGN:S2134567";
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);

    const auto saved = environment.coordinator.persistCourse(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "manual course persists with its supported extension");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadCourseResult(saved.receipt->resultId);
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      expect(false, "manual course exposes its replay reference");
      return;
    }
    const std::array fallbackStage{std::size_t{1}};
    expect(replaceReplayStagesWithStockFallback(
               environment, *loaded.record->replayFile, "course_result_id",
               saved.receipt->resultId, fallbackStage),
           "manual course stage extension becomes unsupported");
    const auto playback =
        environment.replayRepository.loadCourseReplayPlayback(
            saved.receipt->resultId);
    expect(playback.status ==
               CourseReplayPlaybackReadOutcome::Status::IntegrityConflict,
           "stock NORMAL cannot authenticate saved course ASSIGN provenance");
  }
}

void testKnownUndefinedLongNoteCourseStillRequiresPrefixWithFallbackStage() {
  TemporaryDirectory temporary("mixed-known-undefined-ln-course");
  Environment environment(temporary.path());
  auto attempt =
      validCourseAttempt("123e4567-e89b-42d3-a456-426614174034");
  setCourseStageLongNoteContext(attempt, 0, true);
  setCourseStageLongNoteContext(attempt, 1, false);
  attempt.result.resultFingerprint = resultFingerprint(attempt.result);

  const auto saved = environment.coordinator.persistCourse(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "known undefined-LN course fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }
  const auto loaded =
      environment.replayRepository.loadCourseResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value() &&
             loaded.record->replayFile->stem.starts_with('C'),
         "known undefined-LN course fixture owns a C stem");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }

  const std::array futureStage{std::size_t{1}};
  const bool replaced = replaceReplayStagesWithStockFallback(
      environment, *loaded.record->replayFile, "course_result_id",
      saved.receipt->resultId, futureStage);
  expect(replaced, "known undefined-LN course gains one fallback stage");
  if (!replaced) {
    return;
  }

  const auto &reference = *loaded.record->replayFile;
  const std::string wrongStem = reference.stem.substr(1);
  const auto wrongRelativePath =
      std::filesystem::path("replay") / (wrongStem + ".brd");
  std::error_code renameError;
  std::filesystem::rename(temporary.path() / reference.relativePath,
                          temporary.path() / wrongRelativePath, renameError);
  const bool moved =
      !renameError &&
      executeSql(environment.replayDatabase,
                 "UPDATE replay_files SET stem='" + wrongStem +
                     "',relative_path='" + wrongRelativePath.generic_string() +
                     "' WHERE course_result_id=" +
                     std::to_string(saved.receipt->resultId));
  expect(moved, "mixed-source course fixture removes its required prefix");
  if (!moved) {
    return;
  }

  const auto playback = environment.replayRepository.loadCourseReplayPlayback(
      saved.receipt->resultId);
  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::CourseResult,
      .resultId = saved.receipt->resultId,
  });
  expect(playback.status ==
                 CourseReplayPlaybackReadOutcome::Status::IntegrityConflict &&
             inspected.availability == ReplayAvailability::Corrupt,
         "a supported true fact still rejects an unprefixed mixed-source "
         "course");
}

void testOccupiedSlotReplacementRelocatesDisplacedReference() {
  TemporaryDirectory temporary("occupied-slot-relocation");
  Environment environment(temporary.path());
  std::vector<int> resultIds;
  std::vector<ReplayFileReference> references;
  for (int index = 0; index < 5; ++index) {
    auto attempt = validAttempt("123e4567-e89b-42d3-a456-42661417413" +
                                std::to_string(index));
    attempt.result.playedAtUnixMillis += index * 1000;
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    std::string snapshotDiagnostic;
    const auto snapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    expect(snapshot.has_value(), "slot relocation snapshot captures");
    if (!snapshot.has_value()) {
      return;
    }
    attempt.irSnapshot = *snapshot;

    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "slot relocation replay persists");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadChartResult(saved.receipt->resultId);
    expect(loaded.status == ResultReadOutcome::Status::Loaded &&
               loaded.record.has_value() &&
               loaded.record->replayFile.has_value(),
           "slot relocation replay reference loads");
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      return;
    }
    expect(loaded.record->replayFile->historyIndex == index,
           "slot relocation fixtures occupy sequential history paths");
    resultIds.push_back(saved.receipt->resultId);
    references.push_back(*loaded.record->replayFile);
  }

  const auto displacedBytes =
      readBytes(temporary.path() / references[1].relativePath);
  const auto selectedBytes =
      readBytes(temporary.path() / references[4].relativePath);
  expect(displacedBytes != selectedBytes,
         "occupied slot contains different replay bytes before replacement");

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const LocalResultRecordId displacedId{
      .kind = ReplayFileReference::RecordKind::ChartResult,
      .resultId = resultIds[1],
  };
  const LocalResultRecordId selectedId{
      .kind = ReplayFileReference::RecordKind::ChartResult,
      .resultId = resultIds[4],
  };
  const auto copied = actions.copyToBeatorajaSlot(selectedId, 1);
  expect(copied.changed && copied.availability == ReplayAvailability::Available,
         "occupied visible slot replacement succeeds");

  const auto displaced =
      environment.replayRepository.loadChartResult(resultIds[1]);
  const auto selected =
      environment.replayRepository.loadChartResult(resultIds[4]);
  expect(displaced.record.has_value() &&
             displaced.record->replayFile.has_value() &&
             displaced.record->replayFile->historyIndex == 5,
         "displaced replay reference moves to the next history path");
  expect(selected.record.has_value() &&
             selected.record->replayFile.has_value() &&
             selected.record->replayFile->historyIndex == 4,
         "selected result keeps its original replay reference");

  const std::string &stem = references[4].stem;
  expect(readBytes(temporary.path() / "replay" / (stem + "_1.brd")) ==
             selectedBytes,
         "visible slot contains the selected replay bytes");
  expect(readBytes(temporary.path() / "replay" / (stem + "_5.brd")) ==
             displacedBytes,
         "displaced replay bytes survive at the relocated reference");
  expect(actions.inspect(displacedId).availability ==
                 ReplayAvailability::Available &&
             actions.inspect(selectedId).availability ==
                 ReplayAvailability::Available,
         "both result replay references remain valid");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM replay_files") == 5 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_file_reservations") == 0,
         "relocation consumes its reservation without dropping references");
}

void testFailedSlotRelocationKeepsOwnedCopyForStartupRecovery() {
  TemporaryDirectory temporary("failed-slot-relocation-recovery");
  Environment environment(temporary.path(),
                          {.failAt = [](std::string_view point) {
                            return point == "remove-after-quarantine";
                          }});
  std::vector<int> resultIds;
  std::vector<ReplayFileReference> references;
  for (int index = 0; index < 2; ++index) {
    auto attempt = validAttempt("123e4567-e89b-42d3-a456-42661417414" +
                                std::to_string(index));
    attempt.result.playedAtUnixMillis += index * 1000;
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    std::string snapshotDiagnostic;
    const auto snapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    expect(snapshot.has_value(),
           "failed relocation fixture captures its IR snapshot");
    if (!snapshot.has_value()) {
      return;
    }
    attempt.irSnapshot = *snapshot;
    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "failed relocation fixture persists");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadChartResult(saved.receipt->resultId);
    expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
           "failed relocation fixture loads its replay reference");
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      return;
    }
    resultIds.push_back(saved.receipt->resultId);
    references.push_back(*loaded.record->replayFile);
  }
  if (resultIds.size() != 2 || references.size() != 2) {
    return;
  }

  expect(executeSql(environment.replayDatabase,
                    "CREATE TRIGGER fail_replay_relocation BEFORE UPDATE OF "
                    "history_index,relative_path ON replay_files BEGIN SELECT "
                    "RAISE(ABORT,'injected replay relocation failure'); END"),
         "failed relocation fixture rejects the reference update");
  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto copied = actions.copyToBeatorajaSlot(
      {.kind = ReplayFileReference::RecordKind::ChartResult,
       .resultId = resultIds[1]},
      0);
  const auto relocationPath =
      temporary.path() / "replay" / (references[0].stem + "_2.brd");
  expect(!copied.changed &&
             copied.availability == ReplayAvailability::IoFailure,
         "failed reference relocation reports an I/O failure");
  expect(std::filesystem::exists(relocationPath) &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_file_reservations") == 1,
         "ownership-safe rollback preserves the copied file and reservation "
         "when deletion cannot prove ownership");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM replay_file_reservations WHERE "
                "finalized_content_sha256 IS NOT NULL AND "
                "finalized_compressed_size IS NOT NULL") == 1,
         "relocation records copied-file ownership before moving the "
         "reference");

  environment.replayRepository.Shutdown();
  ReplayRepository recovered(environment.replayDatabase);
  expect(recovered.EnsureSchema(),
         "startup recovery opens the failed relocation database");
  expect(!std::filesystem::exists(relocationPath) &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_file_reservations") == 0,
         "startup reclaims the proven orphaned relocation copy");
}

void testSlotRelocationMarkerFailureCannotExposeCopiedReplay() {
  TemporaryDirectory temporary("precopy-relocation-ownership");
  Environment environment(temporary.path(),
                          {.failAt = [](std::string_view point) {
                            return point == "remove-after-quarantine";
                          }});
  std::vector<int> resultIds;
  std::vector<ReplayFileReference> references;
  for (int index = 0; index < 2; ++index) {
    auto attempt = validAttempt("123e4567-e89b-42d3-a456-42661417415" +
                                std::to_string(index + 1));
    attempt.result.playedAtUnixMillis += index * 1000;
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    std::string snapshotDiagnostic;
    const auto snapshot =
        ir::captureIrSubmissionSnapshot(attempt.result, snapshotDiagnostic);
    expect(snapshot.has_value(),
           "pre-copy relocation fixture captures its IR snapshot");
    if (!snapshot.has_value()) {
      return;
    }
    attempt.irSnapshot = *snapshot;
    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "pre-copy relocation fixture persists");
    if (!saved.receipt.has_value()) {
      return;
    }
    const auto loaded =
        environment.replayRepository.loadChartResult(saved.receipt->resultId);
    expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
           "pre-copy relocation fixture loads its replay reference");
    if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
      return;
    }
    resultIds.push_back(saved.receipt->resultId);
    references.push_back(*loaded.record->replayFile);
  }
  if (resultIds.size() != 2 || references.size() != 2) {
    return;
  }

  expect(
      executeSql(environment.replayDatabase,
                 "CREATE TRIGGER fail_relocation_ownership BEFORE UPDATE OF "
                 "finalized_content_sha256,finalized_compressed_size ON "
                 "replay_file_reservations BEGIN SELECT "
                 "RAISE(ABORT,'injected relocation ownership failure'); END"),
      "pre-copy relocation fixture rejects marker storage");
  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto copied = actions.copyToBeatorajaSlot(
      {.kind = ReplayFileReference::RecordKind::ChartResult,
       .resultId = resultIds[1]},
      0);
  const auto relocationPath =
      temporary.path() / "replay" / (references[0].stem + "_2.brd");
  expect(!copied.changed &&
             copied.availability == ReplayAvailability::IoFailure &&
             !std::filesystem::exists(relocationPath) &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_file_reservations") == 0,
         "relocation marker failure stops before copied bytes become visible");
}

void testFilesystemFailuresNeverStageDatabaseRows() {
  for (std::string_view fault :
       {"write", "file-sync", "close", "rename", "directory-sync", "read-back",
        "decode", "hash"}) {
    TemporaryDirectory temporary(std::string("fault-") + std::string(fault));
    bool enabled = true;
    Environment environment(temporary.path(),
                            {.failAt = [&](std::string_view point) {
                              return enabled && point == fault;
                            }});
    const auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174001");
    const auto failed = environment.coordinator.persist(attempt);
    expect(
        failed.state == SaveState::UnfinalizedReplay &&
            scalar(environment.replayDatabase,
                   "SELECT count(*) FROM chart_results") == 0 &&
            scalar(environment.replayDatabase,
                   "SELECT count(*) FROM replay_files") == 0,
        std::string("fault before verified finalization stages no DB rows: ") +
            std::string(fault));
    enabled = false;
    const auto retried = environment.coordinator.persist(attempt);
    if (!retried.saved()) {
      std::cerr << "fault retry " << fault
                << " state=" << static_cast<int>(retried.state)
                << " diagnostic=" << retried.diagnostic << '\n';
    }
    expect(retried.saved(),
           std::string("filesystem fault retry reuses reservation/file: ") +
               std::string(fault));
  }
}

void testCrashAfterCompactStageRecoversWithoutReplayReconstruction() {
  TemporaryDirectory temporary("recovery");
  Environment environment(temporary.path());
  const auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174002", 1);
  const auto replayFile = finalizedReference(environment, attempt);
  const auto staged = environment.replayRepository.stageCompletedChartAttempt(
      attempt.result, attempt.irSnapshot, replayFile, {});
  expect(staged.status == StageStatus::Staged && staged.receipt &&
             staged.receipt->scorePending,
         "crash fixture stops after compact staging");
  expect(scalar(environment.replayDatabase,
                "SELECT count(*) FROM pending_chart_score_writes") == 1 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 0,
         "crash fixture has a durable pending projection");

  const auto recovered = environment.coordinator.recoverAll();
  if (recovered.saved != 1) {
    std::cerr << "recovery diagnostic=" << recovered.diagnostic << '\n';
  }
  expect(recovered.attempted == 1 && recovered.saved == 1 &&
             recovered.pending == 0 && recovered.conflicts == 0 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM pending_chart_score_writes") == 0 &&
             scalar(environment.scoreDatabase, "SELECT count(*) FROM scores") ==
                 1,
         "startup recovery projects compact facts without replay events");
}

void testCoursePipelineUsesOneBeatorajaArrayFile() {
  TemporaryDirectory temporary("course");
  Environment environment(temporary.path());
  const auto attempt =
      validCourseAttempt("123e4567-e89b-42d3-a456-426614174012");
  const auto saved = environment.coordinator.persistCourse(attempt);
  if (!saved.saved()) {
    std::cerr << "course pipeline state=" << static_cast<int>(saved.state)
              << " diagnostic=" << saved.diagnostic << '\n';
  }
  expect(saved.saved() && saved.receipt && saved.receipt->resultId > 0,
         "course result and replay file save atomically");
  if (!saved.receipt) {
    return;
  }
  const auto loaded =
      environment.replayRepository.loadCourseResult(saved.receipt->resultId);
  auto expectedResult = attempt.result;
  expectedResult.resultId = saved.receipt->resultId;
  expect(loaded.status == CourseResultReadOutcome::Status::Loaded &&
             loaded.record && loaded.record->replayFile &&
             loaded.record->result == expectedResult,
         "compact ordered course facts load independently");
  if (!loaded.record || !loaded.record->replayFile) {
    return;
  }
  const auto &reference = *loaded.record->replayFile;
  const replay::ReplayFileMetadata metadata{
      .relativePath = reference.relativePath,
      .sha256 = reference.contentSha256,
      .compressedSize = reference.compressedSize,
      .codecVersion = reference.codecVersion,
  };
  const auto decoded = environment.fileStore.load(metadata, environment.codec);
  expect(decoded.course && decoded.course->stages == attempt.replay.stages &&
             decoded.course->restMicrosAfterStage ==
                 attempt.replay.restMicrosAfterStage,
         "one Beatoraja JSON-array .brd round-trips every ordered stage");
  const auto playback = environment.replayRepository.loadCourseReplayPlayback(
      saved.receipt->resultId);
  expect(playback.status == CourseReplayPlaybackReadOutcome::Status::Loaded &&
             playback.result == expectedResult &&
             playback.playback == attempt.replay,
         "course playback is loaded from its referenced .brd file");
  const auto retried = environment.coordinator.persistCourse(attempt);
  expect(retried.saved() && retried.receipt &&
             retried.receipt->resultId == saved.receipt->resultId &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM course_results") == 1 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM course_result_stages") == 2 &&
             scalar(environment.replayDatabase,
                    "SELECT count(*) FROM replay_files") == 1,
         "course persistence retry is idempotent");

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const LocalResultRecordId courseRecord{
      .kind = ReplayFileReference::RecordKind::CourseResult,
      .resultId = saved.receipt->resultId,
  };
  expect(actions.inspect(courseRecord).availability ==
             ReplayAvailability::Available,
         "course action resolves the course row, not a chart with the same ID");
  const auto removed = actions.remove(courseRecord);
  expect(removed.availability == ReplayAvailability::Missing && removed.changed,
         "course replay file can be deleted independently");
  const auto afterDelete =
      environment.replayRepository.loadCourseResult(saved.receipt->resultId);
  expect(afterDelete.status == CourseResultReadOutcome::Status::Loaded &&
             afterDelete.record &&
             afterDelete.record->result.stages.size() == 2,
         "course result recall survives replay-file deletion");
  const auto playbackAfterDelete =
      environment.replayRepository.loadCourseReplayPlayback(
          saved.receipt->resultId);
  expect(
      playbackAfterDelete.status ==
              CourseReplayPlaybackReadOutcome::Status::ReplayUnavailable &&
          playbackAfterDelete.result == expectedResult &&
          !playbackAfterDelete.playback.has_value(),
      "deleted course replay disables playback without deleting result facts");
}

void testCoursePlaybackAndActionsRejectDifferentCourseStem() {
  TemporaryDirectory temporary("course-stem-validation");
  Environment environment(temporary.path());
  const auto attempt =
      validCourseAttempt("123e4567-e89b-42d3-a456-426614174014");
  const auto saved = environment.coordinator.persistCourse(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "course stem fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }

  const auto loaded =
      environment.replayRepository.loadCourseResult(saved.receipt->resultId);
  expect(loaded.record.has_value() && loaded.record->replayFile.has_value(),
         "course stem fixture reference loads");
  if (!loaded.record.has_value() || !loaded.record->replayFile.has_value()) {
    return;
  }

  const std::string wrongStem = repeated('d', 10);
  const auto wrongRelativePath =
      std::filesystem::path("replay") / (wrongStem + ".brd");
  std::error_code renameError;
  std::filesystem::rename(temporary.path() /
                              loaded.record->replayFile->relativePath,
                          temporary.path() / wrongRelativePath, renameError);
  expect(!renameError &&
             executeSql(environment.replayDatabase,
                        "UPDATE replay_files SET stem='" + wrongStem +
                            "',relative_path='" +
                            wrongRelativePath.generic_string() +
                            "' WHERE course_result_id=" +
                            std::to_string(saved.receipt->resultId)),
         "course stem fixture moves the valid replay under another canonical "
         "stem");
  if (renameError) {
    return;
  }

  const auto playback = environment.replayRepository.loadCourseReplayPlayback(
      saved.receipt->resultId);
  expect(playback.status ==
             CourseReplayPlaybackReadOutcome::Status::IntegrityConflict,
         "course playback rejects a filename stem unrelated to decoded setup");

  ReplayFileActionService actions(environment.replayRepository,
                                  environment.fileStore);
  const auto inspected = actions.inspect({
      .kind = ReplayFileReference::RecordKind::CourseResult,
      .resultId = saved.receipt->resultId,
  });
  expect(inspected.availability == ReplayAvailability::Corrupt &&
             !inspected.sourcePath.has_value(),
         "course actions reject a valid BRD under the wrong course stem");
}

void testVersion13CourseEntryFactsMigration() {
  TemporaryDirectory temporary("course-entry-facts-migration");
  auto attempt =
      validCourseAttempt("123e4567-e89b-42d3-a456-426614174013");
  for (auto &facts : attempt.result.entryFacts) {
    facts.playLengthMicros = 0;
  }
  attempt.result.resultFingerprint = resultFingerprint(attempt.result);
  int resultId = 0;
  {
    Environment environment(temporary.path());
    const auto saved = environment.coordinator.persistCourse(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "schema-13 course fixture persists");
    if (!saved.receipt.has_value()) {
      return;
    }
    resultId = saved.receipt->resultId;
  }

  const auto databasePath = temporary.path() / "replay.db";
  expect(executeSql(
             databasePath,
             "PRAGMA writable_schema=ON; UPDATE sqlite_master SET "
             "sql=replace(sql,',entry_facts_json TEXT NOT NULL','') WHERE "
             "type='table' AND name='course_results'; "
             "PRAGMA writable_schema=OFF; PRAGMA user_version=13;"),
         "schema-13 course fixture removes the new column");

  ReplayRepository repository(databasePath);
  expect(repository.EnsureSchema(),
         "schema-13 course entry facts migrate to the current schema");
  const auto loaded = repository.loadCourseResult(resultId);
  auto expected = attempt.result;
  expected.resultId = resultId;
  const bool migrated =
      scalar(databasePath, "PRAGMA user_version") == 14 &&
      loaded.status == CourseResultReadOutcome::Status::Loaded &&
      loaded.record.has_value() && loaded.record->result == expected;
  if (!migrated) {
    std::cerr << "course facts migration status="
              << static_cast<int>(loaded.status)
              << " diagnostic=" << loaded.diagnostic << '\n';
  }
  expect(migrated,
         "course entry facts backfill and existing fingerprint remain valid");
}

void testReplayVolumeOnlyGrowsTheBrdFile() {
  TemporaryDirectory smallRoot("size-small");
  TemporaryDirectory largeRoot("size-large");
  Environment small(smallRoot.path());
  Environment large(largeRoot.path());

  auto makeAttempt = [](std::size_t transitionCount) {
    auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174020");
    attempt.replay.input.clear();
    attempt.replay.input.reserve(transitionCount);
    for (std::size_t index = 0; index < transitionCount; ++index) {
      attempt.replay.input.push_back(
          {.songTimeMicros = 1'000 + static_cast<std::int64_t>(index) * 100,
           .control = {.kind = replay::LogicalControlKind::Lane,
                       .player = 1,
                       .lane = static_cast<int>((index / 2) % 7)},
           .pressed = index % 2 == 0});
    }
    return attempt;
  };

  const auto smallSaved = small.coordinator.persist(makeAttempt(100));
  const auto largeSaved = large.coordinator.persist(makeAttempt(100'000));
  if (!smallSaved.saved()) {
    std::cerr << "small replay volume state="
              << static_cast<int>(smallSaved.state)
              << " diagnostic=" << smallSaved.diagnostic << '\n';
  }
  if (!largeSaved.saved()) {
    std::cerr << "large replay volume state="
              << static_cast<int>(largeSaved.state)
              << " diagnostic=" << largeSaved.diagnostic << '\n';
  }
  expect(smallSaved.saved() && smallSaved.receipt && largeSaved.saved() &&
             largeSaved.receipt,
         "100 and 100,000 transition attempts both persist");
  if (!smallSaved.receipt || !largeSaved.receipt) {
    return;
  }

  const auto smallResult =
      small.replayRepository.loadChartResult(smallSaved.receipt->resultId);
  const auto largeResult =
      large.replayRepository.loadChartResult(largeSaved.receipt->resultId);
  expect(smallResult.record && smallResult.record->replayFile &&
             largeResult.record && largeResult.record->replayFile,
         "both volume fixtures expose replay-file references");
  if (!smallResult.record || !smallResult.record->replayFile ||
      !largeResult.record || !largeResult.record->replayFile) {
    return;
  }

  const auto smallReplaySize = std::filesystem::file_size(
      smallRoot.path() / smallResult.record->replayFile->relativePath);
  const auto largeReplaySize = std::filesystem::file_size(
      largeRoot.path() / largeResult.record->replayFile->relativePath);
  expect(largeReplaySize > smallReplaySize,
         "additional transitions grow the .brd file");
  expect(tableRowCounts(small.replayDatabase) ==
                 tableRowCounts(large.replayDatabase) &&
             tableRowCounts(small.scoreDatabase) ==
                 tableRowCounts(large.scoreDatabase),
         "transition volume does not change SQLite row cardinality");
  expect(std::filesystem::file_size(small.replayDatabase) ==
                 std::filesystem::file_size(large.replayDatabase) &&
             std::filesystem::file_size(small.scoreDatabase) ==
                 std::filesystem::file_size(large.scoreDatabase),
         "transition-volume growth is confined to standalone .brd bytes");
}

void testSummaryLimitsCountOnlyValidatedRows() {
  TemporaryDirectory temporary("summary-valid-limit");
  Environment environment(temporary.path());

  std::vector<int> chartIds;
  for (int index = 0; index < 3; ++index) {
    auto attempt = validAttempt("123e4567-e89b-42d3-a456-42661417410" +
                                std::to_string(index));
    const auto saved = environment.coordinator.persist(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "chart summary corruption fixture persists");
    if (saved.receipt.has_value()) {
      chartIds.push_back(saved.receipt->resultId);
    }
  }
  expect(chartIds.size() == 3, "three chart summary fixtures are available");
  if (chartIds.size() != 3) {
    return;
  }
  expect(executeSql(environment.replayDatabase,
                    "UPDATE chart_results SET result_fingerprint='bad' "
                    "WHERE id=" +
                        std::to_string(chartIds.back())),
         "newest chart summary row is corrupted");
  bms_parser::ChartMeta chartLookup;
  chartLookup.MD5 = repeated('b', 32);
  chartLookup.SHA256 = repeated('a', 64);
  const auto charts = environment.replayRepository.ListReplays(chartLookup, 2);
  expect(charts.size() == 2 && charts[0].id == chartIds[1] &&
             charts[1].id == chartIds[0],
         "chart summary limit counts valid rows after corrupt newest row");

  std::vector<int> courseIds;
  std::string courseKey;
  for (int index = 0; index < 2; ++index) {
    auto attempt = validCourseAttempt("123e4567-e89b-42d3-a456-42661417411" +
                                      std::to_string(index));
    attempt.result.playedAtUnixMillis += index;
    attempt.result.resultFingerprint = resultFingerprint(attempt.result);
    courseKey = attempt.result.courseKey;
    const auto saved = environment.coordinator.persistCourse(attempt);
    expect(saved.saved() && saved.receipt.has_value(),
           "course summary corruption fixture persists");
    if (saved.receipt.has_value()) {
      courseIds.push_back(saved.receipt->resultId);
    }
  }
  expect(courseIds.size() == 2, "two course summary fixtures are available");
  if (courseIds.size() != 2) {
    return;
  }
  expect(executeSql(environment.replayDatabase,
                    "UPDATE course_results SET result_fingerprint='bad' "
                    "WHERE id=" +
                        std::to_string(courseIds.back())),
         "newest course summary row is corrupted");
  const auto courses = environment.replayRepository.ListCourseReplays(
      {.courseKey = courseKey, .legacyCourseId = 7}, 1);
  expect(courses.size() == 1 && courses[0].id == courseIds[0],
         "course summary limit counts valid rows after corrupt newest row");
}

void testSummaryCorruptionAllowanceIsBounded() {
  TemporaryDirectory temporary("summary-corruption-bound");
  Environment environment(temporary.path());
  auto attempt = validAttempt("123e4567-e89b-42d3-a456-426614174120");
  const auto saved = environment.coordinator.persist(attempt);
  expect(saved.saved() && saved.receipt.has_value(),
         "bounded summary fixture persists");
  if (!saved.receipt.has_value()) {
    return;
  }

  const int corruptCount = replay_summary_scan::kCorruptCandidateAllowance + 1;
  const std::string cloneSql =
      "WITH RECURSIVE seq(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM seq "
      "WHERE x<" +
      std::to_string(corruptCount) +
      ") INSERT INTO chart_results("
      "attempt_id,chart_path,chart_md5,chart_sha256,chart_title,chart_artist,"
      "key_mode,long_note_mode,score,max_score,max_combo,combo_break,p_great,"
      "great,good,bad,poor,k_poor,fast,slow,final_gauge,clear_type,"
      "adopted_gauge_type,gauge_history_json,judgement_timing_json,"
      "provenance_json,"
      "result_fingerprint,played_at_unix_ms) SELECT NULL,chart_path,chart_md5,"
      "chart_sha256,chart_title,chart_artist,key_mode,long_note_mode,score,"
      "max_score,max_combo,combo_break,p_great,great,good,bad,poor,k_poor,"
      "fast,slow,final_gauge,clear_type,adopted_gauge_type,gauge_history_json,"
      "judgement_timing_json,provenance_json,'bad',played_at_unix_ms+x "
      "FROM chart_results JOIN seq WHERE id=" +
      std::to_string(saved.receipt->resultId);
  expect(executeSql(environment.replayDatabase, cloneSql),
         "corrupt summary candidates are cloned ahead of the valid row");

  bms_parser::ChartMeta chartLookup;
  chartLookup.MD5 = attempt.result.score.chartMd5;
  chartLookup.SHA256 = attempt.result.score.chartSha256;
  const auto bounded = environment.replayRepository.ListReplays(chartLookup, 1);
  expect(bounded.empty(),
         "positive summary limit stops at the corruption allowance");
  const auto unbounded =
      environment.replayRepository.ListReplays(chartLookup, 0);
  expect(unbounded.size() == 1 &&
             unbounded.front().id == saved.receipt->resultId,
         "explicit unbounded summary scan reaches the older valid row");
}

} // namespace

int main() {
  testCompleteFileAndDatabasePipeline();
  testOwnershipMarkerFailureCannotExposeFinalReplay();
  testFileActionsRejectReplayFromDifferentSavedProvenance();
  testReplayShareUsesVerifiedSnapshotAfterSourceReplacement();
  testFileActionsRejectReplayUnderDifferentChartStem();
  testStockFallbackKeepsUndefinedLongNoteChartPathAvailable();
  testCourseStockFallbackPreservesUnknownLongNotePathFact();
  testStockFallbackAcceptsNoLongNoteResults();
  testManualAssignmentFallbackFailsClosed();
  testKnownUndefinedLongNoteCourseStillRequiresPrefixWithFallbackStage();
  testOccupiedSlotReplacementRelocatesDisplacedReference();
  testFailedSlotRelocationKeepsOwnedCopyForStartupRecovery();
  testSlotRelocationMarkerFailureCannotExposeCopiedReplay();
  testFilesystemFailuresNeverStageDatabaseRows();
  testCrashAfterCompactStageRecoversWithoutReplayReconstruction();
  testCoursePipelineUsesOneBeatorajaArrayFile();
  testCoursePlaybackAndActionsRejectDifferentCourseStem();
  testVersion13CourseEntryFactsMigration();
  testReplayVolumeOnlyGrowsTheBrdFile();
  testSummaryLimitsCountOnlyValidatedRows();
  testSummaryCorruptionAllowanceIsBounded();
  if (failures != 0) {
    std::cerr << failures << " result persistence integration test(s) failed\n";
    return 1;
  }
  std::cout << "Result persistence v11 integration tests passed\n";
  return 0;
}
