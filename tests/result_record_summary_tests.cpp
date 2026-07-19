#include "../src/ResultRecordSummary.h"

#include <climits>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Callable>
void expectInvalid(Callable &&callable, const char *message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::invalid_argument &) {
    return;
  } catch (...) {
    std::cerr << "FAIL: " << message << " (wrong exception type)\n";
    ++failures;
    return;
  }
  std::cerr << "FAIL: " << message << " (accepted invalid input)\n";
  ++failures;
}

ir::IrRemoteScore validRemoteScore() {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "remote-score-42",
      .remoteChartId = "remote-chart-42",
      .chartMd5 = std::string(32, 'a'),
      .chartSha256 = std::string(64, 'b'),
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .difficulty = "ANOTHER",
      .level = "12",
      .levelNumber = 12.4,
      .noteCount = 1'234,
      .score = 2'100,
      .lampRank = kClearTypeHardClearRank,
      .timeAchievedUnixMillis = 1'704'164'645'123LL,
      .timeAddedUnixMillis = 1'704'164'700'456LL,
      .judgements = {.pGreat = 900,
                     .great = 200,
                     .good = 50,
                     .bad = 40,
                     .poor = 44},
      .timing = {.earlyPGreat = 450,
                 .latePGreat = 450,
                 .earlyGreat = 100,
                 .lateGreat = 100,
                 .earlyGood = 25,
                 .lateGood = 25,
                 .earlyBad = 20,
                 .lateBad = 20,
                 .earlyPoor = 22,
                 .latePoor = 22},
      .fast = 617,
      .slow = 617,
      .maxCombo = 777,
      .badPoints = 84,
      .finalGauge = 78.5F,
      .gaugeHistory = {20.0F, std::nullopt, 78.5F},
      .random = "RANDOM",
      .gauge = "HARD",
      .inputDevice = "Keyboard",
      .client = "AsoBMaShow",
  };
}

void testLocalConversionPreservesRecordSemantics() {
  ReplaySummary replay;
  replay.id = 73;
  replay.finalScore = 987;
  replay.maxScore = 1'000;
  replay.maxCombo = 500;
  replay.clearType = kClearTypeNormalClearRank;
  replay.createdAt = "2024-01-02 03:04:05";
  replay.playOption = "MIRROR";
  replay.irRecordState = ir::IrRecordState::Eligible;
  replay.irSubmissionEligible = true;

  const ResultRecordSummary result = makeLocalResultRecord(replay);

  expect(result.isLocal() && !result.isRemote(),
         "local conversion keeps the local tag");
  expect(result.localReplayId() == 73 && !result.remoteScoreId(),
         "local conversion exposes only the replay identity");
  expect(std::get<LocalReplayRecordId>(result.identity).replayId == 73,
         "local identity preserves the replay ID");
  expect(result.capabilities.watch && result.capabilities.gBattle &&
             result.capabilities.resultRecall &&
             result.capabilities.videoExport && result.capabilities.irUpload,
         "normal local replay preserves all current actions");
  expect(!result.course && !result.autoPlay && result.score == 987 &&
             result.maxScore == 1'000 && result.maxCombo == 500,
         "local score fields are preserved");
  expect(result.clearRank == kClearTypeFullComboRank,
         "local conversion preserves effective full-combo clear behavior");
  expect(result.displayedTimeUnixMillis == 1'704'164'645'000LL &&
             result.displayedTime == replay.createdAt,
         "local timestamp keeps display text and obtains a sortable time");
  expect(result.playOption == replay.playOption &&
             result.irState == ir::IrRecordState::Eligible,
         "local option and semantic IR state are preserved");
  expect(result.local && result.local->id == replay.id && !result.remote,
         "local conversion nests the original replay-only summary");

  replay.courseReplay = true;
  replay.irRecordState = ir::IrRecordState::Hidden;
  const ResultRecordSummary course = makeLocalResultRecord(replay);
  expect(course.course && course.capabilities.watch &&
             !course.capabilities.gBattle &&
             course.capabilities.resultRecall &&
             course.capabilities.videoExport && !course.capabilities.irUpload,
         "course replay keeps current browsing actions");

  replay.courseReplay = false;
  replay.autoPlay = true;
  replay.id = -1;
  const ResultRecordSummary autoPlay = makeLocalResultRecord(replay);
  expect(autoPlay.autoPlay && autoPlay.localReplayId() == -1 &&
             autoPlay.capabilities.watch && !autoPlay.capabilities.gBattle &&
             !autoPlay.capabilities.resultRecall &&
             autoPlay.capabilities.videoExport &&
             !autoPlay.capabilities.irUpload,
         "Auto Play keeps its current local capability semantics");

  replay.autoPlay = false;
  replay.id = 74;
  replay.irRecordState = ir::IrRecordState::Failed;
  expect(makeLocalResultRecord(replay).capabilities.irUpload,
         "failed local IR upload remains retryable");
  replay.irRecordState = ir::IrRecordState::Uploaded;
  const ResultRecordSummary uploaded = makeLocalResultRecord(replay);
  expect(!uploaded.capabilities.irUpload &&
             uploaded.irState == ir::IrRecordState::Uploaded,
         "uploaded local IR state is visible but not uploadable");
}

