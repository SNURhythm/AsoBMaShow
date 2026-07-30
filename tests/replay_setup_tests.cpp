#include "replay/ReplaySetup.h"

#include <array>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

replay::ReplaySetup validSetup() {
  return {
      .chart =
          {
              .md5 = std::string(32, 'b'),
              .sha256 = std::string(64, 'a'),
              .keyMode = 14,
          },
      .longNoteMode = 2,
      .hasUndefinedLongNotes = true,
      .chartRandomSeed = 42,
      .chartRandomPrng = "std::mt19937_64",
      .chartRandomValues = {7, 3, 11},
      .player1 = {.option = "RANDOM", .seed = 1234},
      .player2 = {.option = "MIRROR", .seed = 5678},
      .doublePlayOption = replay::DoublePlayOption::Flip,
      .assistOption = "DRAG",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja),
      .playback = {.percent = 90, .mode = audio::PlaybackMode::TimeStretch},
      .candidateSelection = gameplay::CandidateSelectionMode::Score,
      .judgeWindowScalePercent = 85,
      .startingGaugePercent = 35.0F,
      .initialLaneCoverPercent = 31,
      .laneCoverEnabled = true,
      .clubMode = true,
  };
}

void testValidCapturedAndStockSetups() {
  const auto setup = validSetup();
  expect(replay::validateReplaySetup(setup,
                                     replay::ReplaySetupSource::LocalCapture)
             .valid(),
         "full local setup is valid");
  expect(replay::validateReplaySetup(setup,
                                     replay::ReplaySetupSource::AsoExtension)
             .valid(),
         "full Aso extension setup is valid");

  auto stock = setup;
  stock.chart.md5.clear();
  expect(replay::validateReplaySetup(stock,
                                     replay::ReplaySetupSource::StockBeatoraja)
             .valid(),
         "stock setup may omit MD5");
  expect(replay::validateReplaySetup(stock,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::ChartMd5,
         "local capture cannot omit MD5");

  stock.ruleset = RulesetDescriptor::Current();
  expect(replay::validateReplaySetup(stock,
                                     replay::ReplaySetupSource::StockBeatoraja)
                 .issue == replay::ReplaySetupIssue::Ruleset,
         "stock setup identifies Beatoraja gameplay authority");
}

void testUnknownSetupSourceFailsClosed() {
  expect(replay::validateReplaySetup(
             validSetup(), static_cast<replay::ReplaySetupSource>(255))
                 .issue == replay::ReplaySetupIssue::Source,
         "unknown setup source cannot inherit local or stock authority");

  replay::ReplayLimits invalidLimits = replay::kReplayLimits;
  invalidLimits.maxStringBytes = 0;
  expect(replay::validateReplaySetup(validSetup(),
                                     replay::ReplaySetupSource::LocalCapture,
                                     invalidLimits)
                 .issue == replay::ReplaySetupIssue::Limits,
         "invalid limit policy cannot validate setup");
}

void testSupportedKeyModesAndDoublePlayOption() {
  constexpr std::array keyModes{4, 5, 6, 7, 8, 9, 10, 14, 24, 48};
  for (int keyMode : keyModes) {
    auto setup = validSetup();
    setup.chart.keyMode = keyMode;
    setup.doublePlayOption = replay::DoublePlayOption::Normal;
    expect(replay::validateReplaySetup(setup,
                                       replay::ReplaySetupSource::LocalCapture)
               .valid(),
           "demonstrated Beatoraja key mode is accepted");
  }

  for (const int keyMode : {1, 3, 11, 49, 127}) {
    auto custom = validSetup();
    custom.chart.keyMode = keyMode;
    custom.doublePlayOption = replay::DoublePlayOption::Normal;
    expect(replay::validateReplaySetup(
               custom, replay::ReplaySetupSource::LocalCapture)
                   .issue == replay::ReplaySetupIssue::KeyMode,
           "positive key counts without a stock BRD layout keep replay "
           "optional");
  }

  auto single = validSetup();
  single.chart.keyMode = 7;
  expect(replay::validateReplaySetup(single,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::DoublePlayOption,
         "FLIP requires a supported double-play key mode");
}

void testExactLaneShufflePatterns() {
  struct Case {
    int keyMode;
    std::size_t lanesPerPlayer;
  };
  constexpr std::array cases{
      Case{5, 6},  Case{7, 8},   Case{9, 9},   Case{10, 6},
      Case{14, 8}, Case{24, 26}, Case{48, 26},
  };
  for (const auto test : cases) {
    auto setup = validSetup();
    setup.chart.keyMode = test.keyMode;
    setup.doublePlayOption = replay::DoublePlayOption::Normal;
    setup.player1.laneShufflePattern.emplace(test.lanesPerPlayer);
    for (std::size_t index = 0; index < test.lanesPerPlayer; ++index) {
      (*setup.player1.laneShufflePattern)[index] =
          static_cast<int>(test.lanesPerPlayer - index - 1);
    }
    if (test.keyMode == 10 || test.keyMode == 14 || test.keyMode == 48) {
      setup.player2.laneShufflePattern = setup.player1.laneShufflePattern;
    }
    expect(replay::validateReplaySetup(
               setup, replay::ReplaySetupSource::StockBeatoraja)
               .valid(),
           "exact stock lane-shuffle permutation is retained");
  }

  auto invalid = validSetup();
  invalid.player1.laneShufflePattern = std::vector<int>(7, 0);
  expect(replay::validateReplaySetup(invalid,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::LaneShufflePattern,
         "wrong-sized or duplicate lane pattern is rejected");

  invalid = validSetup();
  invalid.player1.laneShufflePattern = {0, 1, 2, 3, 4, 5, 6, 8};
  expect(replay::validateReplaySetup(invalid,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::LaneShufflePattern,
         "out-of-range lane pattern entry is rejected");
}

void testUndefinedLongNoteContract() {
  auto noLongNotes = validSetup();
  noLongNotes.longNoteMode = 0;
  noLongNotes.hasUndefinedLongNotes = false;
  expect(replay::validateReplaySetup(noLongNotes,
                                     replay::ReplaySetupSource::LocalCapture)
             .valid(),
         "mode zero is valid when no undefined long notes need interpretation");

  noLongNotes.hasUndefinedLongNotes = true;
  expect(replay::validateReplaySetup(noLongNotes,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::LongNoteMode,
         "undefined long notes require an effective LN/CN/HCN mode");
}

void testAttemptSetupPercentagesMatchExistingCaptureContract() {
  for (int percent : {25, 30, 100, 195, 200}) {
    auto setup = validSetup();
    setup.judgeWindowScalePercent = percent;
    expect(replay::validateReplaySetup(setup,
                                       replay::ReplaySetupSource::LocalCapture)
               .valid(),
           "capturable stepped judge scale is replay-valid");
  }
  for (int percent : {20, 26, 205}) {
    auto setup = validSetup();
    setup.judgeWindowScalePercent = percent;
    expect(replay::validateReplaySetup(setup,
                                       replay::ReplaySetupSource::LocalCapture)
                   .issue == replay::ReplaySetupIssue::JudgeWindowScale,
           "uncapturable judge scale is replay-invalid");
  }

  auto maximumGauge = validSetup();
  maximumGauge.startingGaugePercent = 120.0F;
  expect(replay::validateReplaySetup(maximumGauge,
                                     replay::ReplaySetupSource::LocalCapture)
             .valid(),
         "existing 120-percent gauge maximum is replay-valid");
  maximumGauge.startingGaugePercent = 120.01F;
  expect(replay::validateReplaySetup(maximumGauge,
                                     replay::ReplaySetupSource::LocalCapture)
                 .issue == replay::ReplaySetupIssue::StartingGauge,
         "gauge start above the shared maximum is replay-invalid");
}

void testInvalidFieldMatrix() {
  struct Case {
    replay::ReplaySetupIssue issue;
    std::function<void(replay::ReplaySetup &)> mutate;
  };
  const std::vector<Case> cases{
      {replay::ReplaySetupIssue::ChartSha256,
       [](auto &v) { v.chart.sha256[0] = 'A'; }},
      {replay::ReplaySetupIssue::ChartMd5,
       [](auto &v) { v.chart.md5.pop_back(); }},
      {replay::ReplaySetupIssue::LongNoteMode,
       [](auto &v) { v.longNoteMode = 4; }},
      {replay::ReplaySetupIssue::RandomValues,
       [](auto &v) { v.chartRandomValues.resize(100'001); }},
      {replay::ReplaySetupIssue::RandomPrng,
       [](auto &v) { v.chartRandomPrng = "unknown"; }},
      {replay::ReplaySetupIssue::PlayerOneOption,
       [](auto &v) { v.player1.option = "random"; }},
      {replay::ReplaySetupIssue::PlayerTwoOption,
       [](auto &v) { v.player2.seed = -1; }},
      {replay::ReplaySetupIssue::PlayerOptions,
       [](auto &v) {
         v.player1.option = "ASSIGN:L123456789ABCDER";
         v.player2.option = "RANDOM";
       }},
      {replay::ReplaySetupIssue::AssistOption,
       [](auto &v) { v.assistOption = "drag"; }},
      {replay::ReplaySetupIssue::GaugeType,
       [](auto &v) { v.initialGaugeType = static_cast<GaugeType>(99); }},
      {replay::ReplaySetupIssue::GaugeProfile,
       [](auto &v) { v.gaugeProfile = static_cast<GaugeProfile>(99); }},
      {replay::ReplaySetupIssue::GaugeAutoShift,
       [](auto &v) { v.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(99); }},
      {replay::ReplaySetupIssue::GaugeAutoShiftLowerBound,
       [](auto &v) {
         v.gaugeAutoShiftLowerBound = static_cast<GaugeType>(99);
       }},
      {replay::ReplaySetupIssue::Ruleset,
       [](auto &v) { v.ruleset.id = "future"; }},
      {replay::ReplaySetupIssue::PlaybackRate,
       [](auto &v) { v.playback.percent = 91; }},
      {replay::ReplaySetupIssue::CandidateSelection,
       [](auto &v) {
         v.candidateSelection =
             static_cast<gameplay::CandidateSelectionMode>(99);
       }},
      {replay::ReplaySetupIssue::JudgeWindowScale,
       [](auto &v) { v.judgeWindowScalePercent = 20; }},
      {replay::ReplaySetupIssue::StartingGauge,
       [](auto &v) { v.startingGaugePercent = 121.0F; }},
      {replay::ReplaySetupIssue::InitialLaneCover,
       [](auto &v) { v.initialLaneCoverPercent = 101; }},
  };

  for (const auto &test : cases) {
    auto setup = validSetup();
    test.mutate(setup);
    expect(replay::validateReplaySetup(setup,
                                       replay::ReplaySetupSource::LocalCapture)
                   .issue == test.issue,
           "invalid setup field reports its canonical issue");
  }
}

void testChartIdentityAgreementUsesParsedIdentity() {
  const auto recorded = validSetup().chart;
  expect(replay::compareReplayChartIdentity(recorded, recorded) ==
             replay::ReplayChartMatch::Match,
         "identical parsed and recorded identity matches");

  auto selected = recorded;
  selected.sha256[0] = 'c';
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::Sha256Mismatch,
         "SHA-256 mismatch has highest identity priority");

  selected = recorded;
  selected.md5[0] = 'c';
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::Md5Mismatch,
         "present recorded MD5 must match parsed content");

  selected = recorded;
  selected.keyMode = 7;
  expect(replay::compareReplayChartIdentity(recorded, selected) ==
             replay::ReplayChartMatch::KeyModeMismatch,
         "selected key mode cannot be overwritten by replay metadata");

  auto stock = recorded;
  stock.md5.clear();
  expect(replay::compareReplayChartIdentity(stock, recorded) ==
             replay::ReplayChartMatch::Match,
         "stock identity without MD5 is matched by SHA-256 and key mode");
}

} // namespace

int main() {
  testValidCapturedAndStockSetups();
  testUnknownSetupSourceFailsClosed();
  testSupportedKeyModesAndDoublePlayOption();
  testExactLaneShufflePatterns();
  testUndefinedLongNoteContract();
  testAttemptSetupPercentagesMatchExistingCaptureContract();
  testInvalidFieldMatrix();
  testChartIdentityAgreementUsesParsedIdentity();
  if (failures != 0) {
    std::cerr << failures << " replay setup test(s) failed\n";
    return 1;
  }
  std::cout << "replay setup tests passed\n";
  return 0;
}
