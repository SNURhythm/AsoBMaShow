#include "ScoreProvenance.h"
#include "input/InputTypes.h"
#include "scene/play/GamePlayStartOptions.h"

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
  input.gaugeAutoShift = true;
  input.player1 = {.option = "RANDOM", .seed = 12345};
  input.player2 = {.option = "MIRROR", .seed = std::nullopt};
  input.assistOption = assist_options::kOff;
  input.inputDevices = {InputDeviceCategory::Touch,
                        InputDeviceCategory::Keyboard,
                        InputDeviceCategory::Touch, InputDeviceCategory::Midi};
  input.ruleset = RulesetDescriptor::Current();
  return input;
}

ScoreProvenance
sampleVerifiedProvenance(const std::string &hashSuffix = "one") {
  return makeScoreProvenance(sampleInput(hashSuffix));
}

void testRulesetContract() {
  const RulesetDescriptor rules = RulesetDescriptor::Current();
  assert(rules.version == 1);
  assert(rules.scoringModel == "asobmashow-v1");
  assert(rules.judgementModel == "bms-rank-v1");
  assert(rules.gaugeModel == "asobmashow-gauge-v1");

  const RulesetDescriptor legacy = RulesetDescriptor::Legacy();
  assert(legacy.version == 0);
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

void testEligibilityClassification() {
  assert(sampleVerifiedProvenance().eligibility == ScoreEligibility::Verified);

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
  const std::string current = "\"schemaVersion\":1";
  const auto position = json.find(current);
  assert(position != std::string::npos);
  json.replace(position, current.size(), "\"schemaVersion\":2");

  std::string error;
  assert(!deserializeScoreProvenance(json, error).has_value());
  assert(error.find("future") != std::string::npos);
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
  assert(merged.eligibility == ScoreEligibility::LegacyUnverified);

  const std::array modifiedValues{first, second};
  assert(mergeCourseProvenance(modifiedValues).eligibility ==
         ScoreEligibility::Modified);
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
  options.gaugeAutoShift = true;
  options.playOption = "RANDOM";
  options.playOptionSeed = 1234;
  options.playOption2 = "MIRROR";
  options.longNoteMode = 2;
  options.inputDeviceCategories = {InputDeviceCategory::Keyboard,
                                   InputDeviceCategory::Midi};
  options.rulesetDescriptor = RulesetDescriptor::Current();

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
  assert(captured.eligibility == ScoreEligibility::Verified);

  const ScoreProvenance scoreProvenance = captured;
  ReplayData replay;
  replay.provenance = captured;
  assert(scoreProvenance == replay.provenance);
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
  assert(collectPlayStartInputDeviceCategories(
             nonTouchResolverClasses, PlayStartInputPlatform::Mobile) ==
         std::vector({InputDeviceCategory::GameController,
                      InputDeviceCategory::Joystick}));
}

void testPlayStartInputFallbackIsUsedOnlyWhenResolverIsEmpty() {
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
         std::vector({InputDeviceCategory::GameController}));
}

void testConstrainedPlayCapturesEffectiveWindowsAsModified() {
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

  assert(captured.eligibility == ScoreEligibility::Modified);
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
  session.rulesetDescriptor = RulesetDescriptor::Current();

  ScoreProvenance first = sampleVerifiedProvenance("course-first");
  ScoreProvenance second = sampleVerifiedProvenance("course-second");
  second.eligibility = ScoreEligibility::Modified;

  CoursePlaySession sparseSession;
  sparseSession.recordStageProvenance(1, second);
  assert(sparseSession.aggregateProvenance().eligibility ==
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
  options.courseSession->rulesetDescriptor = RulesetDescriptor::Current();
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
  testDeterministicRoundTrip();
  testEligibilityClassification();
  testSignedWindowsAndCanonicalDevices();
  testFutureSchemaIsRejected();
  testCourseMergePreservesStagesAndWorstEligibility();
  testPlayStartCaptureIsImmutableAndShared();
  testMobilePlayStartInputCategoriesPreserveResolverClasses();
  testPlayStartInputFallbackIsUsedOnlyWhenResolverIsEmpty();
  testConstrainedPlayCapturesEffectiveWindowsAsModified();
  testCourseSessionAggregatesRecordedStagesByIndex();
  std::cout << "score provenance tests passed\n";
  return 0;
}
