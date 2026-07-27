#include "replay/ReplaySetupAuthority.h"

#include "replay/BeatorajaLongNoteMode.h"

#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using replay::setup_authority::Source;
using replay::setup_authority::Status;

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

struct Fixture {
  result_persistence::ChartScoreWrite score;
  replay::ChartPlaybackSetup setup;
  int keyMode = 14;
};

Fixture fixture() {
  Fixture result;
  result.score.chartPath = "sample/song.bms";
  result.score.chartMd5 = repeated('b', 32);
  result.score.chartSha256 = repeated('a', 64);
  result.score.longNoteMode = 2;

  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = result.score.chartMd5;
  input.chartMeta.SHA256 = result.score.chartSha256;
  input.chartMeta.KeyMode = result.keyMode;
  input.chartMeta.Rank = 1;
  input.chartMeta.TotalNotes = 5;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.chartMeta.RandomSeed = 42U;
  input.chartMeta.RandomPrng = "mt19937";
  input.chartMeta.RandomValues = {7, 3, 11};
  input.longNoteMode = result.score.longNoteMode;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-330'000, 420'000}},
      {Kpoor, {-500'000, 150'000}},
  };
  input.totalNotes = input.chartMeta.TotalNotes;
  input.authoredGaugeTotal = input.chartMeta.Total;
  input.effectiveGaugeTotal = input.chartMeta.Total;
  input.candidateSelection = gameplay::CandidateSelectionMode::Score;
  input.gaugeType = GaugeType::Hard;
  input.gaugeProfile = GaugeProfile::Standard;
  input.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  input.gaugeAutoShiftLowerBound = GaugeType::Easy;
  input.player1 = {.option = "RANDOM", .seed = 1234};
  input.player2 = {.option = "MIRROR", .seed = 5678};
  input.doublePlayOption = replay::DoublePlayOption::Flip;
  input.assistOption = assist_options::kDrag;
  input.playback = {.percent = 90,
                    .mode = audio::PlaybackMode::TimeStretch};
  input.judgeWindowScalePercent = 85;
  input.startingGaugePercent = 35;
  input.clubMode = true;
  input.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  result.score.provenance = makeScoreProvenance(input);

  result.setup.chartMd5 = result.score.chartMd5;
  result.setup.chartSha256 = result.score.chartSha256;
  result.setup.keyMode = result.keyMode;
  result.setup.longNoteMode = result.score.longNoteMode;
  result.setup.randomSeed = input.chartMeta.RandomSeed;
  result.setup.randomPrng = input.chartMeta.RandomPrng;
  result.setup.randomValues = input.chartMeta.RandomValues;
  result.setup.playOption = input.player1.option;
  result.setup.playOptionSeed = input.player1.seed;
  result.setup.playOption2 = input.player2.option;
  result.setup.playOption2Seed = input.player2.seed;
  result.setup.doublePlayOption = input.doublePlayOption;
  result.setup.assistOption = input.assistOption;
  result.setup.initialGaugeType = input.gaugeType;
  result.setup.gaugeProfile = input.gaugeProfile;
  result.setup.gaugeAutoShift = input.gaugeAutoShift;
  result.setup.gaugeAutoShiftLowerBound = input.gaugeAutoShiftLowerBound;
  result.setup.playbackRulesetId = input.ruleset.id;
  result.setup.playbackRulesetRevision = input.ruleset.version;
  result.setup.playbackRatePercent = input.playback.percent;
  result.setup.playbackMode = input.playback.mode;
  result.setup.candidateSelection = input.candidateSelection;
  result.setup.judgeWindowScalePercent = input.judgeWindowScalePercent;
  result.setup.startingGaugePercent =
      static_cast<float>(*input.startingGaugePercent);
  result.setup.clubMode = input.clubMode;
  result.setup.initialLaneCoverPercent = 31;
  result.setup.laneCoverEnabled = true;
  return result;
}

