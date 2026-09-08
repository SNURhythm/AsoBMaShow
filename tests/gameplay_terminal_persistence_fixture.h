#pragma once

#include "ModernResultRecallBuilder.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "repositories/ScoreCacheQueries.h"
#include "replay/ChartReplayCapture.h"
#include "replay/ChartReplayConsumer.h"
#include "replay/ReplayPlaybackMaterializer.h"
#include "replay/ReplayFileStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>

#ifdef TASK2_CHECK_HEAD_READER
namespace replay {
ReplayDecodeOutcome task2DecodeWithHeadCodec(
    std::span<const std::byte>, const ReplayDecodeContext &);
}
#endif

std::unique_ptr<bms_parser::Chart> freshAbortChart() {
  GamePlayScene fresh;
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta = fresh.chart->Meta;
  chart->Measures.swap(fresh.chart->Measures);
  return chart;
}

void configureAbortCapture(GamePlayScene &scene) {
  scene.options.ruleset = scene.state->gaugeRules().ruleset;
  const auto policy = buildGameplayRulesetPolicyAtPlayStart(
      scene.options, scene.chart->Meta, AppSettings::NotePriorityMode::Lowest);
  require(policy.built(), policy.diagnostic);
  scene.attemptProvenance = captureScoreProvenanceAtPlayStart(
      scene.options, scene.chart->Meta, *policy.policy);
  scene.modernReplayInputRecorder = std::make_unique<replay::ReplayInputRecorder>();
}

void recordAcceptedHit(GamePlayScene &scene) {
  std::string diagnostic;
  const replay::LogicalControl control{
      .kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 0};
  require(scene.modernReplayInputRecorder->recordSongTime(
              2'000'000, control, true, diagnostic), diagnostic);
  require(scene.modernReplayInputRecorder->recordSongTime(
              2'010'000, control, false, diagnostic), diagnostic);
}

struct AbortTemporaryDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("asobmashow-terminal-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  AbortTemporaryDirectory() { require(std::filesystem::create_directories(path), "temporary directory"); }
  ~AbortTemporaryDirectory() { std::filesystem::remove_all(path); }
};

