#include "ResultPersistenceModel.h"
#include "Uuid.h"

#include "BmsMetadataText.h"
#include "Utils.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kAttemptId = "123e4567-e89b-42d3-a456-426614174000";

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

ScoreProvenance populatedProvenance(const bms_parser::ChartMeta &meta) {
  ScoreProvenanceBuildInput input;
  input.chartMeta = meta;
  input.chartMeta.MD5 =
      asobmshow::bms_metadata::normalizedHash(meta.MD5);
  input.chartMeta.SHA256 =
      asobmshow::bms_metadata::normalizedHash(meta.SHA256);
  input.chartMeta.RandomSeed = 17;
  input.chartMeta.RandomPrng = "mt19937";
  input.chartMeta.RandomValues = {4, 8, 15, 16, 23, 42};
  input.longNoteMode = 2;
  input.judgeRankSource = JudgeRankSource::Chart;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = meta.TotalNotes;
  input.authoredGaugeTotal = meta.Total;
  input.effectiveGaugeTotal = std::floor(meta.Total);
  input.candidateSelection = gameplay::CandidateSelectionMode::LR2;
  input.ruleset = RulesetDescriptor::Current();
  ScoreProvenance provenance = makeScoreProvenance(input);
  provenance.gaugeType = GaugeType::Normal;
  provenance.gaugeProfile = GaugeProfile::Standard;
  provenance.gaugeAutoShift = GaugeAutoShiftMode::None;
  provenance.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  provenance.player1 = {.option = "RANDOM", .seed = 101};
  provenance.player2 = {.option = "MIRROR", .seed = 202};
  provenance.assistOption = assist_options::kOff;
  provenance.inputDevices = {InputDeviceCategory::Keyboard,
                             InputDeviceCategory::Touch};
  provenance.autoPlay = false;
  provenance.practice = false;
  provenance.clubMode = true;
  provenance.playback = {.percent = 100,
                         .mode = audio::PlaybackMode::PitchShift};
  provenance.judgeWindowScalePercent = 95;
  provenance.startingGaugePercent = 44;
  provenance.eligibility = ScoreEligibility::Verified;
  return provenance;
}

struct AttemptFixture {
  bms_parser::ChartMeta meta;
  RhythmState state{nullptr, false};
  ScoreProvenance provenance;
  ReplayData replay;
  int storageLongNoteMode = 2;

