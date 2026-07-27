#include "PlayOptionUtils.h"
#include "scene/ResultScene.h"
#include "scene/ResultCoursePersistence.h"
#include "scene/RemoteResultRecallController.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
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

void testRecalledResultExcludesItselfFromPreviousBest() {
  result_persistence::PersistedChartResult stored;
  stored.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  ResultPersistenceOptions persistence;
  persistence.result =
      std::make_shared<const result_persistence::PersistedChartResult>(stored);
  persistence.previousBestBeforeCreatedAt = "2026-07-25 01:02:03";

  const auto current = result_scene_detail::previousBestQueryFor(
      persistence, false, nullptr);
  require(current.excludeAttemptId == stored.attemptId &&
              !current.beforeCreatedAt.has_value(),
          "recalled result excludes its exact stored attempt from previous "
          "best");

  stored.attemptId.reset();
  persistence.result =
      std::make_shared<const result_persistence::PersistedChartResult>(stored);
  const auto legacy = result_scene_detail::previousBestQueryFor(
      persistence, false, nullptr);
  require(!legacy.excludeAttemptId.has_value() &&
              legacy.beforeCreatedAt ==
                  persistence.previousBestBeforeCreatedAt,
          "legacy recalled result uses its selected replay timestamp as the "
          "previous-best cutoff");

  persistence = {};
  const ReplayResultContext watched{
      .resultId = 73,
      .attemptId = "123e4567-e89b-42d3-a456-426614174073",
      .createdAt = "2026-07-25 01:02:03",
  };
  const auto watchedReplay = result_scene_detail::previousBestQueryFor(
      persistence, true, &watched);
  require(watchedReplay.excludeAttemptId == watched.attemptId &&
              watchedReplay.beforeCreatedAt == watched.createdAt,
          "watched replay keeps both exact attempt identity and its "
          "historical previous-best cutoff");
  require(result_scene_detail::resultIdForPractice(persistence, &watched) ==
              watched.resultId,
          "watched replay keeps its repository result identity for practice");
}

void testCoursePersistenceAttemptParticipatesInRetryPolicy() {
  constexpr std::string_view attemptId =
      "123e4567-e89b-42d3-a456-426614174099";
  result_persistence::CompletedCourseAttempt course;
  course.result.attemptId = std::string(attemptId);
  ResultPersistenceOptions persistence;
  require(result_scene_detail::persistCourseAttempt(
              persistence, std::move(course),
              [](const result_persistence::CompletedCourseAttempt &) {
                return result_persistence::SaveOutcome{
                    .state = result_persistence::SaveState::Unstaged,
                    .userMessage = "Course replay was not saved.",
                };
              }) &&
              persistence.outcome.state ==
                  result_persistence::SaveState::Unstaged,
          "failed first course save retains its retryable attempt and outcome");

  require(result_scene_detail::hasPersistenceAttempt(persistence),
          "completed course attempt is available to persistence retry");
  require(result_scene_detail::persistenceAttemptId(persistence) ==
              attemptId,
          "course retry diagnostics use the retained course attempt ID");
  CoursePlaySession session;
  require(!result_scene_detail::applyCoursePersistenceReceipt(
              persistence.courseAttempt, persistence.outcome, session) &&
              !session.courseReplaySaved &&
              session.savedCourseReplayId == 0 &&
              session.courseReplayPlaybackData == nullptr,
          "failed course save leaves the session unsaved while awaiting a "
          "decision");

  const ResultPersistenceRetryCallbacks callbacks{
      .persistChart =
          [](const result_persistence::CompletedChartAttempt &,
             std::span<const ir::IrOutboxDraft>) {
            return result_persistence::SaveOutcome{
                .state = result_persistence::SaveState::InvalidAttempt};
          },
      .persistCourse =
          [](const result_persistence::CompletedCourseAttempt &attempt) {
            return result_persistence::SaveOutcome{
                .state = result_persistence::SaveState::Saved,
                .receipt = result_persistence::StageReceipt{
                    .attemptId = *attempt.result.attemptId,
                    .resultId = 91,
                    .createdAt = "2026-07-26 01:02:03",
                }};
          },
  };
  require(result_scene_detail::retryPersistenceAttempt(
              persistence, {}, callbacks) &&
              persistence.outcome.state ==
                  result_persistence::SaveState::Saved,
          "course retry executes the course persistence branch");

  require(result_scene_detail::applyCoursePersistenceReceipt(
              persistence.courseAttempt, persistence.outcome, session) &&
              session.courseReplaySaved &&
              session.savedCourseReplayId == 91 &&
              session.courseReplayPlaybackData != nullptr,
          "saved course retry applies its receipt and replay to the session");

  persistence.attempt =
      std::make_shared<const result_persistence::CompletedChartAttempt>();
  require(!result_scene_detail::hasPersistenceAttempt(persistence),
          "ambiguous chart and course attempts fail closed");
  require(!result_scene_detail::retryPersistenceAttempt(
              persistence, {}, callbacks),
          "ambiguous persistence attempts cannot execute either branch");

  result_persistence::CompletedChartAttempt chart;
  chart.result.attemptId = "123e4567-e89b-42d3-a456-426614174098";
  ResultPersistenceOptions chartPersistence;
  chartPersistence.attempt =
      std::make_shared<const result_persistence::CompletedChartAttempt>(
          std::move(chart));
  const std::array automaticDrafts{ir::IrOutboxDraft{}};
  const ResultPersistenceRetryCallbacks chartCallbacks{
      .persistChart =
          [](const result_persistence::CompletedChartAttempt &attempt,
             std::span<const ir::IrOutboxDraft> drafts) {
            return result_persistence::SaveOutcome{
                .state = attempt.result.attemptId.has_value() &&
                                 drafts.size() == 1
                             ? result_persistence::SaveState::PendingScore
                             : result_persistence::SaveState::InvalidAttempt};
          },
      .persistCourse =
          [](const result_persistence::CompletedCourseAttempt &) {
            return result_persistence::SaveOutcome{
                .state = result_persistence::SaveState::InvalidAttempt};
          },
  };
  require(result_scene_detail::retryPersistenceAttempt(
              chartPersistence, automaticDrafts, chartCallbacks) &&
              chartPersistence.outcome.state ==
                  result_persistence::SaveState::PendingScore,
          "chart retry reuses the retained chart attempt and automatic drafts");
}

