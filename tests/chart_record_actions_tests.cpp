// Keep the initial contract runnable before the shared implementation exists.
#if __has_include("scene/ChartRecordActions.h")
#include "scene/ChartRecordActions.h"
#include "PlayOptionUtils.h"
#include "repositories/ChartRepository.h"
#include "repositories/ReplayRepository.h"
#include "replay/ReplayFileStore.h"
#include "replay/ReplaySetupProvenance.h"
#define HAS_CHART_RECORD_ACTIONS 1
#else
#define HAS_CHART_RECORD_ACTIONS 0
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

#if HAS_CHART_RECORD_ACTIONS
struct Fixture {
  std::filesystem::path directory;
  ChartMetaRecord record;
  result_persistence::ModernChartResult result;

  explicit Fixture(bool mirror = false) {
    directory = std::filesystem::temp_directory_path() /
        ("asobmashow-chart-record-actions-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto path = directory / "current-location.bms";
    {
      std::ofstream output(path);
      output << "#PLAYER 1\n#TITLE Current title\n#ARTIST Current artist\n"
                "#BPM 120\n#PLAYLEVEL 1\n#RANK 2\n#TOTAL 200\n"
                "#WAV01 test.wav\n#00111:01010101\n#00119:01\n";
    }
    std::atomic_bool cancelled{false};
    auto chart = play_options::parseChart(path, cancelled, "chart records test");
    require(chart && chart->Meta.TotalNotes == 5 && chart->Meta.KeyMode == 7,
            "disk fixture contains five playable seven-key notes");
    record.meta = chart->Meta;
    result.attemptId = "123e4567-e89b-42d3-a456-426614174000";
    result.score.chartPath = (directory / "old-location.bms").string();
    result.score.chartMd5 = chart->Meta.MD5;
    result.score.chartSha256 = chart->Meta.SHA256;
    result.score.chartTitle = "Saved title";
    result.score.chartArtist = "Saved artist";
    result.score.longNoteMode = 0;
    result.score.score = 7;
    result.score.maxScore = 10;
    result.score.maxCombo = 4;
    result.score.comboBreak = 1;
    result.score.pGreat = 3;
    result.score.great = 1;
    result.score.good = 1;
    result.score.finalGauge = 76.0F;
    result.score.clearType = kClearTypeHardClearRank;
    ScoreProvenanceBuildInput provenance;
    provenance.chartMeta = chart->Meta;
    provenance.longNoteMode = 1;
    provenance.sourceJudgeRank = 2;
    provenance.effectiveJudgeWindows = {
        {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
        {Good, {-75'000, 75'000}}, {Bad, {-200'000, 200'000}},
        {Kpoor, {-1'000'000, 0}},
    };
    provenance.totalNotes = 5;
    provenance.authoredGaugeTotal = 200.0;
    provenance.effectiveGaugeTotal = 200.0;
    provenance.gaugeType = GaugeType::Hard;
    provenance.player1.option = mirror ? "MIRROR" : "NORMAL";
    result.score.provenance = makeScoreProvenance(provenance);
    result.keyMode = 7;
    result.adoptedGaugeType = GaugeType::Hard;
    result.adoptedGaugeHistory = {100.0F, 76.0F};
    result.playedAtUnixMillis = 1'700'000'001'234LL;
    result.resultFingerprint = result_persistence::modernResultFingerprint(result);
    std::string diagnostic;
    require(result_persistence::validateModernChartResult(result, diagnostic),
            "fixture creates valid saved chart result: " + diagnostic);
  }
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  void stageResult(ReplayRepository &repository,
                   std::optional<ModernReplayFileAttachment> attachment = {}) const {
    require(repository.StageModernChartResult(result, std::nullopt, attachment).status ==
                ModernChartStageStatus::Staged,
            "real repository stages saved chart result");
  }
};

int noteCountInLane(const bms_parser::Chart &chart, int lane) {
  int count = 0;
  for (const auto *measure : chart.Measures) {
    for (const auto *timeline : measure->TimeLines) {
      if (timeline->Notes[lane]) ++count;
    }
  }
  return count;
}

void testResultOnlyFallbackAndLifetime() {
  Fixture fixture;
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  fixture.stageResult(repository);
  std::atomic_bool cancelled{false};
  auto prepared = chart_records::prepareChartResult(repository, fixture.record,
                                                     fixture.result.attemptId, cancelled);
  require(prepared.completion != nullptr, "saved result prepares: " + prepared.diagnostic);
  auto completion = prepared.completion;
  require(!completion->retryData && completion->view.chart &&
              completion->view.chart->Meta.BmsPath == fixture.record.meta.BmsPath &&
              completion->view.chart->Meta.Title == "Saved title" &&
              completion->view.chart->Meta.Artist == "Saved artist",
          "result-only recall resolves current location and restores saved display facts");
  require(completion->view.state.getScore() == 7 &&
              completion->view.state.maxCombo == 4 &&
              completion->view.state.getClearTypeRank() == kClearTypeHardClearRank &&
              completion->view.state.gaugeHistoryFor(GaugeType::Hard) ==
                  std::vector<float>({100.0F, 76.0F}),
          "saved score, combo, clear type and gauge presentation survive preparation");
  require(completion->view.result.attemptId == fixture.result.attemptId &&
              completion->view.result.playedAtUnixMillis == 1'700'000'001'234LL &&
              completion->view.result.score.provenance == fixture.result.score.provenance,
          "the exact saved attempt retains its provenance and played time");
  std::filesystem::remove(fixture.record.meta.BmsPath);
  prepared = {};
  fixture.record = {};
  completion->view.state.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::None);
  require(completion->view.chart->Meta.TotalNotes == 5 &&
              noteCountInLane(*completion->view.chart, 0) == 4,
          "completion owns the chart referenced by its rhythm state after input and file removal");
}

void testPreparedReplayReuseAndOwnership() {
  Fixture fixture(true);
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  std::string diagnostic;
  auto setup = replay::captureLocalReplaySetup(
      {.chart = {.md5 = fixture.result.score.chartMd5,
                 .sha256 = fixture.result.score.chartSha256,
                 .keyMode = 7},
       .longNoteMode = 1}, fixture.result.score.provenance, diagnostic);
  require(setup.has_value(), "real MIRROR replay setup captures: " + diagnostic);
  replay::ReplayChartDocument document{
      .playback = {.setup = *setup},
      .timeBounds = {.completionSongTimeMicros = 5'000'000},
  };
  replay::BeatorajaReplayCodec codec;
  const auto bytes = codec.encodeChart(document, fixture.result.playedAtUnixMillis,
                                        diagnostic);
  require(bytes.has_value(), "real replay codec encodes: " + diagnostic);
  const auto stem = replay::chartStem(fixture.result.score.chartSha256, 1, false,
                                      diagnostic);
  require(stem.has_value(), "chart replay path derives");
  const auto reserved = repository.ReserveModernReplayPath(
      fixture.result.attemptId, *stem, fixture.result.playedAtUnixMillis);
  require(reserved.reservation.has_value(), "repository reserves replay attachment");
  replay::ReplayFileStore store(repository.GetResolvedProfileRoot());
  const auto fileReservation = store.reserve(reserved.reservation->identity, *bytes,
                                               fixture.result.attemptId);
  require(fileReservation.reservation.has_value(), "file store reserves replay bytes");
  const auto installed = store.install(*fileReservation.reservation, *bytes,
      [&](const replay::ReplayFileOwnershipReceipt &receipt, std::string &) {
        return repository.RecordModernReplayInstallIntent(*reserved.reservation, receipt)
                   .status == ModernReplayOwnershipRecordStatus::Recorded;
      });
  require(installed.file.has_value(), "file store installs verified replay bytes");
  fixture.stageResult(repository, ModernReplayFileAttachment{
      .identity = reserved.reservation->identity,
      .metadata = installed.file->metadata,
  });
  std::atomic_bool cancelled{false};
  auto prepared = chart_records::prepareChartResult(repository, fixture.record,
                                                     fixture.result.attemptId, cancelled);
  require(prepared.completion && prepared.completion->retryData,
          "verified replay is retained by the prepared result");
  auto completion = prepared.completion;
  require(noteCountInLane(*completion->view.chart, 0) == 1 &&
              noteCountInLane(*completion->view.chart, 6) == 4 &&
              completion->retryData->playOption == "MIRROR",
          "result recall reuses the prepared MIRROR chart instead of reparsing original lanes");
  const auto replayPath = repository.GetResolvedProfileRoot() /
                         installed.file->metadata.relativePath;
  std::filesystem::remove(replayPath);
  prepared = {};
  require(completion->retryData->provenance == fixture.result.score.provenance &&
              completion->view.chart->Meta.TotalNotes == 5,
          "replay and chart ownership survives worker outcome destruction and replay deletion");
  auto fallback = chart_records::prepareChartResult(repository, fixture.record,
                                                     fixture.result.attemptId, cancelled);
  require(fallback.completion && !fallback.completion->retryData &&
              fallback.completion->view.state.getScore() == 7,
          "a missing optional replay falls back to the durable saved result");
}

void testFailuresAndCancellation() {
  Fixture fixture;
  ReplayRepository repository(fixture.directory / "replay.db");
  require(repository.EnsureSchema(), "repository schema initializes");
  std::atomic_bool cancelled{false};
  auto missing = chart_records::prepareChartResult(repository, fixture.record,
                                                    fixture.result.attemptId, cancelled);
  require(!missing.completion && !missing.diagnostic.empty(),
          "missing exact attempts return diagnostics without publishing a result");
  fixture.stageResult(repository);
  std::filesystem::remove(fixture.record.meta.BmsPath);
  auto deleted = chart_records::prepareChartResult(repository, fixture.record,
                                                    fixture.result.attemptId, cancelled);
  require(!deleted.completion && !deleted.diagnostic.empty(),
          "a missing current chart cannot publish a partially reconstructed result");
  cancelled.store(true);
  auto stopped = chart_records::prepareChartResult(repository, fixture.record,
                                                    fixture.result.attemptId, cancelled);
  require(!stopped.completion && stopped.diagnostic.empty(),
          "cancelled work publishes neither result nor failure notification");
}
#endif
} // namespace

int main() {
  try {
#if HAS_CHART_RECORD_ACTIONS
    testResultOnlyFallbackAndLifetime();
    testPreparedReplayReuseAndOwnership();
    testFailuresAndCancellation();
#else
    require(false, "shared chart record actions are not implemented");
#endif
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "chart record actions tests passed\n";
}