void testFullSetupMismatchMatrix() {
  struct Case {
    std::string_view name;
    std::string_view field;
    std::function<void(replay::ChartPlaybackSetup &)> mutate;
  };
  const std::vector<Case> cases = {
      {"chart MD5", "chart MD5", [](auto &v) { v.chartMd5[0] = 'c'; }},
      {"chart SHA-256", "chart SHA-256",
       [](auto &v) { v.chartSha256[0] = 'c'; }},
      {"key mode", "key mode", [](auto &v) { v.keyMode = 7; }},
      {"long-note mode", "long-note mode",
       [](auto &v) { v.longNoteMode = 3; }},
      {"initial gauge", "initial gauge",
       [](auto &v) { v.initialGaugeType = GaugeType::Easy; }},
      {"gauge profile", "gauge profile",
       [](auto &v) { v.gaugeProfile = GaugeProfile::Standard9Keys; }},
      {"gauge auto shift", "gauge auto shift",
       [](auto &v) { v.gaugeAutoShift = GaugeAutoShiftMode::None; }},
      {"gauge lower bound", "gauge auto-shift lower bound",
       [](auto &v) { v.gaugeAutoShiftLowerBound = GaugeType::Normal; }},
      {"player-one option", "player-one option",
       [](auto &v) { v.playOption = "MIRROR"; }},
      {"player-one seed", "player-one option seed",
       [](auto &v) { v.playOptionSeed = 4321; }},
      {"player-two option", "player-two option",
       [](auto &v) { v.playOption2 = "RANDOM"; }},
      {"player-two seed", "player-two option seed",
       [](auto &v) { v.playOption2Seed = 8765; }},
      {"double-play option", "double-play option",
       [](auto &v) { v.doublePlayOption = replay::DoublePlayOption::Normal; }},
      {"random values", "chart random values",
       [](auto &v) { v.randomValues[1] = 4; }},
      {"random seed", "chart random seed",
       [](auto &v) { v.randomSeed = 43U; }},
      {"random PRNG", "chart random PRNG",
       [](auto &v) { v.randomPrng = "xorshift"; }},
      {"assist", "assist option",
       [](auto &v) { v.assistOption = assist_options::kOff; }},
      {"ruleset ID", "ruleset ID",
       [](auto &v) { v.playbackRulesetId = "lr2"; }},
      {"ruleset revision", "ruleset revision",
       [](auto &v) { ++v.playbackRulesetRevision; }},
      {"playback percentage", "playback percentage",
       [](auto &v) { v.playbackRatePercent = 95; }},
      {"playback mode", "playback mode",
       [](auto &v) { v.playbackMode = audio::PlaybackMode::PitchShift; }},
      {"candidate selection", "candidate selection",
       [](auto &v) {
         v.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
       }},
      {"judge-window scale", "judge-window scale",
       [](auto &v) { v.judgeWindowScalePercent = 90; }},
      {"club mode", "club mode", [](auto &v) { v.clubMode = false; }},
      {"starting gauge", "starting gauge",
       [](auto &v) { v.startingGaugePercent = 36.0F; }},
  };

  for (const auto &test : cases) {
    auto value = fixture();
    test.mutate(value.setup);
    const auto outcome = replay::setup_authority::resolveForResult(
        value.setup, value.score, value.keyMode, Source::AsoExtension, false);
    expect(outcome.status == Status::Conflict && outcome.field == test.field &&
               !outcome.setup.has_value(),
           std::string("full setup mismatch rejects ") +
               std::string(test.name));
  }
}

void testStockEnrichmentAndOwnedFields() {
  auto value = fixture();
  auto stock = value.setup;
  stock.chartMd5.clear();
  stock.gaugeProfile = GaugeProfile::Standard;
  stock.gaugeAutoShift = GaugeAutoShiftMode::None;
  stock.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  stock.assistOption = assist_options::kOff;
  stock.playbackRulesetId.clear();
  stock.playbackRulesetRevision = 0;
  stock.playbackRatePercent = 100;
  stock.playbackMode = audio::PlaybackMode::PitchShift;
  stock.candidateSelection = gameplay::CandidateSelectionMode::Lowest;
  stock.judgeWindowScalePercent = 100;
  stock.startingGaugePercent = 20.0F;
  stock.clubMode = false;
  stock.randomSeed.reset();
  stock.randomPrng.reset();

  const auto outcome = replay::setup_authority::resolveForResult(
      stock, value.score, value.keyMode, Source::Stock, false);
  expect(outcome.resolved(), "stock setup resolves");
  if (!outcome.setup.has_value()) {
    return;
  }
  const auto &resolved = *outcome.setup;
  expect(resolved.chartMd5 == value.setup.chartMd5 &&
             resolved.longNoteMode == value.setup.longNoteMode &&
             resolved.gaugeAutoShift == value.setup.gaugeAutoShift &&
             resolved.gaugeAutoShiftLowerBound ==
                 value.setup.gaugeAutoShiftLowerBound &&
             resolved.assistOption == value.setup.assistOption &&
             resolved.playbackRulesetId == value.setup.playbackRulesetId &&
             resolved.playbackRulesetRevision ==
                 value.setup.playbackRulesetRevision &&
             resolved.playbackRatePercent ==
                 value.setup.playbackRatePercent &&
             resolved.playbackMode == value.setup.playbackMode &&
             resolved.candidateSelection ==
                 value.setup.candidateSelection &&
             resolved.judgeWindowScalePercent ==
                 value.setup.judgeWindowScalePercent &&
             resolved.clubMode == value.setup.clubMode &&
             resolved.randomSeed == value.setup.randomSeed &&
             resolved.randomPrng == value.setup.randomPrng &&
             resolved.doublePlayOption == value.setup.doublePlayOption,
         "stock setup is enriched only with result-owned missing fields");

  stock.doublePlayOption = replay::DoublePlayOption::Normal;
  const auto conflict = replay::setup_authority::resolveForResult(
      stock, value.score, value.keyMode, Source::Stock, false);
  expect(conflict.status == Status::Conflict &&
             conflict.field == "double-play option",
         "stock-owned DP still has to match result provenance");
}