void testRemoteConversionIsReadOnlyAndRetainsOptionalValues() {
  const ir::IrRemoteScore score = validRemoteScore();
  const ResultRecordSummary result = makeRemoteResultRecord(
      "tachi", "https://boku.tachi.ac", score);

  expect(result.isRemote() && !result.isLocal(),
         "remote conversion keeps the remote tag");
  expect(!result.localReplayId() &&
             result.remoteScoreId() == score.remoteScoreId,
         "remote conversion exposes only the remote score identity");
  const auto &identity = std::get<IrRemoteRecordId>(result.identity);
  expect(identity.providerId == "tachi" &&
             identity.serverOrigin == "https://boku.tachi.ac" &&
             identity.remoteScoreId == score.remoteScoreId,
         "remote identity is scoped by provider and normalized origin");
  expect(!result.capabilities.watch && !result.capabilities.gBattle &&
             result.capabilities.resultRecall &&
             !result.capabilities.videoExport &&
             !result.capabilities.irUpload,
         "remote records expose View Result only");
  expect(!result.course && !result.autoPlay && result.score == score.score &&
             result.maxScore == 2'468 && result.maxCombo == score.maxCombo &&
             result.clearRank == score.lampRank,
         "remote score conversion preserves supplied values and derives max");
  expect(result.displayedTimeUnixMillis ==
                 *score.timeAchievedUnixMillis &&
             result.displayedTime == "2024-01-02 03:04:05.123",
         "remote display time prefers timeAchieved");
  expect(result.playOption == score.random &&
             result.irState == ir::IrRecordState::Uploaded,
         "remote option remains optional and IR is read-only uploaded");
  expect(!result.local && result.remote &&
             result.remote->remoteScoreId == score.remoteScoreId,
         "remote conversion nests only the validated stored score model");

  ir::IrRemoteScore fallback = score;
  fallback.remoteScoreId = "fallback-time";
  fallback.timeAchievedUnixMillis.reset();
  const ResultRecordSummary fallbackResult = makeRemoteResultRecord(
      "tachi", "https://boku.tachi.ac", fallback);
  expect(fallbackResult.displayedTimeUnixMillis ==
                 fallback.timeAddedUnixMillis &&
             fallbackResult.displayedTime == "2024-01-02 03:05:00.456",
         "remote display time falls back to timeAdded");

  fallback.remoteScoreId = "missing-optionals";
  fallback.maxCombo.reset();
  fallback.random.reset();
  const ResultRecordSummary missing = makeRemoteResultRecord(
      "tachi", "https://boku.tachi.ac", fallback);
  expect(!missing.maxCombo && !missing.playOption && missing.remote &&
             !missing.remote->maxCombo && !missing.remote->random,
         "remote nullable fields remain absent instead of becoming sentinels");
}

