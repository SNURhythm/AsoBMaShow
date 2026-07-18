#include "ScoreProvenance.h"
#include "input/InputTypes.h"
#include "scene/play/GamePlayStartOptions.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

std::map<Judgement, std::pair<long long, long long>> sampleWindows() {
  return {
      {Bad, {-330000, 420000}},   {PGreat, {-10000, 10000}},
      {Great, {-30000, 30000}},   {Good, {-75000, 75000}},
      {Kpoor, {-500000, 150000}},
  };
}

ScoreProvenanceBuildInput sampleInput(const std::string &hashSuffix = "one") {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = "md5-" + hashSuffix;
  input.chartMeta.SHA256 = "sha256-" + hashSuffix;
  input.chartMeta.Rank = 1;
  input.chartMeta.RandomSeed = 42U;
  input.chartMeta.RandomPrng = "mt19937";
  input.chartMeta.RandomValues = {7, 3, 11};
  input.longNoteMode = 2;
  input.judgeRankSource = JudgeRankSource::Chart;
  input.sourceJudgeRank = 1;
  input.effectiveJudgeWindows = sampleWindows();
  input.gaugeType = GaugeType::Hard;
  input.gaugeProfile = GaugeProfile::Standard;
  input.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  input.player1 = {.option = "RANDOM", .seed = 12345};
  input.player2 = {.option = "MIRROR", .seed = std::nullopt};
  input.assistOption = assist_options::kOff;
  input.inputDevices = {InputDeviceCategory::Touch,
                        InputDeviceCategory::Keyboard,
                        InputDeviceCategory::Touch, InputDeviceCategory::Midi};
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  return input;
}

ScoreProvenance
sampleVerifiedProvenance(const std::string &hashSuffix = "one") {
  return makeScoreProvenance(sampleInput(hashSuffix));
}

void testRulesetContract() {
  const RulesetDescriptor rules = RulesetDescriptor::Current();
  assert(rules.id == "lr2");
  assert(rules.version == RulesetDescriptor::kCurrentVersion);
  assert(rules.scoringModel == "asobmashow-v1");
  assert(rules.judgementModel == "lr2-v1");
  assert(rules.gaugeModel == "lr2-gauge-v1");
  assert(isSupportedRulesetDescriptor(rules));

  const RulesetDescriptor beatoraja =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  assert(beatoraja.id == "beatoraja");
  assert(beatoraja.version == 2);
  assert(beatoraja.scoringModel == "asobmashow-v1");
  assert(beatoraja.judgementModel == "bms-rank-v1");
  assert(beatoraja.gaugeModel == "beatoraja-profile-gauge-v2");
  assert(isSupportedRulesetDescriptor(beatoraja));

  const RulesetDescriptor legacy = RulesetDescriptor::Legacy();
  assert(legacy.version == 0);
  assert(!isSupportedRulesetDescriptor(legacy));
}

void testSchemaAndInputDeviceVocabularyContract() {
  assert(ScoreProvenance::kSchemaVersion == 3);
  assert(static_cast<int>(InputDeviceCategory::Keyboard) == 0);
  assert(static_cast<int>(InputDeviceCategory::GameController) == 1);
  assert(static_cast<int>(InputDeviceCategory::Joystick) == 2);
  assert(static_cast<int>(InputDeviceCategory::Touch) == 3);
  assert(static_cast<int>(InputDeviceCategory::Midi) == 4);
  assert(static_cast<int>(InputDeviceCategory::Unknown) == 5);
  assert(static_cast<int>(InputDeviceCategory::Gyroscope) == 6);

  auto input = sampleInput();
  input.inputDevices = {InputDeviceCategory::Gyroscope,
                        InputDeviceCategory::Keyboard,
                        InputDeviceCategory::Gyroscope};
  const ScoreProvenance value = makeScoreProvenance(input);
  assert((value.inputDevices == std::vector{InputDeviceCategory::Keyboard,
                                            InputDeviceCategory::Gyroscope}));
  const std::string json = serializeScoreProvenance(value);
  assert(json.find("\"inputDevices\":[\"keyboard\",\"gyroscope\"]") !=
         std::string::npos);
  std::string error;
  const auto decoded = deserializeScoreProvenance(json, error);
  assert(error.empty());
  assert(decoded == value);

  assert(playStartInputDeviceCategory(input::DeviceClass::Gyroscope) ==
         InputDeviceCategory::Gyroscope);
  const std::vector resolverClasses = {input::DeviceClass::Gyroscope};
  assert(collectPlayStartInputDeviceCategories(
             resolverClasses, PlayStartInputPlatform::Mobile) ==
         std::vector({InputDeviceCategory::Touch,
                      InputDeviceCategory::Gyroscope}));
}