  AttemptFixture() {
    meta.SHA256 = "  " + repeated('A', 64) + "  ";
    meta.MD5 = " " + repeated('B', 32) + " ";
    meta.BmsPath = std::filesystem::path("BMS") / "sample" / "song.bms";
    meta.Folder = std::filesystem::path("BMS") / "sample";
    meta.Artist = "Fixture Artist";
    meta.SubArtist = "Fixture Subartist";
    meta.Bpm = 172.5;
    meta.Genre = "Fixture Genre";
    meta.Title = "Fixture Title";
    meta.SubTitle = "Fixture Subtitle";
    meta.Rank = 2;
    meta.Total = 180.25;
    meta.HasTotal = true;
    meta.PlayLength = 1234567;
    meta.TotalLength = 2345678;
    meta.Banner = "banner.png";
    meta.StageFile = "stage.png";
    meta.BackBmp = "back.png";
    meta.Preview = "preview.ogg";
    meta.BgaPoorDefault = true;
    meta.Difficulty = 4;
    meta.PlayLevel = 12.75;
    meta.MinBpm = 86.25;
    meta.MaxBpm = 345.0;
    meta.MostPrevalentBpm = 172.5;
    meta.GuessedBeatBpm = 171.875;
    meta.GuessedBeatsPerMeasure = 3;
    meta.Player = 1;
    meta.KeyMode = 7;
    meta.IsDP = false;
    meta.TotalNotes = 8;
    meta.TotalLongNotes = 2;
    meta.TotalScratchNotes = 1;
    meta.TotalBackSpinNotes = 1;
    meta.TotalLandmineNotes = 1;
    meta.LnMode = 0;
    meta.RandomSeed = 303U;
    meta.RandomPrng = "chart-prng";
    meta.RandomValues = {3, 1, 4};

    state.configureGauge(GaugeType::Normal, GaugeAutoShiftMode::None,
                         GaugeProfile::Standard);
    state.currentGauge = 84.5f;
    state.gaugeValues[gaugeTypeIndex(state.gaugeType)] = state.currentGauge;
    state.gaugeHistoryFor(state.gaugeType) = {61.0f, 72.5f, 84.5f};
    state.maxCombo = 6;
    state.comboBreak = 2;
    state.judgeCount[PGreat] = 2;
    state.judgeCount[Great] = 1;
    state.judgeCount[Good] = 1;
    state.judgeCount[Bad] = 1;
    state.judgeCount[Poor] = 2;
    state.judgeCount[Kpoor] = 1;
    state.judgementFastSlowCount[PGreat] = {.fast = 1, .slow = 0};
    state.judgementFastSlowCount[Great] = {.fast = 0, .slow = 1};
    state.judgementFastSlowCount[Good] = {.fast = 1, .slow = 0};
    state.judgementFastSlowCount[Bad] = {.fast = 0, .slow = 1};
    state.judgementFastSlowCount[Poor] = {.fast = 1, .slow = 1};
    state.fastCount = 3;
    state.slowCount = 3;

    provenance = populatedProvenance(meta);

    replay.id = 91;
    replay.autoPlay = false;
    replay.chartMeta = meta;
    replay.randomSeed = 404U;
    replay.randomPrng = "replay-prng";
    replay.randomValues = {2, 7, 1, 8};
    replay.playOption = "RANDOM";
    replay.playOptionSeed = 505;
    replay.playOption2 = "MIRROR";
    replay.playOption2Seed = 606;
    replay.assistOption = assist_options::kOff;
    replay.initialGaugeType = GaugeType::Normal;
    replay.gaugeAutoShift = GaugeAutoShiftMode::None;
    replay.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
    replay.finalScore = state.getScore();
    replay.maxCombo = state.maxCombo;
    replay.finalGauge = state.currentGauge;
    replay.clearType = state.getClearTypeRank();
    replay.createdAt = "2026-07-13 12:34:56";
    replay.events = {{
        .action = ReplayEventAction::Release,
        .lane = 3,
        .noteTimeMicros = 111,
        .songTimeMicros = 222,
        .judgeTimeMicros = 333,
        .judgement = Great,
        .diffMicros = -444,
        .gauge = 55.25f,
        .gaugeType = GaugeType::Hard,
        .combo = 5,
        .score = 9,
    }};
    replay.touchSamples = {{
        .action = ReplayTouchAction::Move,
        .fingerId = 707,
        .songTimeMicros = 808,
        .x = 0.25f,
        .y = 0.75f,
    }};
    replay.laneCoverEvents = {{
        .songTimeMicros = 909,
        .noteStartPositionPercent = 31,
        .resetVisibleTimeReference = true,
    }};
    replay.provenance = provenance;
  }
};

result_persistence::ChartScoreWrite scoreFor(const AttemptFixture &fixture) {
  return result_persistence::captureChartScoreWrite(
      fixture.meta, fixture.state, fixture.provenance,
      fixture.storageLongNoteMode);
}

template <typename Mutator>
void expectReplayFingerprintChange(const AttemptFixture &fixture,
                                   Mutator mutate, std::string_view fieldName) {
  const auto score = scoreFor(fixture);
  const std::string baseline =
      result_persistence::payloadFingerprint(fixture.replay, score);
  ReplayData changed = fixture.replay;
  mutate(changed);
  expect(result_persistence::payloadFingerprint(changed, score) != baseline,
         std::string("replay fingerprint covers ") + std::string(fieldName));
}

template <typename Mutator>
void expectScoreFingerprintChange(const AttemptFixture &fixture, Mutator mutate,
                                  std::string_view fieldName) {
  const auto baselineScore = scoreFor(fixture);
  const std::string baseline =
      result_persistence::payloadFingerprint(fixture.replay, baselineScore);
  auto changed = baselineScore;
  mutate(changed);
  expect(result_persistence::payloadFingerprint(fixture.replay, changed) !=
             baseline,
         std::string("score fingerprint covers ") + std::string(fieldName));
}