std::shared_ptr<ReplayData> testDurableAbort(GamePlayScene &scene, bool midway) {
  AbortTemporaryDirectory temporary;
  std::string diagnostic;
  auto captured = result_persistence::captureModernChartResult(
      "123e4567-e89b-42d3-a456-426614174000", scene.chart->Meta, *scene.state,
      scene.attemptProvenance, 0, 1'700'000'000'000, diagnostic);
  require(captured.has_value(), diagnostic);
  require(captured->score.clearType == kClearTypeFailedRank &&
              captured->score.poor == (midway ? 1 : 2),
          "durable capture retains abort failure and note accounting");
  const auto rawCapture = scene.completeModernReplayCapture();
  require(rawCapture.acceptedInput.has_value(), "actual scene accepts modern raw input");
  const auto attempt = replay::captureChartReplayPersistenceAttempt(
      {.result = *captured,
       .setupFacts = {.chart = {.md5 = scene.chart->Meta.MD5,
                                .sha256 = scene.chart->Meta.SHA256, .keyMode = 7},
                      .longNoteMode = 1},
       .acceptedInput = rawCapture.acceptedInput,
       .touchSamples = rawCapture.touchSamples,
       .laneCoverEvents = rawCapture.laneCoverEvents,
       .timeBounds = rawCapture.timeBounds}, diagnostic);
  require(attempt && attempt->replay, diagnostic);
  replay::BeatorajaReplayCodec codec;
  const auto bytes = codec.encodeChart(*attempt->replay, captured->playedAtUnixMillis, diagnostic);
  require(bytes.has_value(), diagnostic);
  const auto identity = replay::pathForStem(scene.chart->Meta.SHA256, 0, diagnostic);
  require(identity.has_value(), diagnostic);
  replay::ReplayFileStore fileStore(temporary.path);
  const auto reserved = fileStore.reserve(*identity, *bytes, captured->attemptId);
  require(reserved.reservation.has_value(), reserved.diagnostic);
  const auto installed = fileStore.install(*reserved.reservation, *bytes);
  require(installed.file.has_value(), installed.diagnostic);
  const auto replayPath = temporary.path / "replay.db";
  const auto scorePath = temporary.path / "score.db";
  {
    ReplayRepository repository(replayPath);
    require(repository.EnsureSchema(), "modern repository schema");
    const auto pathReservation = repository.ReserveModernReplayPath(
        captured->attemptId, identity->stem, captured->playedAtUnixMillis);
    require(pathReservation.reservation && pathReservation.reservation->identity == *identity,
            pathReservation.diagnostic);
    const auto staged = repository.StageModernChartResult(
        attempt->result, attempt->irSnapshot,
        ModernReplayFileAttachment{.identity = *identity,
                                   .metadata = installed.file->metadata}, {});
    require(staged.status == ModernChartStageStatus::Staged, staged.diagnostic);
    const auto pending = repository.LoadPendingModernChartScore(captured->attemptId);
    require(pending.value.has_value(), "real pending score write loads");
    ScoreRepository score(scorePath);
    require(score.EnsureSchema(), "score schema");
    const auto projected = score.SaveProjectedScore(*pending.value);
    require(projected.status == result_persistence::ProjectionStatus::Inserted,
            "abort projects through the real score repository");
  }
  {
    ReplayRepository reopened(replayPath);
    const auto loaded = reopened.LoadModernChartResultByAttempt(captured->attemptId);
    require(loaded.record.has_value(), "modern result reopens from disk");
    captured = loaded.record->result;
    require(captured->score.clearType == kClearTypeFailedRank &&
                captured->score.finalGauge == 0 &&
                captured->score.poor == (midway ? 1 : 2),
            "reopened result does not turn an abort into a clear");
    ScoreRepository score(scorePath);
    const auto best = score.LoadBestScore(scene.chart->Meta);
    if (!best || best->clearCount != 0 || best->comboBreak != (midway ? 1 : 2)) {
      std::cerr << "Best cache: present=" << best.has_value()
                << " clearCount=" << (best ? best->clearCount : -1)
                << " comboBreak=" << (best ? best->comboBreak.value_or(-1) : -1)
                << '\n';
    }
    require(best && best->clearCount == 0 && best->comboBreak == (midway ? 1 : 2),
            "reopened best-score query has no success or full-combo count");
    sqlite3 *database = nullptr;
    require(sqlite3_open(scorePath.string().c_str(), &database) == SQLITE_OK,
            "open projected cache");
    SqliteConnectionHandle connection(database);
    SqliteStatementHandle statement;
    require(prepareSqliteStatement(database,
                "SELECT MAX(rank) FROM score_sha256_clear_rank_cache", statement) == SQLITE_OK &&
                sqlite3_step(statement.get()) == SQLITE_ROW &&
                sqlite3_column_int(statement.get(), 0) == kClearTypeFailedRank,
            "durable clear-rank cache cannot promote abort to full combo");
  }
  std::atomic_bool cancelled = false;
  auto recalled = result_recall::BuildChartResult(*captured, cancelled,
      [](const std::filesystem::path &, std::atomic_bool &) {
        return freshAbortChart();
      });
  require(recalled.value && recalled.value->state.getClearTypeRank() == kClearTypeFailedRank &&
              recalled.value->state.currentGauge == 0 &&
              std::string_view(recalled.value->state.getClearTypeLabel()) == "FAILED",
          "actual result recall presents failed terminal state");
  const auto stored = fileStore.readVerified(installed.file->metadata);
  require(stored.state == replay::ReplayFileState::Available && stored.bytes,
          "actual file store verifies the accepted abort BRD from disk");
  const auto decoded = codec.decode(*stored.bytes, {.stageKeyModes = {7}});
  require(decoded.chart.has_value(), decoded.diagnostic);
  ReplayRepository consumerRepository(replayPath);
  const auto listed = consumerRepository.LoadModernChartResultByAttempt(captured->attemptId);
  require(listed.record && listed.record->replayFile, "reopened replay attachment is authoritative");
  replay::ChartReplayContext currentContext(consumerRepository);
  const auto admitted = currentContext.load(captured->attemptId);
  require(admitted.replayAvailable(), admitted.diagnostic);
  replay::ChartReplayConsumer consumer({
      .loadContext = [&](std::string_view attemptId) { return currentContext.load(attemptId); },
      .prepareChart = [](const auto &, const auto &, const auto &, auto &, auto &) {
        return freshAbortChart();
      },
      .materialize = [](const auto &document, const auto &result, const auto &chart) {
        auto outcome = replay::ReplayPlaybackMaterializer::materializeForConsumers(
            document, result, chart, 128);
        require(outcome.matched(), outcome.diagnostic);
        return outcome;
      }});
  auto consumed = consumer.load(*listed.record, scene.chart->Meta.BmsPath, cancelled);
  require(consumed.ready() && consumed.replayData->abortedAtSongTimeMicros,
          consumed.diagnostic);
  testAbortExportAdmission(*consumed.replayData);
#ifdef TASK2_CHECK_HEAD_READER
  replay::ChartReplayContext headContext(replay::ChartReplayContextDependencies{
      .loadResult = [&](std::string_view attemptId) {
        return consumerRepository.LoadModernChartResultByAttempt(attemptId);
      },
      .readVerifiedFile = [&](const auto &metadata) { return fileStore.readVerified(metadata); },
      .decode = replay::task2DecodeWithHeadCodec});
  const auto headAdmission = headContext.load(captured->attemptId);
  require(headAdmission.state == replay::ChartReplayContextState::UnsupportedExtension &&
              headAdmission.resultAvailable() && !headAdmission.replayAvailable(),
          "T2-R1: genuine abort bytes must be unavailable to the HEAD reader");
  auto ordinaryDocument = *attempt->replay;
  ordinaryDocument.timeBounds.aborted.reset();
  const auto ordinaryBytes = codec.encodeChart(ordinaryDocument, captured->playedAtUnixMillis, diagnostic);
  require(ordinaryBytes.has_value(), diagnostic);
  const auto oldOrdinary = replay::task2DecodeWithHeadCodec(*ordinaryBytes, {.stageKeyModes = {7}});
  require(oldOrdinary.chart && !oldOrdinary.unsupportedAsoExtension,
          "T2-R1: actual HEAD decoder still accepts newly written ordinary v3 bytes");
#endif
  const auto reproduced = replay::ReplayPlaybackMaterializer::materializeForConsumers(
      *decoded.chart, *captured, *scene.chart, 128);
  if (!reproduced.matched() && reproduced.judgedResult) {
    std::cerr << "Gauge fixture selected=" << gaugeTypeIndex(scene.options.gaugeType)
              << " shift=" << static_cast<int>(scene.options.gaugeAutoShift)
              << " midway=" << midway << " live=" << gaugeTypeIndex(captured->adoptedGaugeType)
              << " replay=" << gaugeTypeIndex(reproduced.judgedResult->adoptedGaugeType)
              << " start=" << decoded.chart->playback.setup.startingGaugePercent
              << " profile=" << static_cast<int>(decoded.chart->playback.setup.gaugeProfile)
              << " live history:";
    for (const auto value : captured->adoptedGaugeHistory) std::cerr << ' ' << value;
    std::cerr << " replay history:";
    for (const auto value : reproduced.judgedResult->adoptedGaugeHistory) std::cerr << ' ' << value;
    std::cerr << '\n';
  }
  require(reproduced.matched() && reproduced.playable(), reproduced.diagnostic);
  require(reproduced.judgedResult->score.poor == (midway ? 1 : 2) &&
              reproduced.judgedResult->score.clearType == kClearTypeFailedRank &&
              reproduced.finalGaugeState->currentGauge == 0,
          "accepted modern raw replay reconstructs the persisted failed outcome");
  GamePlayScene watch;
  watch.state->configureGauge(scene.options.gaugeType, scene.options.gaugeAutoShift);
  watch.options.replayData = consumed.replayData;
  watch.buildReplayNoteLookup();
  watch.context.jukebox.time = decoded.chart->timeBounds.completionSongTimeMicros;
  watch.processReplayEvents(watch.context.jukebox.time);
  require(watch.state->judgeCount[Poor] == (midway ? 1 : 2),
          "T2-R3: admitted MATCH Watch must retain the captured abort judgment count");
  require(watch.transitions == 1 && watch.state->isEnding &&
              watch.state->getClearTypeRank() == kClearTypeFailedRank &&
              watch.state->judgeCount[Poor] == (midway ? 1 : 2),
          "actual consumer scene terminal method exits as failed, not corruption");
  for (const auto flag : {std::optional(false), std::optional<bool>{}}) {
    auto forged = *decoded.chart;
    forged.timeBounds.aborted = flag;
    const auto forgedBytes = codec.encodeChart(forged, captured->playedAtUnixMillis, diagnostic);
    require(forgedBytes.has_value(), diagnostic);
    {
      std::ofstream file(temporary.path / installed.file->metadata.relativePath,
                         std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char *>(forgedBytes->data()), forgedBytes->size());
      require(file.good(), "write tampered test replay");
    }
    require(fileStore.readVerified(installed.file->metadata).state ==
                replay::ReplayFileState::Corrupt,
            "trusted file identity rejects both flipped and omitted abort evidence");
  }
  return consumed.replayData;
}