void testDeterministicRoundTrip() {
  const ScoreProvenance value = sampleVerifiedProvenance();
  const std::string first = serializeScoreProvenance(value);
  const std::string second = serializeScoreProvenance(value);
  assert(first == second);

  std::string error;
  const auto decoded = deserializeScoreProvenance(first, error);
  assert(error.empty());
  assert(decoded == value);
}

void testPlaybackAndJudgeProvenanceRoundTripAndMigration() {
  auto input = sampleInput();
  input.clubMode = true;
  input.playback = {.percent = 75, .mode = audio::PlaybackMode::PitchShift};
  input.judgeWindowScalePercent = 80;
  input.startingGaugePercent = 37;
  const ScoreProvenance modified = makeScoreProvenance(input);
  assert(modified.eligibility == ScoreEligibility::Modified);
  assert(modified.playback.percent == 75);
  assert(modified.playback.mode == audio::PlaybackMode::PitchShift);
  assert(modified.judgeWindowScalePercent == 80);
  assert(modified.startingGaugePercent == 37);
  assert(modified.clubMode);

  const std::string serialized = serializeScoreProvenance(modified);
  assert(serialized.find("\"mode\":\"pitch-shift\"") != std::string::npos);
  std::string error;
  const auto roundTrip = deserializeScoreProvenance(serialized, error);
  assert(error.empty());
  assert(roundTrip == modified);

  auto timeStretch = input;
  timeStretch.playback = {.percent = 125,
                          .mode = audio::PlaybackMode::TimeStretch};
  const auto timeStretchRoundTrip = deserializeScoreProvenance(
      serializeScoreProvenance(makeScoreProvenance(timeStretch)), error);
  assert(error.empty());
  assert(timeStretchRoundTrip.has_value());
  assert(timeStretchRoundTrip->playback == timeStretch.playback);
  assert(serializeScoreProvenance(*timeStretchRoundTrip)
             .find("\"mode\":\"time-stretch\"") != std::string::npos);

  auto legacyRoot = nlohmann::json::parse(
      serializeScoreProvenance(sampleVerifiedProvenance()));
  legacyRoot["schemaVersion"] = 2;
  legacyRoot.erase("playback");
  legacyRoot.erase("judgeWindowScalePercent");
  legacyRoot.erase("startingGaugePercent");
  legacyRoot.erase("clubMode");
  const auto migrated = deserializeScoreProvenance(legacyRoot.dump(), error);
  assert(error.empty());
  assert(migrated.has_value());
  assert(migrated->playback == audio::PlaybackRate{});
  assert(migrated->judgeWindowScalePercent == 100);
  assert(!migrated->startingGaugePercent.has_value());
  assert(!migrated->clubMode);
}