void testContinueWithoutSavingRequiresSuccessfulCleanup() {
  bool cleanupCalled = false;
  require(!result_scene_detail::cleanupAllowsContinueWithoutSaving(
              true, true,
              [&] {
                cleanupCalled = true;
                return false;
              }) &&
              cleanupCalled,
          "failed undurable replay cleanup keeps Continue blocked for retry");

  cleanupCalled = false;
  require(result_scene_detail::cleanupAllowsContinueWithoutSaving(
              true, true,
              [&] {
                cleanupCalled = true;
                return true;
              }) &&
              cleanupCalled,
          "successful undurable replay cleanup allows Continue");

  cleanupCalled = false;
  require(result_scene_detail::cleanupAllowsContinueWithoutSaving(
              false, true,
              [&] {
                cleanupCalled = true;
                return false;
              }) &&
              !cleanupCalled,
          "durable results need no cleanup before Continue");

  cleanupCalled = false;
  require(result_scene_detail::cleanupAllowsContinueWithoutSaving(
              true, false,
              [&] {
                cleanupCalled = true;
                return false;
              }) &&
              cleanupCalled,
          "a permanent invalid attempt cannot trap the user when cleanup "
          "must be deferred to startup recovery");
}

void testCourseReplayActionAcceptsRawAndLegacyData() {
  CoursePlaySession session;
  require(!result_scene_detail::courseReplayActionAvailable(session),
          "empty course sessions hide replay");

  session.courseReplayPlaybackData =
      std::make_shared<replay::CourseReplayPlaybackData>();
  session.courseReplayPlaybackData->stages.emplace_back();
  require(result_scene_detail::courseReplayActionAvailable(session),
          "raw course replay data exposes replay");

  session.courseReplayPlaybackData.reset();
  session.courseReplayData = std::make_shared<JudgedCoursePlaybackData>();
  session.courseReplayData->stages.emplace_back();
  require(result_scene_detail::courseReplayActionAvailable(session),
          "legacy course replay data still exposes replay");
}

