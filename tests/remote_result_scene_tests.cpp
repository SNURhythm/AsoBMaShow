#include "scene/ResultScene.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

template <typename T>
concept HasReplayData = requires(T value) { value.replay; };

template <typename T>
concept HasRhythmState = requires(T value) { value.resultState; };

template <typename T>
concept HasScoreProvenance = requires(T value) { value.attemptProvenance; };

template <typename T>
concept HasPreviousScenePointer = requires(T value) { value.returnScene; };

ir::IrRemoteScore remoteScore(std::string game = "bms-7k") {
  return {
      .remoteUserId = 17,
      .game = std::move(game),
      .remoteScoreId = "remote-score-17",
      .remoteChartId = "remote-chart-17",
      .chartMd5 = std::string(32, 'b'),
      .chartSha256 = std::string(64, 'a'),
      .title = "Remote Result",
      .artist = "Remote Artist",
      .service = "Bokutachi",
      .difficulty = "ANOTHER",
      .noteCount = 1000,
      .score = 1800,
      .lampRank = kClearTypeHardClearRank,
      .timeAchievedUnixMillis = 1'721'377'845'000LL,
      .timeAddedUnixMillis = 1'721'377'846'000LL,
      .judgements =
          {.pGreat = 850, .great = 50, .good = 40, .bad = 30, .poor = 30},
      .maxCombo = 700,
      .badPoints = 60,
      .finalGauge = 78.0F,
      .gaugeHistory = {20.0F, 48.0F, std::nullopt, 78.0F},
      .random = "RANDOM",
      .gauge = "HARD",
  };
}