void testPlaybackAndJudgeProvenanceValidation() {
  const auto assertValid = [](ScoreProvenance value) {
    std::string error;
    assert(serializeValidatedScoreProvenance(value, error).has_value());
    assert(error.empty());
  };
  const auto assertInvalid = [](ScoreProvenance value) {
    std::string error;
    assert(!serializeValidatedScoreProvenance(value, error).has_value());
    assert(!error.empty());
  };

  auto minimumPlayback = sampleVerifiedProvenance();
  minimumPlayback.playback.percent = 50;
  assertValid(minimumPlayback);

  auto maximumPlayback = sampleVerifiedProvenance();
  maximumPlayback.playback = {.percent = 200,
                              .mode = audio::PlaybackMode::TimeStretch};
  assertValid(maximumPlayback);

  auto invalidPlayback = sampleVerifiedProvenance();
  invalidPlayback.playback.percent = 49;
  assertInvalid(invalidPlayback);

  auto offStepPlayback = sampleVerifiedProvenance();
  offStepPlayback.playback.percent = 51;
  assertInvalid(offStepPlayback);

  auto invalidPlaybackMode = sampleVerifiedProvenance();
  invalidPlaybackMode.playback.mode = static_cast<audio::PlaybackMode>(99);
  assertInvalid(invalidPlaybackMode);

  auto invalidJudgeScale = sampleVerifiedProvenance();
  invalidJudgeScale.judgeWindowScalePercent = 20;
  assertInvalid(invalidJudgeScale);

  auto minimumJudgeScale = sampleVerifiedProvenance();
  minimumJudgeScale.judgeWindowScalePercent = 25;
  assertValid(minimumJudgeScale);

  auto maximumJudgeScale = sampleVerifiedProvenance();
  maximumJudgeScale.judgeWindowScalePercent = 200;
  assertValid(maximumJudgeScale);

  auto offStepJudgeScale = sampleVerifiedProvenance();
  offStepJudgeScale.judgeWindowScalePercent = 26;
  assertInvalid(offStepJudgeScale);

  auto invalidStartingGauge = sampleVerifiedProvenance();
  invalidStartingGauge.startingGaugePercent = 121;
  assertInvalid(invalidStartingGauge);
}

void testEligibilityClassification() {
  assert(sampleVerifiedProvenance().eligibility == ScoreEligibility::Verified);

  auto clubMode = sampleInput();
  clubMode.clubMode = true;
  assert(makeScoreProvenance(clubMode).eligibility ==
         ScoreEligibility::Verified);

  auto slowed = sampleInput();
  slowed.playback.percent = 75;
  assert(makeScoreProvenance(slowed).eligibility == ScoreEligibility::Modified);

  auto scaledJudge = sampleInput();
  scaledJudge.judgeWindowScalePercent = 80;
  assert(makeScoreProvenance(scaledJudge).eligibility ==
         ScoreEligibility::Verified);

  auto expandedJudge = sampleInput();
  expandedJudge.judgeWindowScalePercent = 120;
  assert(makeScoreProvenance(expandedJudge).eligibility ==
         ScoreEligibility::Modified);

  auto startingGauge = sampleInput();
  startingGauge.startingGaugePercent = 100;
  assert(makeScoreProvenance(startingGauge).eligibility ==
         ScoreEligibility::Modified);

  auto autoplay = sampleInput();
  autoplay.autoPlay = true;
  assert(makeScoreProvenance(autoplay).eligibility ==
         ScoreEligibility::Modified);

  auto practice = sampleInput();
  practice.practice = true;
  assert(makeScoreProvenance(practice).eligibility ==
         ScoreEligibility::Modified);

  auto constrained = sampleInput();
  constrained.judgeRankSource = JudgeRankSource::CourseConstraint;
  assert(makeScoreProvenance(constrained).eligibility ==
         ScoreEligibility::Verified);

  auto courseGauge = sampleInput();
  courseGauge.gaugeProfile = GaugeProfile::CourseDefault;
  assert(makeScoreProvenance(courseGauge).eligibility ==
         ScoreEligibility::Verified);

  auto unknownJudge = sampleInput();
  unknownJudge.judgeRankSource = JudgeRankSource::Unknown;
  assert(makeScoreProvenance(unknownJudge).eligibility ==
         ScoreEligibility::Modified);

  auto overridden = sampleInput();
  overridden.ruleset.scoringModel = "custom-scoring";
  assert(makeScoreProvenance(overridden).eligibility ==
         ScoreEligibility::Modified);

  auto assisted = sampleInput();
  assisted.assistOption = assist_options::kDrag;
  assert(makeScoreProvenance(assisted).eligibility ==
         ScoreEligibility::Modified);

  auto legacy = sampleInput();
  legacy.ruleset = RulesetDescriptor::Legacy();
  assert(makeScoreProvenance(legacy).eligibility ==
         ScoreEligibility::LegacyUnverified);
}