void testCourseReplayCaptureDoesNotFillFailedStageHole() {
  CoursePlaySession session;
  session.entries.resize(2);

  RhythmState stageState(nullptr, false);
  session.currentIndex = 0;
  session.recordResult(session.entries[0].meta, stageState);
  session.recordRestMicrosAfterCurrentStage(1'000'000);

  session.currentIndex = 1;
  session.recordResult(session.entries[1].meta, stageState);
  session.recordReplayPlaybackStage(replay::ReplayPlaybackData{});
  session.recordRestMicrosAfterCurrentStage(0);

  require(session.completedResults.size() == 2 &&
              session.recordedReplayPlayback.stages.empty(),
          "a later successful capture cannot fill an earlier failed course "
          "stage with a default replay");
}

void testCoursePersistenceFactsUseAuthoritativeEntrySnapshot() {
  CoursePlaySession session;
  session.entries.resize(3);
  session.entries[0].meta.TotalNotes = 40;
  session.entries[0].meta.PlayLength = 4'000'000;
  session.entries[1].meta.TotalNotes = 50;
  session.entries[1].meta.PlayLength = 5'000'000;
  session.entries[2].meta.TotalNotes = 60;
  session.entries[2].meta.PlayLength = 6'000'000;

  std::vector<bms_parser::ChartMeta> authoritative(3);
  authoritative[0].TotalNotes = 45;
  authoritative[0].PlayLength = 4'500'000;
  authoritative[0].RandomSeed = 17;
  authoritative[0].RandomPrng = "mt19937";
  authoritative[0].RandomValues = {2, 1};
  authoritative[1].TotalNotes = 55;
  authoritative[1].PlayLength = 5'500'000;
  authoritative[2].TotalNotes = 65;
  authoritative[2].PlayLength = 6'500'000;
  require(session.installAuthoritativeEntryMetas(authoritative),
          "a complete course entry snapshot installs atomically");

  bms_parser::ChartMeta played = authoritative[0];
  played.TotalNotes = 999;
  played.PlayLength = 999'000'000;
  session.completedResults.emplace_back(played, RhythmState(nullptr, false));

  const auto facts =
      result_scene_detail::courseEntryFactsForPersistence(session);
  require(facts.size() == 3 && facts[0].totalNotes == 45 &&
              facts[0].playLengthMicros == 4'500'000 &&
              facts[1].totalNotes == 55 &&
              facts[2].playLengthMicros == 6'500'000,
          "course persistence ignores stale source and completed metadata once "
          "the full-course snapshot is authoritative");
  const auto *first = session.entryMeta(0);
  require(first != nullptr && first->RandomSeed == 17 &&
              first->RandomPrng == "mt19937" &&
              first->RandomValues == std::vector<int>({2, 1}),
          "the authoritative entry preserves the parser random branch for "
          "later gameplay parsing");
  const auto *persistedStageMeta =
      result_scene_detail::courseStageMetaForPersistence(session, 0);
  require(persistedStageMeta != nullptr &&
              persistedStageMeta->TotalNotes == 45 &&
              persistedStageMeta->PlayLength == 4'500'000,
          "course stage persistence captures identity and max facts from the "
          "same authoritative snapshot");

  auto incomplete = authoritative;
  incomplete.pop_back();
  require(!session.installAuthoritativeEntryMetas(std::move(incomplete)) &&
              session.entryMeta(2)->TotalNotes == 65,
          "an incomplete replacement cannot partially change course facts");
  auto completeReplacement = authoritative;
  completeReplacement[0].TotalNotes = 777;
  require(
      !session.installAuthoritativeEntryMetas(std::move(completeReplacement)) &&
          session.entryMeta(0)->TotalNotes == 45,
      "a frozen full-course snapshot cannot be replaced later");
}

void testEffectiveCourseFactsSaturateResultMetadata() {
  CoursePlaySession session;
  session.courseName = "Overflow Course";
  session.courseGroupName = "Bounds";
  session.entries.resize(2);
  session.entries[0].meta.TotalNotes = std::numeric_limits<int>::max();
  session.entries[0].meta.PlayLength = std::numeric_limits<std::int64_t>::max();
  session.entries[1].meta.TotalNotes = std::numeric_limits<int>::max();
  session.entries[1].meta.PlayLength = 1;

  const auto facts = result_scene_detail::effectiveCourseEntryFacts(session);
  const auto meta = result_scene_detail::courseResultMetaForSession(session);
  require(facts.size() == 2 &&
              facts[0].totalNotes == std::numeric_limits<int>::max() &&
              facts[0].playLengthMicros ==
                  std::numeric_limits<std::int64_t>::max() &&
              meta.TotalNotes == std::numeric_limits<int>::max() &&
              meta.PlayLength == std::numeric_limits<std::int64_t>::max(),
          "effective course facts saturate result metadata without signed "
          "overflow");
}