void testLegacyAndPreviousSchemaAuthority() {
  auto value = fixture();
  value.score.provenance = ScoreProvenance::Legacy();
  value.setup.playOption = "S-RANDOM";
  value.setup.doublePlayOption = replay::DoublePlayOption::Normal;
  value.setup.startingGaugePercent = 73.0F;
  const auto legacy = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::AsoExtension, false);
  expect(legacy.resolved() && legacy.setup == value.setup,
         "legacy-unverified playback retains BRD setup authority");

  value = fixture();
  value.score.provenance.schemaVersion = 4;
  value.score.provenance.stages.front().doublePlayOption.reset();
  value.setup.doublePlayOption = replay::DoublePlayOption::Normal;
  const auto previous = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::AsoExtension, false);
  expect(previous.resolved(),
         "schema-four unknown DP does not invent a Normal comparison");
}

void testUnsupportedFullRulesetDescriptorIsInvalid() {
  auto value = fixture();
  value.score.provenance.ruleset.judgementModel =
      "future-judgement-model";

  const auto outcome = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::AsoExtension, false);

  expect(outcome.status == Status::Invalid &&
             outcome.field == "ruleset descriptor" &&
             !outcome.setup.has_value(),
         "matching ruleset ID and revision cannot hide an unsupported full "
         "descriptor");
}

void testCarriedGaugeAndInvalidEvidence() {
  auto value = fixture();
  value.score.provenance.startingGaugePercent.reset();
  value.setup.startingGaugePercent = 87.0F;
  const auto carried = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::AsoExtension, true);
  expect(carried.resolved() && carried.setup->startingGaugePercent == 87.0F,
         "later course stages may retain a carried gauge");
  const auto fresh = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::AsoExtension, false);
  expect(fresh.status == Status::Conflict &&
             fresh.field == "starting gauge",
         "fresh stages reject a carried gauge");

  value = fixture();
  value.score.provenance.stages.front().chartRandomSeed =
      std::numeric_limits<std::uint64_t>::max();
  value.setup.randomSeed.reset();
  const auto overflow = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::Stock, false);
  expect(overflow.status == Status::Invalid &&
             overflow.field == "chart random seed",
         "unrepresentable provenance random seed is invalid evidence");

  value = fixture();
  value.score.provenance = ScoreProvenance::Legacy();
  value.score.longNoteMode = 99;
  value.setup.longNoteMode = 98;
  const auto invalidLongNote = replay::setup_authority::resolveForResult(
      value.setup, value.score, value.keyMode, Source::Stock, false);
  expect(invalidLongNote.status == Status::Invalid &&
             invalidLongNote.field == "long-note mode",
         "invalid stock long-note modes cannot match through null optionals");
}

} // namespace

int main() {
  testFullSetupMismatchMatrix();
  testStockEnrichmentAndOwnedFields();
  testLegacyAndPreviousSchemaAuthority();
  testUnsupportedFullRulesetDescriptorIsInvalid();
  testCarriedGaugeAndInvalidEvidence();
  if (failures != 0) {
    std::cerr << failures << " replay setup authority test(s) failed\n";
    return 1;
  }
  std::cout << "replay setup authority tests passed\n";
  return 0;
}