void testSignedWindowsAndCanonicalDevices() {
  const ScoreProvenance value = sampleVerifiedProvenance();
  assert(value.stages.size() == 1);
  const auto &windows = value.stages.front().effectiveJudgeWindows;
  assert(windows.size() == 5);
  assert(windows[0] == JudgeWindowProvenance(PGreat, -10000, 10000));
  assert(windows[1] == JudgeWindowProvenance(Great, -30000, 30000));
  assert(windows[2] == JudgeWindowProvenance(Good, -75000, 75000));
  assert(windows[3] == JudgeWindowProvenance(Bad, -330000, 420000));
  assert(windows[4] == JudgeWindowProvenance(Kpoor, -500000, 150000));

  assert((value.inputDevices == std::vector{InputDeviceCategory::Keyboard,
                                            InputDeviceCategory::Touch,
                                            InputDeviceCategory::Midi}));

  const std::string json = serializeScoreProvenance(value);
  assert(json.find("-330000") != std::string::npos);
  assert(json.find("420000") != std::string::npos);
}

void testFutureSchemaIsRejected() {
  std::string json = serializeScoreProvenance(sampleVerifiedProvenance());
  const std::string current =
      "\"schemaVersion\":" +
      std::to_string(ScoreProvenance::kSchemaVersion);
  const auto position = json.find(current);
  assert(position != std::string::npos);
  json.replace(position, current.size(), "\"schemaVersion\":4");

  std::string error;
  assert(!deserializeScoreProvenance(json, error).has_value());
  assert(error.find("future") != std::string::npos);
}

void testVersionOneMigratesToCurrentSchema() {
  const ScoreProvenance currentValue = sampleVerifiedProvenance();
  std::string json = serializeScoreProvenance(currentValue);
  const std::string current = "\"schemaVersion\":3";
  const auto position = json.find(current);
  assert(position != std::string::npos);
  json.replace(position, current.size(), "\"schemaVersion\":1");

  std::string error;
  const auto decoded = deserializeScoreProvenance(json, error);
  assert(error.empty());
  assert(decoded.has_value());
  assert(decoded->schemaVersion == ScoreProvenance::kSchemaVersion);
  assert(decoded == currentValue);

  json.replace(json.find("\"schemaVersion\":1"),
               std::string("\"schemaVersion\":1").size(),
               "\"schemaVersion\":0");
  assert(!deserializeScoreProvenance(json, error).has_value());
  assert(error.find("unsupported") != std::string::npos ||
         error.find("Unsupported") != std::string::npos);
}

void testCourseMergePreservesStagesAndWorstEligibility() {
  ScoreProvenance first = sampleVerifiedProvenance("first");
  ScoreProvenance second = sampleVerifiedProvenance("second");
  second.eligibility = ScoreEligibility::Modified;
  ScoreProvenance third = sampleVerifiedProvenance("third");
  third.eligibility = ScoreEligibility::LegacyUnverified;

  const std::array values{first, second, third};
  const ScoreProvenance merged = mergeCourseProvenance(values);
  assert(merged.stages.size() == 3);
  assert(merged.stages[0].chartSha256 == "sha256-first");
  assert(merged.stages[1].chartSha256 == "sha256-second");
  assert(merged.stages[2].chartSha256 == "sha256-third");
  assert(merged.eligibility == ScoreEligibility::Modified);

  const std::array modifiedValues{first, second};
  assert(mergeCourseProvenance(modifiedValues).eligibility ==
         ScoreEligibility::Modified);

  const std::array legacyValues{first, third};
  assert(mergeCourseProvenance(legacyValues).eligibility ==
         ScoreEligibility::LegacyUnverified);

  ScoreProvenance seededFirst = sampleVerifiedProvenance("seeded-first");
  ScoreProvenance seededSecond = sampleVerifiedProvenance("seeded-second");
  seededSecond.player1.seed = 67890;
  const std::array seededValues{seededFirst, seededSecond};
  assert(mergeCourseProvenance(seededValues).eligibility ==
         ScoreEligibility::Verified);
}

