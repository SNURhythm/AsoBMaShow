#include "ScoreProvenance.h"

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

} // namespace

int main() {
  testRulesetContract();
  testDeterministicRoundTrip();
  testEligibilityClassification();
  testSignedWindowsAndCanonicalDevices();
  testFutureSchemaIsRejected();
  testCourseMergePreservesStagesAndWorstEligibility();
  std::cout << "score provenance tests passed\n";
  return 0;
}