template <typename Mutator>
void expectScoreDifference(const AttemptFixture &fixture, Mutator mutate,
                           std::string_view expectedFragment) {
  const auto expected = scoreFor(fixture);
  auto actual = expected;
  mutate(actual);
  const std::string diagnostic =
      result_persistence::describeChartScoreDifference(expected, actual);
  expect(diagnostic.find(expectedFragment) != std::string::npos,
         std::string("score diagnostic names ") +
             std::string(expectedFragment));
}

void testUuidPolicies() {
  expect(uuid::isStructurallyValid("123E4567-E89B-42D3-A456-426614174000"),
         "legacy structural UUID accepts upper case");
  expect(uuid::isCanonicalLowerV4(kAttemptId),
         "attempt UUID accepts canonical lower v4");
  expect(!uuid::isCanonicalLowerV4("123E4567-E89B-42D3-A456-426614174000"),
         "attempt UUID rejects upper case");
  expect(!uuid::isCanonicalLowerV4("123e4567-e89b-12d3-a456-426614174000"),
         "attempt UUID rejects non-v4 version");
  expect(!uuid::isCanonicalLowerV4("123e4567-e89b-42d3-7456-426614174000"),
         "attempt UUID rejects invalid variant");
  expect(!uuid::isStructurallyValid("123e4567"),
         "structural UUID rejects wrong length");
  expect(uuid::isStructurallyValid("123e4567-e89b-12d3-7456-426614174000"),
         "legacy structural UUID preserves version and variant tolerance");

  const std::string generated = uuid::generateV4();
  expect(uuid::isCanonicalLowerV4(generated),
         "generated UUID is canonical lower v4");
}

void testScoreCapture() {
  const AttemptFixture fixture;
  const auto score = scoreFor(fixture);
  expect(score.chartPath == Utils::GetStoragePathUtf8RelativeToDocuments(
                                fixture.meta.BmsPath, "BMS/"),
         "capture uses the shared storage-path policy");
  expect(score.chartMd5 == repeated('b', 32), "capture normalizes MD5");
  expect(score.chartSha256 == repeated('a', 64), "capture normalizes SHA256");
  expect(score.chartTitle == fixture.meta.Title &&
             score.chartArtist == fixture.meta.Artist,
         "capture retains display metadata");
  expect(score.longNoteMode == fixture.storageLongNoteMode,
         "capture uses caller-supplied storage long-note mode");
  expect(score.score == fixture.state.getScore() &&
             score.maxScore == fixture.meta.TotalNotes * 2,
         "capture stores score and max score");
  expect(score.maxCombo == fixture.state.maxCombo &&
             score.comboBreak == fixture.state.comboBreak,
         "capture stores combo facts");
  expect(score.pGreat == 2 && score.great == 1 && score.good == 1 &&
             score.bad == 1 && score.poor == 2 && score.kPoor == 1,
         "capture stores named judgement counts");
  expect(score.fast == 3 && score.slow == 3,
         "capture stores fast and slow counts");
  expect(score.finalGauge == fixture.state.currentGauge &&
             score.clearType == fixture.state.getClearTypeRank(),
         "capture stores base gauge facts");
  expect(score.provenance == fixture.provenance,
         "capture stores the supplied provenance");
}