void testPlayStartCaptureIsImmutableAndShared() {
  bms_parser::ChartMeta meta;
  meta.MD5 = "attempt-md5";
  meta.SHA256 = "attempt-sha256";
  meta.Rank = 1;
  meta.RandomSeed = 91U;
  meta.RandomPrng = "mt19937";
  meta.RandomValues = {4, 2, 7};

  StartOptions options;
  options.gaugeType = GaugeType::Hard;
  options.gaugeProfile = GaugeProfile::Standard;
  options.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  options.playOption = "RANDOM";
  options.playOptionSeed = 1234;
  options.playOption2 = "MIRROR";
  options.longNoteMode = 2;
  options.playback = {.percent = 75, .mode = audio::PlaybackMode::PitchShift};
  options.judgeWindowScalePercent = 80;
  options.startingGaugePercent = 37;
  options.inputDeviceCategories = {InputDeviceCategory::Keyboard,
                                   InputDeviceCategory::Midi};
  options.rulesetDescriptor =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);

  Judge effectiveJudge(meta.Rank);
  const ScoreProvenance captured = captureScoreProvenanceAtPlayStart(
      options, meta, effectiveJudge.timingWindows);
  const std::string capturedJson = serializeScoreProvenance(captured);

  options.gaugeType = GaugeType::Easy;
  options.playOption = "NORMAL";
  options.playOptionSeed.reset();
  options.inputDeviceCategories = {InputDeviceCategory::Touch};
  options.rulesetDescriptor = RulesetDescriptor::Legacy();
  meta.MD5 = "mutated-md5";
  meta.SHA256 = "mutated-sha256";
  meta.RandomValues = {99};
  effectiveJudge.timingWindows[PGreat] = {-1, 1};

  assert(serializeScoreProvenance(captured) == capturedJson);
  assert(captured.stages.size() == 1);
  assert(captured.stages.front().chartSha256 == "attempt-sha256");
  assert(captured.stages.front().longNoteMode == 2);
  assert(captured.player1 ==
         PlayerOptionProvenance("RANDOM", std::int64_t{1234}));
  assert(captured.player2 == PlayerOptionProvenance("MIRROR", std::nullopt));
  assert(captured.playback == options.playback);
  assert(captured.judgeWindowScalePercent == 80);
  assert(captured.startingGaugePercent == 37);
  assert(captured.eligibility == ScoreEligibility::Modified);

  const ScoreProvenance scoreProvenance = captured;
  ReplayData replay;
  replay.provenance = captured;
  assert(scoreProvenance == replay.provenance);
}

void testReplayStartRestoresPracticeProvenance() {
  ReplayData replay;
  replay.provenance = sampleVerifiedProvenance("replay-start");
  replay.provenance.playback = {.percent = 75,
                                .mode = audio::PlaybackMode::PitchShift};
  replay.provenance.judgeWindowScalePercent = 80;
  replay.provenance.startingGaugePercent = 37;

  StartOptions options;
  options.playback = {.percent = 150, .mode = audio::PlaybackMode::PitchShift};
  applyReplayProvenanceToStartOptions(options, replay);
  assert(options.playback == replay.provenance.playback);
  assert(options.judgeWindowScalePercent == 80);
  assert(options.startingGaugePercent == 37);
}

void testResultRetryPlaybackAuthority() {
  ScoreProvenance attempt = sampleVerifiedProvenance("result-retry");
  attempt.playback = {.percent = 75, .mode = audio::PlaybackMode::PitchShift};

  const audio::PlaybackRate samePattern =
      resultRetryPlayback(attempt, std::nullopt);
  const audio::PlaybackRate newPattern =
      resultRetryPlayback(attempt, std::nullopt);
  assert(samePattern == attempt.playback);
  assert(newPattern == attempt.playback);

  practice::Configuration practiceConfiguration;
  practiceConfiguration.playback = {.percent = 125,
                                    .mode = audio::PlaybackMode::PitchShift};
  assert(resultRetryPlayback(attempt, practiceConfiguration) ==
         practiceConfiguration.playback);
}

