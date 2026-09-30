#include "scene/ResultScene.h"
#include "scene/RemoteResultRecallController.h"
#include "BeatorajaScoreMetrics.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

void testRemoteSourceOwnsOnlyValidatedRemoteData() {
  static_assert(std::is_constructible_v<ResultScene, ApplicationContext &,
                                        ResultRemoteOptions>);
  static_assert(!HasReplayData<ResultRemoteOptions>);
  static_assert(!HasRhythmState<ResultRemoteOptions>);
  static_assert(!HasScoreProvenance<ResultRemoteOptions>);
  static_assert(HasPreviousScenePointer<ResultRemoteOptions>);
  static_assert(!HasReplayData<RemoteResultSource>);
  static_assert(!HasRhythmState<RemoteResultSource>);
  static_assert(!HasScoreProvenance<RemoteResultSource>);
  static_assert(HasPreviousScenePointer<RemoteResultSource>);

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

  // The retained owner is navigation context; the remote result still has no
  // local chart, replay, provenance, or persistence authority.
  auto *owner = reinterpret_cast<Scene *>(std::uintptr_t{0x1234});
  const auto retained = makeResultRemoteSource({.score = score,
      .rankingQuery = makeRemoteResultRankingQuery(score),
      .providerId = std::string(ir::kTachiProviderId),
      .serverOrigin = std::string(ir::kDefaultTachiServerOrigin),
      .returnScene = owner});
  require(retained.returnScene == owner, "remote result preserves its originating Records scene");

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

ResultRecordSummary
remoteRecordSummary(std::string origin = "https://ir.example.test:8443") {
  return makeRemoteResultRecord(ir::kTachiProviderId, origin, remoteScore());
}

struct RemoteRecallFixture {
  ResultRecordSummary selected = remoteRecordSummary();
  bool recordsModalOpen = true;
  bool resultSceneOpen = false;
  bool retainedRecordsScene = false;
  int selectionChecks = 0;
  int lookupCalls = 0;
  int transitionCalls = 0;
  int reloadCalls = 0;
  int failCalls = 0;
  int staleAfterCheck = -1;
  ir::IrRemoteScoreLookupOutcome lookupOutcome{
      .status = ir::IrRemoteScoreLookupOutcome::Status::Loaded,
      .score = remoteScore(),
  };
  std::optional<IrRemoteRecordId> lookedUpIdentity;
  std::optional<ResultRemoteOptions> transitionedRemote;
  std::string failure;

  [[nodiscard]] RemoteResultRecallRequest request() const {
    return {
        .identity = std::get<IrRemoteRecordId>(selected.identity),
        .selectedStableKey = selected.stableKey(),
    };
  }

  [[nodiscard]] RemoteResultRecallCallbacks callbacks() {
    return {
        .selectionStillMatches =
            [this](const auto &request) {
              ++selectionChecks;
              if (staleAfterCheck >= 0 && selectionChecks > staleAfterCheck) {
                return false;
              }
              return remoteResultRecallSelectionMatches(selected, request);
            },
        .loadExact =
            [this](const IrRemoteRecordId &identity) {
              ++lookupCalls;
              lookedUpIdentity = identity;
              return lookupOutcome;
            },
        .transition =
            [this](ResultRemoteOptions remote, bool retainCurrentScene) {
              ++transitionCalls;
              transitionedRemote = std::move(remote);
              retainedRecordsScene = retainCurrentScene && recordsModalOpen;
              recordsModalOpen = false;
              resultSceneOpen = true;
              return true;
            },
        .failAndReload =
            [this](std::string diagnostic) {
              ++failCalls;
              ++reloadCalls;
              failure = std::move(diagnostic);
              recordsModalOpen = true;
              resultSceneOpen = false;
            },
    };
  }

  void pressBack() {
    require(resultSceneOpen, "Back starts from the recalled result scene");
    const bool returned = executeRemoteResultBack([this]() {
      resultSceneOpen = false;
      if (retainedRecordsScene) {
        recordsModalOpen = true;
      }
    });
    require(returned, "remote Back executes the retained Records return");
  }
};

void testRemoteRecordViewResultActionIsPresentedEnabled() {
  const auto remote = remoteRecordSummary();
  const ResultRecordRecallActionState available =
      resultRecordRecallActionState(remote, true, false);
  require(available.visible && available.enabled,
          "selected remote View Result is visibly enabled");

  const ResultRecordRecallActionState busy =
      resultRecordRecallActionState(remote, true, true);
  require(busy.visible && !busy.enabled,
          "an active modal operation disables but does not hide View Result");
  require(!resultRecordRecallActionState(std::nullopt, true, false).visible &&
              !resultRecordRecallActionState(remote, false, false).visible,
          "View Result is absent without a tagged selection or in another "
          "modal mode");
}

void testRemoteRecallExecutesExactLookupAndRetainedBackLifecycle() {
  RemoteRecallFixture fixture;
  const auto request = fixture.request();
  auto callbacks = fixture.callbacks();
  const RemoteResultRecallStatus status =
      executeRemoteResultRecall(request, callbacks);

  require(status == RemoteResultRecallStatus::Transitioned &&
              fixture.lookupCalls == 1 && fixture.transitionCalls == 1 &&
              fixture.failCalls == 0 && fixture.lookedUpIdentity.has_value(),
          "valid remote recall performs one lookup and one transition");
  require(fixture.lookedUpIdentity->providerId == request.identity.providerId &&
              fixture.lookedUpIdentity->serverOrigin ==
                  "https://ir.example.test:8443" &&
              fixture.lookedUpIdentity->remoteScoreId ==
                  request.identity.remoteScoreId,
          "recall lookup uses exact provider, normalized configured origin, "
          "and remote ID");
  require(fixture.transitionedRemote.has_value() &&
              fixture.transitionedRemote->providerId ==
                  request.identity.providerId &&
              fixture.transitionedRemote->serverOrigin ==
                  request.identity.serverOrigin &&
              fixture.transitionedRemote->score.remoteScoreId ==
                  request.identity.remoteScoreId &&
              fixture.retainedRecordsScene && fixture.resultSceneOpen &&
              !fixture.recordsModalOpen,
          "transition retains the live Records scene behind the remote result");

  fixture.pressBack();
  require(fixture.recordsModalOpen && !fixture.resultSceneOpen &&
              remoteResultRecallSelectionMatches(fixture.selected, request),
          "Back returns to the still-live Records modal and selection");
}

void testRemoteRecallFailsClosedForConcurrentDeletion() {
  RemoteRecallFixture fixture;
  fixture.lookupOutcome = {
      .status = ir::IrRemoteScoreLookupOutcome::Status::NotFound,
  };
  auto callbacks = fixture.callbacks();
  const RemoteResultRecallStatus status =
      executeRemoteResultRecall(fixture.request(), callbacks);

  require(status == RemoteResultRecallStatus::NotFound &&
              fixture.lookupCalls == 1 && fixture.transitionCalls == 0 &&
              fixture.reloadCalls == 1 && fixture.recordsModalOpen &&
              fixture.failure.find("no longer available") != std::string::npos,
          "a concurrently deleted score reloads the still-open Records modal");
}

void testRemoteRecallRejectsStaleSelectionBeforeAndAfterLookup() {
  RemoteRecallFixture staleBefore;
  staleBefore.staleAfterCheck = 0;
  auto staleBeforeCallbacks = staleBefore.callbacks();
  const RemoteResultRecallStatus beforeStatus =
      executeRemoteResultRecall(staleBefore.request(), staleBeforeCallbacks);
  require(beforeStatus == RemoteResultRecallStatus::StaleSelection &&
              staleBefore.lookupCalls == 0 &&
              staleBefore.transitionCalls == 0 && staleBefore.reloadCalls == 1,
          "stale selection before lookup cannot read or transition");

  RemoteRecallFixture staleAfter;
  staleAfter.staleAfterCheck = 1;
  auto staleAfterCallbacks = staleAfter.callbacks();
  const RemoteResultRecallStatus afterStatus =
      executeRemoteResultRecall(staleAfter.request(), staleAfterCallbacks);
  require(afterStatus == RemoteResultRecallStatus::StaleSelection &&
              staleAfter.lookupCalls == 1 && staleAfter.selectionChecks == 2 &&
              staleAfter.transitionCalls == 0 && staleAfter.reloadCalls == 1,
          "selection changed during lookup cannot transition");
}

void testResultTableContextRestoresRetryLaunchOptions() {
  const ResultTableContext table{
      .tableName = "Insane BMS Table",
      .tableLevel = "★12",
  };
  StartOptions retry;
  applyResultTableContext(retry, table);
  require(retry.tableName == table.tableName &&
              retry.tableLevel == table.tableLevel,
          "result retries retain their difficulty-table context");
}

void testResultTimingStatisticsUseBeatorajaConventions() {
  ReplayData replay;
  require(!beatorajaResultTimingStatistics(nullptr, 1, nullptr) &&
              !beatorajaResultTimingStatistics(&replay, 0, nullptr),
          "timing statistics require a replay and a positive note count");
  const auto empty = beatorajaResultTimingStatistics(&replay, 1, nullptr);
  require(empty && !empty->hasTimingSamples && empty->timingSampleCount == 0 &&
              empty->averageJudgeMicros == 1'000'000,
          "unplayed notes retain the one-second judge penalty without samples");
  replay.events = {
      {.judgement = PGreat, .diffMicros = 1'999},
      {.judgement = Great, .diffMicros = -2'999},
      {.action = ReplayEventAction::MultiBad, .judgement = Bad, .diffMicros = 4'000},
      {.action = ReplayEventAction::Release, .judgement = Good, .diffMicros = -5'000},
      {.judgement = Poor, .diffMicros = 6'000},
      {.judgement = Kpoor, .diffMicros = 1'000},
      {.action = ReplayEventAction::Miss, .judgement = Poor, .diffMicros = 7'000},
      {.judgement = None, .diffMicros = 8'000},
      {.judgement = Great, .diffMicros = 150'001},
  };
  const auto timing = beatorajaResultTimingStatistics(&replay, 6, nullptr);
  require(timing && timing->hasTimingSamples && timing->timingSampleCount == 5 &&
              timing->distribution.size() == 301 &&
              timing->distribution[149] == 1 && timing->distribution[152] == 1 &&
              timing->distribution[146] == 1 && timing->distribution[155] == 1 &&
              timing->distribution[144] == 1,
          "result timing reverses input sign and truncates to millisecond bins");
  require(std::abs(timing->averageMillis + 0.8) < 0.00001 &&
              std::abs(timing->standardDeviationMillis - std::sqrt(15.76)) <
                  0.00001 && timing->averageJudgeMicros == 193'999,
          "result timing separates histogram samples from judged-note penalties");
}

void testResultTimingStatisticsCountLongNoteResultsOnce() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  chart.Measures.push_back(measure);
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  measure->TimeLines = {headTimeline, tailTimeline};
  headTimeline->Timing = 100'000;
  tailTimeline->Timing = 200'000;
  auto *head = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  auto *tail = new bms_parser::LongNote(1, bms_parser::LongNoteType::LongNote);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(0, head);
  tailTimeline->SetNote(0, tail);
  ReplayData replay;
  replay.events = {
      {.lane = 0, .noteTimeMicros = 100'000, .judgement = PGreat, .diffMicros = 1'000},
      {.action = ReplayEventAction::Release, .lane = 0, .noteTimeMicros = 200'000,
       .judgement = Great, .diffMicros = 2'000},
  };
  for (const auto judgement : {PGreat, Bad}) {
    replay.events[0].judgement = judgement;
    const auto classic = beatorajaResultTimingStatistics(&replay, 1, &chart);
    require(classic && classic->timingSampleCount == 1 &&
                classic->distribution[148] == 1 && classic->averageJudgeMicros == 2'000,
            "classic long notes use their tail result instead of counting both ends");
  }
  replay.events[0].judgement = PGreat;
  head->SetType(bms_parser::LongNoteType::ChargeNote);
  const auto charge = beatorajaResultTimingStatistics(&replay, 2, &chart);
  require(charge && charge->timingSampleCount == 2 &&
              charge->distribution[149] == 1 && charge->distribution[148] == 1 &&
              charge->averageJudgeMicros == 1'500,
          "charge notes retain independent head and tail timing results");
  head->SetType(bms_parser::LongNoteType::LongNote);
  replay.events[0].judgement = Bad;
  replay.events.resize(1);
  const auto missedHead = beatorajaResultTimingStatistics(&replay, 1, &chart);
  require(missedHead && missedHead->timingSampleCount == 1 &&
              missedHead->averageJudgeMicros == 1'000,
          "a classic bad head remains a result when no tail result exists");
}

} // namespace

int main() {
  testRemoteSourceOwnsOnlyValidatedRemoteData();
  testRemoteRankingDependenciesFailClosed();
  testRemoteActionMatrixIsReadOnly();
  testRemoteRecordViewResultActionIsPresentedEnabled();
  testRemoteRecallExecutesExactLookupAndRetainedBackLifecycle();
  testRemoteRecallFailsClosedForConcurrentDeletion();
  testRemoteRecallRejectsStaleSelectionBeforeAndAfterLookup();
  testResultTableContextRestoresRetryLaunchOptions();
  testResultTimingStatisticsUseBeatorajaConventions();
  testResultTimingStatisticsCountLongNoteResultsOnce();
  std::cout << "remote result scene tests passed\n";
  return 0;
}