void testScoreDifferenceDiagnostics() {
  const AttemptFixture fixture;
  const auto score = scoreFor(fixture);
  expect(result_persistence::describeChartScoreDifference(score, score).empty(),
         "equal score payloads have no difference diagnostic");

  expectScoreDifference(fixture, [](auto &v) { v.chartPath += "!"; },
                        "chartPath");
  expectScoreDifference(fixture, [](auto &v) { v.chartMd5 += "!"; },
                        "chartMd5");
  expectScoreDifference(fixture, [](auto &v) { v.chartSha256 += "!"; },
                        "chartSha256");
  expectScoreDifference(fixture, [](auto &v) { v.chartTitle += "!"; },
                        "chartTitle");
  expectScoreDifference(fixture, [](auto &v) { v.chartArtist += "!"; },
                        "chartArtist");
  expectScoreDifference(fixture, [](auto &v) { ++v.longNoteMode; },
                        "longNoteMode expected=2 actual=3");
  expectScoreDifference(fixture, [](auto &v) { ++v.score; }, "score expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.maxScore; },
                        "maxScore expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.maxCombo; },
                        "maxCombo expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.comboBreak; },
                        "comboBreak expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.pGreat; },
                        "pGreat expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.great; },
                        "great expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.good; }, "good expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.bad; },
                        "bad expected=1 actual=2");
  expectScoreDifference(fixture, [](auto &v) { ++v.poor; }, "poor expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.kPoor; },
                        "kPoor expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.fast; }, "fast expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.slow; }, "slow expected=");
  expectScoreDifference(fixture, [](auto &v) { v.finalGauge += 0.5F; },
                        "finalGauge expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.clearType; },
                        "clearType expected=");
  expectScoreDifference(fixture, [](auto &v) { ++v.provenance.schemaVersion; },
                        "provenance");

  auto privateTextMismatch = score;
  privateTextMismatch.chartPath = "private/chart/location.bms";
  privateTextMismatch.chartMd5 = repeated('c', 32);
  privateTextMismatch.chartSha256 = repeated('d', 64);
  privateTextMismatch.chartTitle = "private title";
  privateTextMismatch.chartArtist = "private artist";
  const std::string privateTextDiagnostic =
      result_persistence::describeChartScoreDifference(score,
                                                       privateTextMismatch);
  expect(privateTextDiagnostic.find("private") == std::string::npos &&
             privateTextDiagnostic.find(repeated('c', 32)) ==
                 std::string::npos &&
             privateTextDiagnostic.find(repeated('d', 64)) ==
                 std::string::npos,
         "score difference diagnostic does not expose text payloads");
}

void expectRejected(const AttemptFixture &fixture, std::string attemptId,
                    std::string_view expectedDiagnostic) {
  std::string diagnostic;
  const auto attempt = result_persistence::makeChartResultAttempt(
      std::move(attemptId), fixture.meta, fixture.state, fixture.provenance,
      fixture.storageLongNoteMode, fixture.replay, diagnostic);
  expect(!attempt.has_value(),
         std::string("rejects ") + std::string(expectedDiagnostic));
  expect(diagnostic == expectedDiagnostic,
         std::string("diagnostic names only ") +
             std::string(expectedDiagnostic));
  expect(diagnostic.find("BMS") == std::string::npos &&
             diagnostic.find("Fixture") == std::string::npos &&
             diagnostic.find("123e4567") == std::string::npos,
         "diagnostic contains no payload data");
}