void testRemoteConversionFailsClosed() {
  const ir::IrRemoteScore valid = validRemoteScore();
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "Tachi", "https://boku.tachi.ac", valid));
      },
      "noncanonical provider ID is rejected");
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "HTTPS://BOKU.TACHI.AC:443/", valid));
      },
      "non-normalized origin is rejected");

  ir::IrRemoteScore invalidIdentity = valid;
  invalidIdentity.remoteScoreId = std::string("score\nsecret", 12);
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "https://boku.tachi.ac", invalidIdentity));
      },
      "invalid remote score identity is rejected");

  ir::IrRemoteScore overflowing = valid;
  overflowing.noteCount = INT_MAX;
  overflowing.score = 0;
  expectInvalid(
      [&] {
        static_cast<void>(makeRemoteResultRecord(
            "tachi", "https://boku.tachi.ac", overflowing));
      },
      "note-count doubling overflow is rejected before multiplication");
}

void testIdentityEqualityHashAndStableKeys() {
  const ResultRecordIdentity localA = LocalReplayRecordId{.replayId = 42};
  const ResultRecordIdentity localB = LocalReplayRecordId{.replayId = 42};
  const ResultRecordIdentity localOther = LocalReplayRecordId{.replayId = 43};
  const ResultRecordIdentity remoteA = IrRemoteRecordId{
      .providerId = "tachi",
      .serverOrigin = "https://boku.tachi.ac",
      .remoteScoreId = "score:42",
  };
  const ResultRecordIdentity remoteB = remoteA;
  const ResultRecordIdentity remoteOther = IrRemoteRecordId{
      .providerId = "tachi",
      .serverOrigin = "https://scores.example.test",
      .remoteScoreId = "score:42",
  };

  expect(localA == localB && localA != localOther && localA != remoteA &&
             remoteA == remoteB && remoteA != remoteOther,
         "tagged identity equality includes type and every remote scope");
  std::unordered_set<ResultRecordIdentity, ResultRecordIdentityHash> values;
  values.insert(localA);
  values.insert(localB);
  values.insert(remoteA);
  values.insert(remoteB);
  values.insert(remoteOther);
  expect(values.size() == 3,
         "identity hash agrees with tagged identity equality");

  ReplaySummary replay;
  replay.id = 42;
  const ResultRecordSummary local = makeLocalResultRecord(replay);
  ir::IrRemoteScore score = validRemoteScore();
  score.remoteScoreId = "42";
  const ResultRecordSummary remote = makeRemoteResultRecord(
      "tachi", "https://boku.tachi.ac", score);
  expect(local.stableKey() != remote.stableKey(),
         "local and remote display keys cannot collide");

  score.remoteScoreId = "bc";
  const ResultRecordSummary componentA = makeRemoteResultRecord(
      "tachi", "https://a.example", score);
  score.remoteScoreId = "c";
  const ResultRecordSummary componentB = makeRemoteResultRecord(
      "tachi", "https://a.exampleb", score);
  expect(componentA.stableKey() != componentB.stableKey(),
         "remote display keys frame identity components unambiguously");

  score.remoteScoreId.assign(ir::kMaximumIrRemoteScoreIdBytes, 's');
  const ResultRecordSummary maximumId = makeRemoteResultRecord(
      "tachi", "https://boku.tachi.ac", score);
  expect(maximumId.stableKey().size() <=
             kMaximumResultRecordStableKeyBytes,
         "validated display keys have an explicit UI-safe size bound");
}

} // namespace

int main() {
  testLocalConversionPreservesRecordSemantics();
  testRemoteConversionIsReadOnlyAndRetainsOptionalValues();
  testRemoteConversionFailsClosed();
  testIdentityEqualityHashAndStableKeys();

  if (failures != 0) {
    std::cerr << failures << " result record summary assertion(s) failed\n";
    return 1;
  }
  std::cout << "result record summary tests passed\n";
  return 0;
}