std::vector<JudgeWindowProvenance> provenanceWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  std::vector<JudgeWindowProvenance> result;
  for (const auto &[judgement, window] : windows) {
    result.push_back({.judgement = judgement,
                      .earlyMicros = window.first,
                      .lateMicros = window.second});
  }
  return result;
}

ScoreStageProvenance replayStage(
    std::string sha256, std::string md5,
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  return {.chartMd5 = std::move(md5),
          .chartSha256 = std::move(sha256),
          .judgeRankSource = JudgeRankSource::Override,
          .effectiveJudgeWindows = provenanceWindows(windows)};
}

void testReplayUsesPersistedJudgeWindowsAsAuthority() {
  const std::string sha(64, 'a');
  const std::string md5(32, 'b');
  const std::map<Judgement, std::pair<long long, long long>> persisted = {
      {PGreat, {-1234, 5678}},  {Great, {-11111, 22222}},
      {Good, {-33333, 44444}},  {Bad, {-55555, 66666}},
      {Kpoor, {-77777, 88888}},
  };

  ReplayData replay;
  replay.chartMeta.SHA256 = sha;
  replay.chartMeta.MD5 = md5;
  replay.chartMeta.Rank = 0;
  replay.provenance = sampleVerifiedProvenance("replay-authority");
  replay.provenance.stages = {replayStage(sha, md5, persisted)};
  std::string error;
  const auto decoded = deserializeScoreProvenance(
      serializeScoreProvenance(replay.provenance), error);
  assert(error.empty());
  assert(decoded.has_value());
  replay.provenance = *decoded;

  StartOptions options;
  applyReplayProvenanceToStartOptions(options, replay);
  assert(options.replayJudgeOverride.has_value());

  bms_parser::ChartMeta currentMeta = replay.chartMeta;
  currentMeta.Rank = 3;
  const Judge restored = makeEffectiveJudgeAtPlayStart(options, currentMeta);
  assert(restored.timingWindows == persisted);
  assert(restored.timingWindows != Judge(currentMeta.Rank).timingWindows);
}

void testReplayJudgeOverrideValidatesChartAndWindows() {
  const std::string sha(64, 'c');
  const std::string md5(32, 'd');
  const auto persisted = sampleWindows();
  ReplayData replay;
  replay.chartMeta.SHA256 = sha;
  replay.chartMeta.MD5 = md5;
  replay.provenance = sampleVerifiedProvenance("replay-validation");
  replay.provenance.stages = {replayStage(sha, md5, persisted)};

  StartOptions options;
  applyReplayProvenanceToStartOptions(options, replay);
  assert(options.replayJudgeOverride.has_value());

  bms_parser::ChartMeta differentChart = replay.chartMeta;
  differentChart.SHA256 = std::string(64, 'e');
  differentChart.MD5 = std::string(32, 'f');
  differentChart.Rank = 2;
  const Judge fallback = makeEffectiveJudgeAtPlayStart(options, differentChart);
  assert(fallback.timingWindows == Judge(differentChart.Rank).timingWindows);

  replay.provenance.stages.front().effectiveJudgeWindows.pop_back();
  StartOptions incompleteOptions;
  applyReplayProvenanceToStartOptions(incompleteOptions, replay);
  assert(!incompleteOptions.replayJudgeOverride.has_value());
  const Judge incompleteFallback =
      makeEffectiveJudgeAtPlayStart(incompleteOptions, replay.chartMeta);
  assert(incompleteFallback.timingWindows ==
         Judge(replay.chartMeta.Rank).timingWindows);

  replay.provenance = ScoreProvenance::Legacy();
  StartOptions legacyOptions;
  applyReplayProvenanceToStartOptions(legacyOptions, replay);
  assert(!legacyOptions.replayJudgeOverride.has_value());
}