void testAttemptValidationAndFingerprint() {
  AttemptFixture fixture;
  std::string diagnostic = "stale";
  auto attempt = result_persistence::makeChartResultAttempt(
      std::string(kAttemptId), fixture.meta, fixture.state, fixture.provenance,
      fixture.storageLongNoteMode, fixture.replay, diagnostic);
  expect(attempt.has_value(), "valid completed attempt is accepted");
  expect(diagnostic.empty(), "successful construction clears diagnostic");
  expect(attempt && attempt->attemptId == kAttemptId,
         "attempt retains its identity");
  expect(attempt && attempt->payloadFingerprint.size() == 64,
         "attempt fingerprint is SHA-256 hex");
  expect(attempt && attempt->adoptedGaugeHistory ==
                        std::vector<float>({61.0f, 72.5f, 84.5f}),
         "attempt snapshots the complete final adopted gauge series");
  expect(attempt && attempt->judgementTiming.has_value() &&
             attempt->judgementTiming->byJudgement[PGreat] ==
                 JudgementFastSlowCount{.fast = 1, .slow = 0} &&
             attempt->judgementTiming->byJudgement[Great] ==
                 JudgementFastSlowCount{.fast = 0, .slow = 1} &&
             attempt->judgementTiming->byJudgement[Good] ==
                 JudgementFastSlowCount{.fast = 1, .slow = 0} &&
             attempt->judgementTiming->byJudgement[Bad] ==
                 JudgementFastSlowCount{.fast = 0, .slow = 1} &&
             attempt->judgementTiming->byJudgement[Poor] ==
                 JudgementFastSlowCount{.fast = 1, .slow = 1},
         "attempt snapshots authoritative per-judgement timing");
  expect(attempt && attempt->payloadFingerprint.find_first_not_of(
                        "0123456789abcdef") == std::string::npos,
         "attempt fingerprint is canonical lower hex");
  expect(attempt && attempt->payloadFingerprint ==
                        result_persistence::payloadFingerprint(attempt->replay,
                                                               attempt->score),
         "attempt stores its canonical payload fingerprint");
  auto changed = fixture.replay;
  changed.events.front().diffMicros += 1;
  expect(attempt && attempt->payloadFingerprint !=
                        result_persistence::payloadFingerprint(changed,
                                                               attempt->score),
         "event change changes fingerprint");

  expectRejected(fixture, "123e4567-e89b-12d3-a456-426614174000",
                 "invalid attempt ID");

  auto wrongIdentity = fixture;
  wrongIdentity.replay.chartMeta.SHA256 = repeated('c', 64);
  wrongIdentity.replay.chartMeta.MD5 = repeated('d', 32);
  expectRejected(wrongIdentity, std::string(kAttemptId),
                 "chart identity mismatch");

  auto md5OnlyIdentity = fixture;
  md5OnlyIdentity.meta.SHA256.clear();
  md5OnlyIdentity.replay.chartMeta.SHA256.clear();
  md5OnlyIdentity.provenance.stages.front().chartSha256.clear();
  md5OnlyIdentity.replay.provenance = md5OnlyIdentity.provenance;
  expectRejected(md5OnlyIdentity, std::string(kAttemptId),
                 "chart identity is not projectable");

  auto wrongProvenance = fixture;
  wrongProvenance.replay.provenance.player1.option = "MIRROR";
  expectRejected(wrongProvenance, std::string(kAttemptId),
                 "provenance mismatch");

  auto wrongScore = fixture;
  ++wrongScore.replay.finalScore;
  expectRejected(wrongScore, std::string(kAttemptId), "final score mismatch");

  auto wrongGauge = fixture;
  wrongGauge.replay.finalGauge += 0.25f;
  expectRejected(wrongGauge, std::string(kAttemptId), "final gauge mismatch");

  auto wrongCombo = fixture;
  ++wrongCombo.replay.maxCombo;
  expectRejected(wrongCombo, std::string(kAttemptId), "max combo mismatch");

  auto wrongClear = fixture;
  wrongClear.replay.clearType = kClearTypeFailedRank;
  expectRejected(wrongClear, std::string(kAttemptId), "clear type mismatch");
}

void testFullComboNormalization() {
  AttemptFixture fixture;
  fixture.state.maxCombo = fixture.meta.TotalNotes;
  fixture.state.comboBreak = 0;
  fixture.replay.maxCombo = fixture.state.maxCombo;
  fixture.replay.finalScore = fixture.state.getScore();
  fixture.replay.finalGauge = fixture.state.currentGauge;
  fixture.replay.clearType = clear_policy::fullComboRankForPlayback(
      fixture.state.getClearTypeRank(), true, fixture.provenance.playback);

  std::string diagnostic;
  const auto fullCombo = result_persistence::makeChartResultAttempt(
      std::string(kAttemptId), fixture.meta, fixture.state, fixture.provenance,
      fixture.storageLongNoteMode, fixture.replay, diagnostic);
  expect(fullCombo.has_value(), "valid full-combo replay rank is accepted");
  expect(fullCombo && fullCombo->score.clearType == kClearTypeNormalClearRank &&
             fullCombo->replay.clearType == kClearTypeFullComboRank,
         "score keeps base rank while replay keeps full-combo rank");

  fixture.provenance.playback.percent = 75;
  fixture.replay.provenance = fixture.provenance;
  fixture.replay.clearType = clear_policy::fullComboRankForPlayback(
      fixture.state.getClearTypeRank(), true, fixture.provenance.playback);
  const auto assisted = result_persistence::makeChartResultAttempt(
      std::string(kAttemptId), fixture.meta, fixture.state, fixture.provenance,
      fixture.storageLongNoteMode, fixture.replay, diagnostic);
  expect(assisted.has_value() &&
             assisted->replay.clearType == kClearTypeAssistedEasyClearRank,
         "playback policy caps the replay full-combo rank");
}