std::string readSource(const std::filesystem::path &relative) {
  const auto path = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / relative;
  std::ifstream input(path);
  require(input.good(), "remote ResultScene contract source is readable");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void requireContains(const std::string &source, const std::string &token,
                     const char *message) {
  require(source.find(token) != std::string::npos, message);
}

void testRemoteSourceOwnsOnlyValidatedRemoteData() {
  static_assert(std::is_constructible_v<ResultScene, ApplicationContext &,
                                        ResultRemoteOptions>);
  static_assert(!HasReplayData<ResultRemoteOptions>);
  static_assert(!HasRhythmState<ResultRemoteOptions>);
  static_assert(!HasScoreProvenance<ResultRemoteOptions>);
  static_assert(!HasPreviousScenePointer<ResultRemoteOptions>);
  static_assert(!HasReplayData<RemoteResultSource>);
  static_assert(!HasRhythmState<RemoteResultSource>);
  static_assert(!HasScoreProvenance<RemoteResultSource>);
  static_assert(!HasPreviousScenePointer<RemoteResultSource>);

  auto score = remoteScore("bms-14k");
  const auto ranking = makeRemoteResultRankingQuery(score);
  require(ranking.has_value(),
          "validated bms-14k score with SHA builds an exact ranking query");
  require(ranking->keyMode == 14 && ranking->chartSha256 == score.chartSha256 &&
              ranking->chartMd5 == score.chartMd5 &&
              ranking->totalNotes == score.noteCount,
          "remote ranking query preserves exact stored game and hashes");

  const RemoteResultSource source =
      makeResultRemoteSource({.score = score,
                              .rankingQuery = ranking,
                              .providerId = "tachi",
                              .serverOrigin = "https://boku.tachi.ac"});
  require(source.score.remoteScoreId == score.remoteScoreId &&
              source.rankingQuery == ranking && source.providerId == "tachi" &&
              source.serverOrigin == "https://boku.tachi.ac",
          "remote source retains the validated origin-scoped identity");
  require(source.presentation.readOnlyIrUploaded &&
              source.presentation.title == score.title &&
              source.presentation.gaugeSeries.size() == 1,
          "remote source owns its immutable presentation model");

  const RemoteResultSource customOrigin =
      makeResultRemoteSource({.score = score,
                              .rankingQuery = ranking,
                              .providerId = "tachi",
                              .serverOrigin = "https://ir.example.test:8443"});
  require(customOrigin.serverOrigin == "https://ir.example.test:8443",
          "remote source accepts any normalized configured Tachi origin");

  bool rejected = false;
  try {
    auto wrong = *ranking;
    wrong.keyMode = 7;
    (void)makeResultRemoteSource({.score = score,
                                  .rankingQuery = wrong,
                                  .providerId = "tachi",
                                  .serverOrigin = "https://boku.tachi.ac"});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "remote source rejects a ranking query for another game");

  rejected = false;
  try {
    (void)makeResultRemoteSource(
        {.score = score,
         .rankingQuery = ranking,
         .providerId = "tachi",
         .serverOrigin = "https://ir.example.test:8443/"});
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected,
          "remote source rejects a non-normalized origin-scoped identity");
}

void testRemoteRankingDependenciesFailClosed() {
  auto score = remoteScore();
  score.chartSha256.clear();
  require(!makeRemoteResultRankingQuery(score).has_value(),
          "MD5-only remote score omits rankings when normal SHA dependency is "
          "absent");

  score = remoteScore("bms-9k");
  require(!makeRemoteResultRankingQuery(score).has_value(),
          "unsupported stored game never guesses a ranking key mode");

  score = remoteScore();
  score.chartMd5 = "not-a-digest";
  require(!makeRemoteResultRankingQuery(score).has_value(),
          "malformed optional stored MD5 fails closed");
}

void testRemoteActionMatrixIsReadOnly() {
  const ResultSceneActionAvailability withRankings =
      remoteResultSceneActions(true);
  require(withRankings.back && withRankings.rankings &&
              withRankings.exportPhoto && withRankings.readOnlyIrUploaded,
          "remote result exposes Back, exact Rankings, photo, and uploaded IR");
  require(!withRankings.persistence && !withRankings.retry &&
              !withRankings.retrySame && !withRankings.replay &&
              !withRankings.practice && !withRankings.course &&
              !withRankings.irSubmit && !withRankings.irRetry,
          "remote result exposes no mutating or replay-derived actions");

  const ResultSceneActionAvailability withoutRankings =
      remoteResultSceneActions(false);
  require(withoutRankings.back && !withoutRankings.rankings &&
              withoutRankings.exportPhoto && withoutRankings.readOnlyIrUploaded,
          "remote rankings disappear without exact query dependencies");
}

void testLifecycleAndLocalRegressionContracts() {
  const std::string header = readSource("src/scene/ResultScene.h");
  const std::string result = readSource("src/scene/ResultScene.cpp");
  const std::string menu = readSource("src/scene/MainMenuScene.cpp");
  const std::string combined = header + result;

  requireContains(combined,
                  "std::variant<LocalResultSource, RemoteResultSource>",
                  "ResultScene stores a real local/remote source variant");
  requireContains(combined, "makeRemoteResultPresentation",
                  "remote scene renders the shared partial presentation");
  requireContains(
      result, "result_gauge_history::graphFor",
      "remote and local gauge series share nullable graph rendering");
  requireContains(result,
                  "remoteResultSceneActions(remote->rankingQuery.has_value())",
                  "remote UI is built from the tested read-only action policy");
  requireContains(result, "addRemoteIrStatus();",
                  "remote init installs read-only uploaded IR state");
  requireContains(result, "addRemoteButtons();",
                  "remote init installs only the remote action row");
  requireContains(menu, "LoadIrRemoteScore",
                  "Records re-loads the exact remote row before transition");
  requireContains(menu, "selectedResultRecordStableKey",
                  "remote recall reconciles against stable selection identity");
  requireContains(menu, "std::make_unique<ResultScene>",
                  "validated remote recall transitions to ResultScene");
  requireContains(menu, "changeScene(std::move(next), true)",
                  "remote transition retains the live Records scene");
  requireContains(
      menu, "reloadReplayRecordModels(true);",
      "remote recall failure reloads while preserving Records state");

  for (const char *localToken :
       {"Retry Same", "Replay", "Practice Section",
        "addResultPersistenceStatus();", "addIrResultStatus();",
        "addCourseButtons();", "showSavedCourseStage();",
        "showCourseResult();"}) {
    requireContains(
        result, localToken,
        "local/course ResultScene action regression contract remains");
  }
}

} // namespace

int main() {
  testRemoteSourceOwnsOnlyValidatedRemoteData();
  testRemoteRankingDependenciesFailClosed();
  testRemoteActionMatrixIsReadOnly();
  testLifecycleAndLocalRegressionContracts();
  std::cout << "remote result scene tests passed\n";
  return 0;
}