void testCourseReplaySelectsMatchingStageJudgeWindows() {
  const std::string firstSha(64, '1');
  const std::string secondSha(64, '2');
  const std::string firstMd5(32, '3');
  const std::string secondMd5(32, '4');
  const auto firstWindows = sampleWindows();
  const std::map<Judgement, std::pair<long long, long long>> secondWindows = {
      {PGreat, {-101, 102}}, {Great, {-201, 202}}, {Good, {-301, 302}},
      {Bad, {-401, 402}},    {Kpoor, {-501, 502}},
  };

  auto stageReplay = std::make_shared<ReplayData>();
  stageReplay->chartMeta.SHA256 = secondSha;
  stageReplay->chartMeta.MD5 = secondMd5;
  stageReplay->chartMeta.Rank = 3;
  stageReplay->provenance = sampleVerifiedProvenance("course-replay-stage");
  stageReplay->provenance.stages = {
      replayStage(firstSha, firstMd5, firstWindows),
      replayStage(secondSha, secondMd5, secondWindows),
  };

  auto session = std::make_shared<CoursePlaySession>();
  session->constraints.judgement = CourseJudgementConstraint::NoGood;
  const StartOptions options =
      makeCourseReplayStageStartOptions(session, stageReplay);
  assert(options.replayJudgeOverride.has_value());
  const Judge restored =
      makeEffectiveJudgeAtPlayStart(options, stageReplay->chartMeta);
  assert(restored.timingWindows == secondWindows);
}

void testMobilePlayStartInputCategoriesPreserveResolverClasses() {
  const std::vector resolverClasses = {
      input::DeviceClass::Joystick, input::DeviceClass::GameController,
      input::DeviceClass::Joystick, input::DeviceClass::Touch,
      input::DeviceClass::GameController};

  const auto categories = collectPlayStartInputDeviceCategories(
      resolverClasses, PlayStartInputPlatform::Mobile);

  assert(categories == std::vector({InputDeviceCategory::GameController,
                                    InputDeviceCategory::Joystick,
                                    InputDeviceCategory::Touch}));

  const std::vector nonTouchResolverClasses = {
      input::DeviceClass::GameController, input::DeviceClass::Joystick};
  assert(
      collectPlayStartInputDeviceCategories(nonTouchResolverClasses,
                                            PlayStartInputPlatform::Mobile) ==
      std::vector({InputDeviceCategory::GameController,
                   InputDeviceCategory::Joystick, InputDeviceCategory::Touch}));
}

void testPlayStartInputPlatformDefaultsAreIncluded() {
  const std::vector<input::DeviceClass> noResolverClasses;
  assert(collectPlayStartInputDeviceCategories(
             noResolverClasses, PlayStartInputPlatform::Mobile) ==
         std::vector({InputDeviceCategory::Touch}));
  assert(collectPlayStartInputDeviceCategories(
             noResolverClasses, PlayStartInputPlatform::Desktop) ==
         std::vector({InputDeviceCategory::Keyboard}));

  const std::vector controllerOnly = {input::DeviceClass::GameController};
  assert(collectPlayStartInputDeviceCategories(
             controllerOnly, PlayStartInputPlatform::Mobile) ==
         std::vector({InputDeviceCategory::GameController,
                      InputDeviceCategory::Touch}));
}