void testVersionOneFingerprintGolden() {
  ReplayData replay;
  replay.id = 99;
  replay.createdAt = "excluded database timestamp";
  const result_persistence::ChartScoreWrite score;
  const std::string actual =
      result_persistence::payloadFingerprint(replay, score);
  expect(actual ==
             "c8744f5007aa619288309622462545732f2ae4a40a772ce5ff012d2542c7dac4",
         "v1 fingerprint remains stable");
}

void testReplayFingerprintCoverage() {
  const AttemptFixture fixture;

  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.autoPlay = true; }, "autoPlay");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.SHA256[3] = 'c'; },
      "chartMeta.SHA256");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.MD5[2] = 'c'; }, "chartMeta.MD5");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.BmsPath /= "changed"; },
      "chartMeta.BmsPath");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Folder /= "changed"; },
      "chartMeta.Folder");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Artist += "!"; }, "chartMeta.Artist");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.SubArtist += "!"; },
      "chartMeta.SubArtist");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Bpm += 0.5; }, "chartMeta.Bpm");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Genre += "!"; }, "chartMeta.Genre");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Title += "!"; }, "chartMeta.Title");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.SubTitle += "!"; },
      "chartMeta.SubTitle");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.Rank; }, "chartMeta.Rank");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Total += 0.5; }, "chartMeta.Total");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.HasTotal = !v.chartMeta.HasTotal; },
      "chartMeta.HasTotal");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.PlayLength; },
      "chartMeta.PlayLength");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalLength; },
      "chartMeta.TotalLength");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Banner /= "changed"; },
      "chartMeta.Banner");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.StageFile /= "changed"; },
      "chartMeta.StageFile");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.BackBmp /= "changed"; },
      "chartMeta.BackBmp");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.Preview /= "changed"; },
      "chartMeta.Preview");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.chartMeta.BgaPoorDefault = !v.chartMeta.BgaPoorDefault; },
      "chartMeta.BgaPoorDefault");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.Difficulty; },
      "chartMeta.Difficulty");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.PlayLevel += 0.25; },
      "chartMeta.PlayLevel");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.MinBpm += 0.25; }, "chartMeta.MinBpm");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.MaxBpm += 0.25; }, "chartMeta.MaxBpm");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.MostPrevalentBpm += 0.25; },
      "chartMeta.MostPrevalentBpm");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.GuessedBeatBpm += 0.25; },
      "chartMeta.GuessedBeatBpm");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.GuessedBeatsPerMeasure; },
      "chartMeta.GuessedBeatsPerMeasure");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.Player; }, "chartMeta.Player");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.KeyMode; }, "chartMeta.KeyMode");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.IsDP = !v.chartMeta.IsDP; },
      "chartMeta.IsDP");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalNotes; },
      "chartMeta.TotalNotes");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalLongNotes; },
      "chartMeta.TotalLongNotes");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalScratchNotes; },
      "chartMeta.TotalScratchNotes");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalBackSpinNotes; },
      "chartMeta.TotalBackSpinNotes");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.TotalLandmineNotes; },
      "chartMeta.TotalLandmineNotes");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.chartMeta.LnMode; }, "chartMeta.LnMode");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.RandomSeed.reset(); },
      "chartMeta.RandomSeed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.RandomPrng.reset(); },
      "chartMeta.RandomPrng optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.chartMeta.RandomValues.push_back(9); },
      "chartMeta.RandomValues vector");

  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.randomSeed.reset(); }, "randomSeed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.randomPrng.reset(); }, "randomPrng optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.randomValues.push_back(9); },
      "randomValues vector");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.playOption.reset(); }, "playOption optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.playOptionSeed.reset(); },
      "playOptionSeed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.playOption2.reset(); }, "playOption2 optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.playOption2Seed.reset(); },
      "playOption2Seed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.assistOption += "!"; }, "assistOption");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.initialGaugeType = GaugeType::Easy; },
      "initialGaugeType");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.gaugeAutoShift = GaugeAutoShiftMode::BestClear; },
      "gaugeAutoShift");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.gaugeAutoShiftLowerBound = GaugeType::Easy; },
      "gaugeAutoShiftLowerBound");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.finalScore; }, "finalScore");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.maxCombo; }, "maxCombo");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.finalGauge += 0.5f; }, "finalGauge");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.clearType; }, "clearType");

  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.events.front().action = ReplayEventAction::Mine; },
      "events.action");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().lane; }, "events.lane");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().noteTimeMicros; },
      "events.noteTimeMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().songTimeMicros; },
      "events.songTimeMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().judgeTimeMicros; },
      "events.judgeTimeMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.events.front().judgement = Bad; },
      "events.judgement");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().diffMicros; },
      "events.diffMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.events.front().gauge += 0.5f; }, "events.gauge");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.events.front().gaugeType = GaugeType::Hazard; },
      "events.gaugeType");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().combo; }, "events.combo");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.events.front().score; }, "events.score");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.events.emplace_back(); },
      "events vector length");

  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.touchSamples.front().action = ReplayTouchAction::Up; },
      "touchSamples.action");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.touchSamples.front().fingerId; },
      "touchSamples.fingerId");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.touchSamples.front().songTimeMicros; },
      "touchSamples.songTimeMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.touchSamples.front().x += 0.125f; },
      "touchSamples.x");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.touchSamples.front().y += 0.125f; },
      "touchSamples.y");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.touchSamples.emplace_back(); },
      "touchSamples vector length");

  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.laneCoverEvents.front().songTimeMicros; },
      "laneCoverEvents.songTimeMicros");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { ++v.laneCoverEvents.front().noteStartPositionPercent; },
      "laneCoverEvents.noteStartPositionPercent");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        auto &event = v.laneCoverEvents.front();
        event.resetVisibleTimeReference = !event.resetVisibleTimeReference;
      },
      "laneCoverEvents.resetVisibleTimeReference");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.laneCoverEvents.emplace_back(); },
      "laneCoverEvents vector length");

  const auto score = scoreFor(fixture);
  const std::string baseline =
      result_persistence::payloadFingerprint(fixture.replay, score);
  auto databaseOnly = fixture.replay;
  ++databaseOnly.id;
  databaseOnly.createdAt += " changed";
  expect(result_persistence::payloadFingerprint(databaseOnly, score) ==
             baseline,
         "fingerprint excludes replay database ID and timestamp");

  auto positiveZero = fixture.replay;
  positiveZero.events.front().gauge = 0.0f;
  auto negativeZero = positiveZero;
  negativeZero.events.front().gauge = -0.0f;
  expect(result_persistence::payloadFingerprint(positiveZero, score) !=
             result_persistence::payloadFingerprint(negativeZero, score),
         "fingerprint preserves floating-point bit patterns");
}