void testLegacyPartialCourseReplayPreservesEveryEntry() {
  CoursePlaySession source;
  source.entries.resize(3);
  source.entries[0].meta.Title = "Played";
  source.entries[1].meta.TotalNotes = 200;
  source.entries[2].meta.PlayLength = 30'000'000;
  auto authoritative = source.entryMetasSnapshot();
  authoritative[1].TotalNotes = 250;
  authoritative[2].PlayLength = 35'000'000;
  require(source.installAuthoritativeEntryMetas(std::move(authoritative)),
          "legacy replay source can retain authoritative future facts");
  JudgedCoursePlaybackData replay;
  replay.totalCharts = 3;
  replay.completedCharts = 1;
  replay.stages.emplace_back();
  replay.stages.front().replay.chartMeta.Title = "Played replay";

  const auto entries =
      result_scene_detail::legacyReplayEntriesForSession(source, replay);
  require(entries.size() == 3 && entries[0].meta.Title == "Played replay" &&
              entries[1].meta.TotalNotes == 250 &&
              entries[2].meta.PlayLength == 35'000'000,
          "replaying a migrated partial course retains authoritative unplayed "
          "entry facts");
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

void testLocalRegressionContractsRemainPresent() {
  const std::string header = readSource("src/scene/ResultScene.h");
  const std::string result = readSource("src/scene/ResultScene.cpp");
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

void testRawAndLegacyPlaybackAreClassifiedAsReplayResults() {
  JudgedPlaybackData judged;
  replay::ReplayPlaybackData raw;
  require(!result_scene_detail::isReplayResultSource(nullptr, nullptr, nullptr),
          "normal result is not classified as replay playback");
  require(!result_scene_detail::isReplayResultSource(&judged, &judged, nullptr),
          "a newly recorded presentation is not classified as replay playback");
  require(result_scene_detail::isReplayResultSource(nullptr, &judged, nullptr),
          "legacy judged playback is classified as a replay result");
  require(result_scene_detail::isReplayResultSource(nullptr, nullptr, &raw),
          "raw BRD playback is classified as a replay result");
}

void testSavedResultBuildsRetrySourceFromPersistedProvenance() {
  bms_parser::ChartMeta meta;
  meta.MD5 = std::string(32, 'a');
  meta.SHA256 = std::string(64, 'b');
  meta.KeyMode = 7;

  ScoreProvenance provenance = ScoreProvenance::Legacy();
  provenance.stages = {{.chartMd5 = meta.MD5,
                        .chartSha256 = meta.SHA256,
                        .chartRandomSeed = 41U,
                        .chartRandomPrng = "std::mt19937_64",
                        .chartRandomValues = {2}}};
  provenance.gaugeType = GaugeType::Hard;
  provenance.player1 = {.option = "MIRROR", .seed = 73};

  const auto retrySource = result_scene_detail::retrySourceForLocalResult(
      meta, provenance, nullptr, nullptr, nullptr);
  require(retrySource.has_value(),
          "a compact saved result receives a provenance retry source");
  const JudgedPlaybackData &retry = *retrySource;
  require(retry.setup.initialGaugeType == GaugeType::Hard &&
              retry.setup.playOption == "MIRROR" &&
              retry.setup.playOptionSeed == 73 &&
              retry.setup.randomSeed == 41U &&
              retry.setup.randomValues == std::vector<int>({2}),
          "View Result derives retry authority from persisted provenance when "
          "no replay projection is present");

  JudgedPlaybackData explicitRetry;
  explicitRetry.setup.playOption = "RANDOM";
  const auto selected = result_scene_detail::retrySourceForLocalResult(
      meta, provenance, nullptr, &explicitRetry, nullptr);
  require(selected.has_value() && selected->setup.playOption == "RANDOM",
          "an explicit live retry source remains authoritative");

  replay::ReplayPlaybackData rawPlayback;
  rawPlayback.setup = retry.setup;
  rawPlayback.setup.doublePlayOption = replay::DoublePlayOption::Flip;
  const auto rawRetry = result_scene_detail::retrySourceForLocalResult(
      meta, provenance, nullptr, nullptr, &rawPlayback);
  require(rawRetry.has_value() &&
              rawRetry->setup == rawPlayback.setup &&
              rawRetry->setup.doublePlayOption ==
                  replay::DoublePlayOption::Flip,
          "a raw replay result projects the complete BRD setup for retry, "
          "including schema-v4 DP FLIP");

  bms_parser::Chart retainedResultChart;
  require(!result_scene_detail::shouldReuseResultRetryChart(
              true, &retainedResultChart, false) &&
              result_scene_detail::shouldReuseResultRetryChart(
                  true, &retainedResultChart, true),
          "Retry Same reparses a compact recalled result whose retained chart "
          "does not contain the persisted lane pattern");
  require(!result_scene_detail::retrySourceProvidesTimingAnalytics(retry),
          "a provenance-only retry projection is not treated as judged "
          "timing analytics");
  JudgedPlaybackData judgedRetry = retry;
  judgedRetry.events.push_back({.action = ReplayEventAction::Press});
  require(result_scene_detail::retrySourceProvidesTimingAnalytics(judgedRetry),
          "a retry projection with real judged events remains an analytics "
          "source");
}

void testCourseReplayResultLabelRetainsStageSetupFlip() {
  bms_parser::ChartMeta meta;
  meta.KeyMode = 14;
  meta.IsDP = true;
  replay::ChartPlaybackSetup stageSetup{
      .playOption = "MIRROR",
      .playOptionSeed = 17,
      .playOption2 = "RANDOM",
      .playOption2Seed = 29,
      .doublePlayOption = replay::DoublePlayOption::Flip,
  };
  const auto retainedStage =
      play_options::formatReplayPlaybackModeDisplayLabel(meta, stageSetup);
  const auto sessionOptions = play_options::formatPlayModeDisplayLabel(
      meta, stageSetup.playOption, stageSetup.playOptionSeed,
      stageSetup.playOption2, stageSetup.playOption2Seed);

  const auto replayDisplay =
      result_scene_detail::selectCoursePlayModeDisplayLabel(
          retainedStage, sessionOptions, true);
  require(replayDisplay.mode == "FLIP + MIRROR #17 / RANDOM #29" &&
              replayDisplay.laneOrder.empty(),
          "a course replay result keeps the stage setup DP FLIP label");

  const auto liveDisplay =
      result_scene_detail::selectCoursePlayModeDisplayLabel(
          retainedStage, sessionOptions, false);
  require(liveDisplay.mode == "MIRROR #17 / RANDOM #29" &&
              liveDisplay.laneOrder == sessionOptions.laneOrder,
          "a live course result keeps the session play-option label");

  const auto defaultLiveDisplay =
      result_scene_detail::selectCoursePlayModeDisplayLabel(
          retainedStage, {}, false);
  require(defaultLiveDisplay.mode == "COURSE" &&
              defaultLiveDisplay.laneOrder.empty(),
          "a live course without an explicit option keeps the COURSE fallback");
}

} // namespace

int main() {
  testRemoteSourceOwnsOnlyValidatedRemoteData();
  testRemoteRankingDependenciesFailClosed();
  testRemoteActionMatrixIsReadOnly();
  testRecalledResultExcludesItselfFromPreviousBest();
  testCoursePersistenceAttemptParticipatesInRetryPolicy();
  testContinueWithoutSavingRequiresSuccessfulCleanup();
  testCourseReplayActionAcceptsRawAndLegacyData();
  testCourseReplayCaptureDoesNotFillFailedStageHole();
  testCoursePersistenceFactsUseAuthoritativeEntrySnapshot();
  testEffectiveCourseFactsSaturateResultMetadata();
  testLegacyPartialCourseReplayPreservesEveryEntry();
  testRemoteRecordViewResultActionIsPresentedEnabled();
  testRemoteRecallExecutesExactLookupAndRetainedBackLifecycle();
  testRemoteRecallFailsClosedForConcurrentDeletion();
  testRemoteRecallRejectsStaleSelectionBeforeAndAfterLookup();
  testLocalRegressionContractsRemainPresent();
  testRawAndLegacyPlaybackAreClassifiedAsReplayResults();
  testSavedResultBuildsRetrySourceFromPersistedProvenance();
  testCourseReplayResultLabelRetainsStageSetupFlip();
  std::cout << "remote result scene tests passed\n";
  return 0;
}