void testConstrainedPlayCapturesEffectiveWindowsAsVerified() {
  bms_parser::ChartMeta meta;
  meta.MD5 = "constrained-md5";
  meta.SHA256 = "constrained-sha256";
  meta.Rank = 2;

  StartOptions options;
  options.inputDeviceCategories = {InputDeviceCategory::Keyboard};
  options.courseConstraints.judgement = CourseJudgementConstraint::NoGood;

  Judge effectiveJudge(meta.Rank);
  const auto originalGood = effectiveJudge.timingWindows.at(Good);
  effectiveJudge.applyCourseJudgementConstraint(
      options.courseConstraints.judgement);
  const ScoreProvenance captured = captureScoreProvenanceAtPlayStart(
      options, meta, effectiveJudge.timingWindows);

  assert(captured.eligibility == ScoreEligibility::Verified);
  assert(captured.stages.size() == 1);
  const auto &stage = captured.stages.front();
  assert(stage.judgeRankSource == JudgeRankSource::CourseConstraint);
  assert(stage.sourceJudgeRank == meta.Rank);
  const auto good = std::ranges::find_if(
      stage.effectiveJudgeWindows, [](const JudgeWindowProvenance &window) {
        return window.judgement == Good;
      });
  const auto great = std::ranges::find_if(
      stage.effectiveJudgeWindows, [](const JudgeWindowProvenance &window) {
        return window.judgement == Great;
      });
  assert(good != stage.effectiveJudgeWindows.end());
  assert(great != stage.effectiveJudgeWindows.end());
  assert(std::pair(good->earlyMicros, good->lateMicros) != originalGood);
  assert(good->earlyMicros == great->earlyMicros);
  assert(good->lateMicros == great->lateMicros);
}

void testCourseSessionAggregatesRecordedStagesByIndex() {
  CoursePlaySession session;
  session.rulesetDescriptor =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);

  ScoreProvenance first = sampleVerifiedProvenance("course-first");
  ScoreProvenance second = sampleVerifiedProvenance("course-second");
  second.eligibility = ScoreEligibility::Modified;

  CoursePlaySession sparseSession;
  sparseSession.recordStageProvenance(1, second);
  assert(sparseSession.aggregateProvenance().eligibility ==
         ScoreEligibility::Modified);

  CoursePlaySession legacySparseSession;
  legacySparseSession.recordStageProvenance(1, first);
  assert(legacySparseSession.aggregateProvenance().eligibility ==
         ScoreEligibility::LegacyUnverified);

  session.recordStageProvenance(1, second);
  session.recordStageProvenance(0, first);

  const ScoreProvenance aggregate = session.aggregateProvenance();
  assert(aggregate.stages.size() == 2);
  assert(aggregate.stages[0].chartSha256 == "sha256-course-first");
  assert(aggregate.stages[1].chartSha256 == "sha256-course-second");
  assert(aggregate.eligibility == ScoreEligibility::Modified);

  StartOptions options;
  options.courseSession = std::make_shared<CoursePlaySession>();
  options.courseSession->rulesetDescriptor =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  options.courseSession->rulesetDescriptor.scoringModel = "course-override";
  options.inputDeviceCategories = {InputDeviceCategory::Keyboard};
  bms_parser::ChartMeta meta;
  meta.MD5 = "course-md5";
  meta.SHA256 = "course-sha256";
  meta.Rank = 1;
  Judge judge(meta.Rank);
  const ScoreProvenance captured =
      captureScoreProvenanceAtPlayStart(options, meta, judge.timingWindows);
  assert(captured.ruleset == options.courseSession->rulesetDescriptor);
  assert(captured.eligibility == ScoreEligibility::Modified);
}

} // namespace

int main() {
  testRulesetContract();
  testSchemaAndInputDeviceVocabularyContract();
  testDeterministicRoundTrip();
  testPlaybackAndJudgeProvenanceRoundTripAndMigration();
  testPlaybackAndJudgeProvenanceValidation();
  testEligibilityClassification();
  testSignedWindowsAndCanonicalDevices();
  testFutureSchemaIsRejected();
  testVersionOneMigratesToCurrentSchema();
  testCourseMergePreservesStagesAndWorstEligibility();
  testPlayStartCaptureIsImmutableAndShared();
  testReplayStartRestoresPracticeProvenance();
  testResultRetryPlaybackAuthority();
  testReplayUsesPersistedJudgeWindowsAsAuthority();
  testReplayJudgeOverrideValidatesChartAndWindows();
  testCourseReplaySelectsMatchingStageJudgeWindows();
  testMobilePlayStartInputCategoriesPreserveResolverClasses();
  testPlayStartInputPlatformDefaultsAreIncluded();
  testConstrainedPlayCapturesEffectiveWindowsAsVerified();
  testCourseSessionAggregatesRecordedStagesByIndex();
  std::cout << "score provenance tests passed\n";
  return 0;
}