void testProvenanceFingerprintCoverage() {
  const AttemptFixture fixture;
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.schemaVersion; },
      "provenance.schemaVersion");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.ruleset.id += "!"; },
      "provenance.ruleset.id");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.ruleset.version; },
      "provenance.ruleset.version");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.ruleset.scoringModel += "!"; },
      "provenance.ruleset.scoringModel");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.ruleset.judgementModel += "!"; },
      "provenance.ruleset.judgementModel");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.ruleset.gaugeModel += "!"; },
      "provenance.ruleset.gaugeModel");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.stages.emplace_back(); },
      "provenance.stages vector");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.stages.front().chartMd5 += "!"; },
      "provenance.stage.chartMd5");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.stages.front().chartSha256 += "!"; },
      "provenance.stage.chartSha256");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.stages.front().longNoteMode; },
      "provenance.stage.longNoteMode");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.stages.front().chartRandomSeed.reset(); },
      "provenance.stage.chartRandomSeed optional");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.stages.front().chartRandomPrng.reset(); },
      "provenance.stage.chartRandomPrng optional");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().chartRandomValues.push_back(108);
      },
      "provenance.stage.chartRandomValues vector");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().judgeRankSource = JudgeRankSource::Override;
      },
      "provenance.stage.judgeRankSource");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.stages.front().sourceJudgeRank.reset(); },
      "provenance.stage.sourceJudgeRank optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.stages.front().totalNotes; },
      "provenance.stage.totalNotes");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().authoredGaugeTotal.reset();
      },
      "provenance.stage.authoredGaugeTotal optional");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { ++v.provenance.stages.front().effectiveGaugeTotal; },
      "provenance.stage.effectiveGaugeTotal");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().candidateSelection =
            gameplay::CandidateSelectionMode::Lowest;
      },
      "provenance.stage.candidateSelection");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().effectiveJudgeWindows.emplace_back();
      },
      "provenance.stage.effectiveJudgeWindows vector");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().effectiveJudgeWindows.front().judgement =
            Good;
      },
      "provenance.window.judgement");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.stages.front().effectiveJudgeWindows.front().context =
            gameplay::JudgeWindowContext::Scratch;
      },
      "provenance.window.context");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        ++v.provenance.stages.front().effectiveJudgeWindows.front().earlyMicros;
      },
      "provenance.window.earlyMicros");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        ++v.provenance.stages.front().effectiveJudgeWindows.front().lateMicros;
      },
      "provenance.window.lateMicros");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.gaugeType = GaugeType::Hard; },
      "provenance.gaugeType");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.gaugeProfile = GaugeProfile::CourseLR2; },
      "provenance.gaugeProfile");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.gaugeAutoShift = GaugeAutoShiftMode::SelectToUnder;
      },
      "provenance.gaugeAutoShift");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.gaugeAutoShiftLowerBound = GaugeType::Easy; },
      "provenance.gaugeAutoShiftLowerBound");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.player1.option += "!"; },
      "provenance.player1.option");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.player1.seed.reset(); },
      "provenance.player1.seed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.player2.option += "!"; },
      "provenance.player2.option");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.player2.seed.reset(); },
      "provenance.player2.seed optional");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.assistOption += "!"; },
      "provenance.assistOption");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.inputDevices.push_back(InputDeviceCategory::Midi);
      },
      "provenance.inputDevices vector");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.autoPlay = !v.provenance.autoPlay; },
      "provenance.autoPlay");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.practice = !v.provenance.practice; },
      "provenance.practice");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.clubMode = !v.provenance.clubMode; },
      "provenance.clubMode");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.playback.percent = 105; },
      "provenance.playback.percent");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) {
        v.provenance.playback.mode = audio::PlaybackMode::TimeStretch;
      },
      "provenance.playback.mode");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.judgeWindowScalePercent; },
      "provenance.judgeWindowScalePercent");
  expectReplayFingerprintChange(
      fixture, [](auto &v) { v.provenance.startingGaugePercent.reset(); },
      "provenance.startingGaugePercent optional");
  expectReplayFingerprintChange(
      fixture,
      [](auto &v) { v.provenance.eligibility = ScoreEligibility::Modified; },
      "provenance.eligibility");
}

void testScoreFingerprintCoverage() {
  const AttemptFixture fixture;
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.chartPath += "!"; }, "chartPath");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.chartMd5 += "!"; }, "chartMd5");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.chartSha256 += "!"; }, "chartSha256");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.chartTitle += "!"; }, "chartTitle");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.chartArtist += "!"; }, "chartArtist");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.longNoteMode; }, "longNoteMode");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.score; }, "score");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.maxScore; }, "maxScore");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.maxCombo; }, "maxCombo");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.comboBreak; }, "comboBreak");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.pGreat; }, "pGreat");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.great; }, "great");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.good; }, "good");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.bad; }, "bad");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.poor; }, "poor");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.kPoor; }, "kPoor");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.fast; }, "fast");
  expectScoreFingerprintChange(fixture, [](auto &v) { ++v.slow; }, "slow");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { v.finalGauge += 0.5f; }, "finalGauge");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.clearType; }, "clearType");
  expectScoreFingerprintChange(
      fixture, [](auto &v) { ++v.provenance.schemaVersion; }, "provenance");
}

} // namespace

int main() {
  testUuidPolicies();
  testScoreCapture();
  testScoreDifferenceDiagnostics();
  testAttemptValidationAndFingerprint();
  testFullComboNormalization();
  testVersionOneFingerprintGolden();
  testReplayFingerprintCoverage();
  testProvenanceFingerprintCoverage();
  testScoreFingerprintCoverage();
  if (failures != 0) {
    std::cerr << failures << " result persistence model test(s) failed\n";
    return 1;
  }
  std::cout << "result persistence model tests passed\n";
  return 0;
}
