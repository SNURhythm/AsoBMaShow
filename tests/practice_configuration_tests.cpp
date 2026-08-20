#include "audio/PlaybackRate.h"
#include "bms_parser.hpp"
#include "practice/PracticeConfiguration.h"
#include "practice/PracticeSession.h"
#include "scene/ChartListenStart.h"

#include <iostream>
#include <ranges>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPlaybackRateConversions() {
  audio::PlaybackRate rate{.percent = 75,
                           .mode = audio::PlaybackMode::PitchShift};
  expect(rate.valid(), "75% pitch-shift playback is valid");
  expect(rate.chartMicrosFromReal(20'000) == 15'000,
         "real time converts to chart time exactly");
  expect(rate.realMicrosFromChart(15'000) == 20'000,
         "chart time converts to real time exactly");
  expect(!audio::PlaybackRate{.percent = 73}.valid(),
         "playback percentages must use five-percent steps");
  expect(!audio::PlaybackRate{.percent = 100,
                              .mode = static_cast<audio::PlaybackMode>(99)}
              .valid(),
         "unknown playback modes are invalid");
}

void testFreshCountInUsesChartMeasureSize() {
  expect(practice::defaultCountInBeatsForChart(3) == 3,
         "fresh 3/4 practice defaults to three count-in beats");
  expect(practice::defaultCountInBeatsForChart(4) == 4,
         "fresh 4/4 practice defaults to four count-in beats");
  expect(practice::defaultCountInBeatsForChart(0) == 4,
         "invalid chart measure size falls back to four beats");
}

void testSkinMenuUsesPinnedInitialPracticeViewport() {
  practice::Configuration configuration{
      .startMicros = 0,
      .endMicros = 90'000'000,
  };
  const auto menu = practice::buildSkinMenuState(
      configuration,
      {.lastTimelineMicros = 90'000'000,
       .judgeRank = 100,
       .chartTotal = 200.0,
       .keyMode = 7,
       .random1P = 0,
       .random2P = 0,
       .doublePlay = 0});
  expect(menu.items[0].available && menu.items[0].selected &&
             menu.items[0].label == "START TIME" &&
             menu.items[0].value == " 0:00.0" &&
             menu.items[9].available && menu.items[9].label == "OPTION-1P" &&
             !menu.items[10].available,
         "practice skin menu preserves the source ten-slot initial viewport");
}

void testSkinMenuInitialJudgeRankUsesPinnedBmsRuleConversion() {
  expect(practice::sourcePracticeJudgeRank(5, 2) == 75 &&
             practice::sourcePracticeJudgeRank(7, 3) == 100 &&
             practice::sourcePracticeJudgeRank(7, 5) == 75 &&
             practice::sourcePracticeJudgeRank(9, 2) == 70,
         "practice JUDGERANK converts BMS RANK through the pinned mode rule");
}

void testFiveKeyPracticeUsesTheNormalRandomOptionDomain() {
  practice::SkinMenuController menu(
      {.startMicros = 0, .endMicros = 5'000'000},
      {.lastTimelineMicros = 5'000'000,
       .judgeRank = 75,
       .chartTotal = 200.0,
       .keyMode = 5,
       .random1P = 0});

  expect(menu.changeVisibleItem(9, false),
         "five-key option row accepts a decrement event");
  expect(menu.skinMenuState().items[9].value == "S-RANDOM-EX",
         "five-key option row uses the normal ten-choice random domain");
}

void testSkinMenuJudgeRankUsesPinnedWindowRule() {
  const auto seven = practice::sourcePracticeJudgeRules(7, 50);
  const auto &sevenWindows =
      seven.contexts[static_cast<std::size_t>(gameplay::JudgeWindowContext::Normal)]
          .windows;
  const auto pms = practice::sourcePracticeJudgeRules(9, 50);
  const auto &pmsWindows =
      pms.contexts[static_cast<std::size_t>(gameplay::JudgeWindowContext::Normal)]
          .windows;
  expect(sevenWindows[0].earlyMicros == -10'000 &&
             sevenWindows[1].lateMicros == 30'000 &&
             sevenWindows[2].lateMicros == 75'000 &&
             sevenWindows[3].earlyMicros == -280'000 &&
             sevenWindows[4].lateMicros == 500'000 &&
             pmsWindows[0].lateMicros == 20'000 &&
             pmsWindows[1].lateMicros == 25'000 &&
             pmsWindows[2].lateMicros == 58'500,
         "practice JUDGERANK scales only the pinned adjustable windows and "
         "retains their source fixed bounds");
}

void testSkinMenuScrollUsesPinnedDoublePlayViewport() {
  practice::Configuration configuration{
      .startMicros = 0,
      .endMicros = 90'000'000,
  };
  const auto menu = practice::buildSkinMenuState(
      configuration,
      {.lastTimelineMicros = 90'000'000,
       .judgeRank = 100,
       .chartTotal = 200.0,
       .keyMode = 14,
       .random1P = 0,
       .random2P = 1,
       .doublePlay = 1},
      1.0F);
  expect(menu.itemScrollPosition == 1.0F && menu.items[0].available &&
             menu.items[0].label == "GAUGE TYPE" &&
             menu.items[9].available && menu.items[9].label == "OPTION-DP" &&
             menu.items[9].value == "FLIP",
         "practice scroll follows Beatoraja's two-row double-play viewport "
         "offset");
}

void testSkinMenuControllerRetainsSourceRowsAndActions() {
  practice::Configuration configuration{
      .startMicros = 0,
      .endMicros = 90'000'000,
      .gaugeType = GaugeType::Normal,
      .startingGaugePercent = 20,
      .playback = {.percent = 100, .mode = audio::PlaybackMode::PitchShift},
  };
  practice::SkinMenuInputs inputs{
      .lastTimelineMicros = 90'000'000,
      .judgeRank = 100,
      .chartTotal = 200.0,
      .keyMode = 14,
      .random1P = 0,
      .random2P = 0,
      .doublePlay = 0,
  };
  practice::SkinMenuController menu(configuration, inputs);

  auto state = menu.skinMenuState();
  expect(state.items[0].available && state.items[0].selected &&
             state.items[0].label == "START TIME" &&
             state.items[9].available && state.items[9].label == "OPTION-1P" &&
             !state.items[10].available,
         "source menu starts with its first ten double-play rows visible");

  menu.setItemScrollPosition(1.0F);
  state = menu.skinMenuState();
  expect(state.itemScrollPosition == 1.0F && state.items[0].selected &&
             state.items[0].label == "GAUGE TYPE" &&
             state.items[9].label == "OPTION-DP" &&
             state.items[9].value == "NORMAL",
         "source scroll selects the first visible row at the two-row maximum");

  expect(menu.changeVisibleItem(0, false),
         "source gauge row accepts a decrement event");
  state = menu.skinMenuState();
  expect(state.items[0].selected && state.items[0].value == "EASY",
         "source gauge event updates the selected scrolled row");

  expect(menu.changeVisibleItem(9, true),
         "source double-play row accepts an increment event");
  state = menu.skinMenuState();
  expect(state.items[9].selected && state.items[9].value == "FLIP",
         "source double-play event toggles the DP option and selects its row");
}

void testSkinMenuControllerUsesPinnedRowDomains() {
  practice::Configuration configuration{
      .startMicros = 0,
      .endMicros = 5'000'000,
      .gaugeType = GaugeType::Normal,
      .startingGaugePercent = 20,
      .playback = {.percent = 100, .mode = audio::PlaybackMode::PitchShift},
  };
  practice::SkinMenuInputs inputs{
      .lastTimelineMicros = 5'000'000,
      .judgeRank = 100,
      .chartTotal = 200.0,
      .keyMode = 9,
      .random1P = 0,
      .random2P = 0,
      .doublePlay = 0,
  };
  practice::SkinMenuController menu(configuration, inputs);

  expect(menu.changeVisibleItem(9, false),
         "source Pop'n option row accepts a decrement event");
  auto state = menu.skinMenuState();
  expect(state.items[9].value == "H-RANDOM",
         "source Pop'n option row wraps through seven random modes");

  expect(menu.changeVisibleItem(8, false),
         "source graph-type row accepts a decrement event");
  state = menu.skinMenuState();
  expect(state.items[8].value == "EARLYLATE",
         "source graph type decrements through its three-value ring");

  expect(menu.changeVisibleItem(7, false),
         "source frequency row accepts a decrement event");
  expect(menu.configuration().playback.percent == 95,
         "source non-turbo frequency changes in five-percent steps");

  expect(menu.changeVisibleItem(0, true),
         "source start-time row accepts an increment event");
  expect(menu.configuration().startMicros == 100'000,
         "source start time changes in one-hundred-millisecond steps");
  expect(menu.changeVisibleItem(1, false),
         "source end-time row accepts a decrement event");
  expect(menu.configuration().endMicros == 4'900'000,
         "source end time preserves its one-second minimum above start");
}

void testPracticeSessionUsesRetainedSkinMenuController() {
  practice::Configuration configuration{
      .startMicros = 0,
      .endMicros = 90'000'000,
  };
  practice::Session session(configuration);
  session.configureSkinMenu({.lastTimelineMicros = 90'000'000,
                             .judgeRank = 100,
                             .chartTotal = 200.0,
                             .keyMode = 14,
                             .random1P = 0,
                             .random2P = 0,
                             .doublePlay = 0});
  session.setSkinItemScrollPosition(1.0F);
  expect(session.changeSkinMenuVisibleItem(9, true),
         "practice session forwards a visible source row action");
  const auto state = session.skinMenuState();
  expect(state.items[9].selected && state.items[9].value == "FLIP",
         "practice session retains the controller's selected DP row");
}

void testSkinMenuPropertyProducesPinnedAttemptPlan() {
  practice::Configuration configuration{
      .startMicros = 2'000'000,
      .endMicros = 9'000'000,
      .gaugeType = GaugeType::Normal,
      .startingGaugePercent = 20,
      .playback = {.percent = 200, .mode = audio::PlaybackMode::PitchShift},
  };
  practice::SkinMenuController menu(
      configuration,
      {.lastTimelineMicros = 90'000'000,
       .judgeRank = 250,
       .chartTotal = 333.0,
       .keyMode = 14,
       .random1P = 0,
       .random2P = 0,
       .doublePlay = 0});

  menu.setItemScrollPosition(1.0F);
  for (int index = 0; index < 4; ++index) {
    expect(menu.changeVisibleItem(0, true),
           "source gauge row advances to the Grade gauge");
    if (index == 3) {
      break;
    }
    expect(menu.changeVisibleItem(1, true),
           "source gauge category row advances to LR2");
  }

  const auto attempt = practice::skinMenuAttemptPlan(menu.property());
  expect(attempt.startMicros == 1'000'000 &&
             attempt.endMicros == 4'500'000 &&
             attempt.gaugeType == GaugeType::Grade &&
             attempt.gaugeProfile == GaugeProfile::StandardLr2 &&
             attempt.startingGaugePercent == 100 && attempt.judgeRank == 250 &&
             attempt.total == 333.0 && attempt.playback.percent == 200,
         "source menu property retains frequency-scaled range and Grade/LR2 "
         "attempt authority");
}

void testSkinMenuAttemptPlanMovesOutOfRangeNotesToBackground() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 1;
  chart.Meta.TotalNotes = 3;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 300.0;
  auto *measure = new bms_parser::Measure();
  for (const long long timing : {0LL, 1'000'000LL, 2'000'000LL}) {
    auto *timeline = new bms_parser::TimeLine(1, false);
    timeline->Timing = timing;
    timeline->SetNote(0, new bms_parser::Note(1));
    measure->TimeLines.push_back(timeline);
  }
  chart.Measures.push_back(measure);

  practice::applySkinMenuPracticeModifier(
      chart, {.startMicros = 500'000,
              .endMicros = 1'500'000,
              .gaugeType = GaugeType::Normal,
              .total = 300.0});

  expect(measure->TimeLines[0]->Notes[0] == nullptr &&
             measure->TimeLines[0]->BackgroundNotes.size() == 1 &&
             measure->TimeLines[1]->Notes[0] != nullptr &&
             measure->TimeLines[2]->Notes[0] == nullptr &&
             measure->TimeLines[2]->BackgroundNotes.size() == 1 &&
             chart.Meta.TotalNotes == 1 && chart.Meta.Total == 100.0,
         "source practice modifier keeps only the selected range visible and "
         "scales NORMAL TOTAL by its remaining note count");
}

void testSkinMenuAttemptPlanMovesBothCrossingLongNoteEndpoints() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 2;
  chart.Meta.LnMode = 1;
  chart.Meta.TotalNotes = 2;
  chart.Meta.TotalLongNotes = 2;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 200.0;
  auto *measure = new bms_parser::Measure();
  for (const long long timing : {0LL, 1'000'000LL, 2'000'000LL,
                                 3'000'000LL}) {
    auto *timeline = new bms_parser::TimeLine(2, false);
    timeline->Timing = timing;
    measure->TimeLines.push_back(timeline);
  }

  auto *crossingStartHead = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::LongNote);
  auto *crossingStartTail = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::LongNote);
  crossingStartHead->Tail = crossingStartTail;
  crossingStartTail->Head = crossingStartHead;
  measure->TimeLines[0]->SetNote(0, crossingStartHead);
  measure->TimeLines[1]->SetNote(0, crossingStartTail);

  auto *crossingEndHead = new bms_parser::LongNote(
      2, bms_parser::LongNoteType::LongNote);
  auto *crossingEndTail = new bms_parser::LongNote(
      2, bms_parser::LongNoteType::LongNote);
  crossingEndHead->Tail = crossingEndTail;
  crossingEndTail->Head = crossingEndHead;
  measure->TimeLines[2]->SetNote(1, crossingEndHead);
  measure->TimeLines[3]->SetNote(1, crossingEndTail);
  chart.Measures.push_back(measure);

  practice::applySkinMenuPracticeModifier(
      chart, {.startMicros = 500'000,
              .endMicros = 2'500'000,
              .gaugeType = GaugeType::Normal,
              .total = 200.0});

  expect(measure->TimeLines[0]->Notes[0] == nullptr &&
             measure->TimeLines[1]->Notes[0] == nullptr &&
             measure->TimeLines[2]->Notes[1] == nullptr &&
             measure->TimeLines[3]->Notes[1] == nullptr &&
             measure->TimeLines[0]->BackgroundNotes.size() == 1 &&
             measure->TimeLines[1]->BackgroundNotes.size() == 1 &&
             measure->TimeLines[2]->BackgroundNotes.size() == 1 &&
             measure->TimeLines[3]->BackgroundNotes.size() == 1 &&
             chart.Meta.TotalNotes == 0 && chart.Meta.TotalLongNotes == 0 &&
             chart.Meta.Total == 0.0,
         "source practice modifier removes both endpoints when a long note "
         "crosses either range boundary");
}

void testSkinMenuPracticeModifierPreservesLandmineOnlyTotal() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 1;
  chart.Meta.TotalNotes = 0;
  chart.Meta.TotalLandmineNotes = 1;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 300.0;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->Timing = 1'000'000;
  timeline->SetLandmineNote(0, new bms_parser::LandmineNote(24.0F));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);

  practice::applySkinMenuPracticeModifier(
      chart, {.startMicros = 500'000,
              .endMicros = 1'500'000,
              .gaugeType = GaugeType::Normal,
              .total = 300.0});

  expect(chart.Meta.TotalNotes == 0 && chart.Meta.TotalLandmineNotes == 1 &&
             chart.Meta.Total == 300.0,
         "landmine-only practice keeps finite authored TOTAL when there are "
         "no playable notes to scale");
}

void testSkinMenuPracticeModifierRemovesOutOfRangeLandmines() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 1;
  chart.Meta.TotalLandmineNotes = 3;
  auto *measure = new bms_parser::Measure();
  for (const long long timing : {1'000'000LL, 2'000'000LL}) {
    auto *timeline = new bms_parser::TimeLine(1, false);
    timeline->Timing = timing;
    timeline->SetLandmineNote(0, new bms_parser::LandmineNote(24.0F));
    measure->TimeLines.push_back(timeline);
  }
  auto *legacyMineTimeline = new bms_parser::TimeLine(1, false);
  legacyMineTimeline->Timing = 3'000'000;
  legacyMineTimeline->SetNote(0, new bms_parser::LandmineNote(24.0F));
  measure->TimeLines.push_back(legacyMineTimeline);
  chart.Measures.push_back(measure);

  practice::applySkinMenuPracticeModifier(
      chart, {.startMicros = 500'000,
              .endMicros = 1'500'000,
              .gaugeType = GaugeType::Normal,
              .total = 300.0});

  expect(measure->TimeLines[0]->LandmineNotes[0] != nullptr &&
             measure->TimeLines[1]->LandmineNotes[0] == nullptr &&
             measure->TimeLines[2]->Notes[0] == nullptr &&
             measure->TimeLines[2]->BackgroundNotes.empty() &&
             chart.Meta.TotalLandmineNotes == 1,
         "source practice modifier discards every out-of-range landmine "
         "without turning legacy mine-channel notes into BGM");
}

void testSkinMenuDoublePlayFlipSwapsSourcePlayerHalves() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(16, false);
  auto *note = new bms_parser::Note(1);
  timeline->SetNote(0, note);
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);

  practice::applySkinMenuDoublePlayFlip(chart);

  expect(timeline->Notes[0] == nullptr && timeline->Notes[8] == note &&
             note->Lane == 8,
         "source PlayerFlipModifier rotates every double-play lane by one "
         "player half");
}

void testSkinMenuDoublePlayFlipSwapsLandminePlayerHalves() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(16, false);
  auto *mine = new bms_parser::LandmineNote(24.0F);
  timeline->SetLandmineNote(1, mine);
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);

  practice::applySkinMenuDoublePlayFlip(chart);

  expect(timeline->LandmineNotes[1] == nullptr &&
             timeline->LandmineNotes[9] == mine && mine->Lane == 9,
         "source PlayerFlipModifier rotates landmines with every other "
         "double-play lane");
}

void testSkinMenuShortChartDoesNotSelectNegativeStartTime() {
  practice::SkinMenuController menu(
      {.startMicros = 0, .endMicros = 1'000'000},
      {.lastTimelineMicros = 1'000'000,
       .judgeRank = 100,
       .chartTotal = 200.0,
       .keyMode = 7});

  expect(menu.changeVisibleItem(0, true),
         "short-chart START TIME accepts a source menu increment event");
  expect(menu.property().startTimeMillis == 0,
         "short-chart START TIME remains at the source zero minimum");
}

void testSessionUsesScaledRangeDuringRateAdjustedAttempt() {
  practice::Session session(
      {.startMicros = 2'000'000,
       .endMicros = 9'000'000,
       .playback = {.percent = 200,
                    .mode = audio::PlaybackMode::PitchShift}});
  const practice::SkinMenuInputs inputs{
      .lastTimelineMicros = 90'000'000,
      .judgeRank = 100,
      .chartTotal = 200.0,
      .keyMode = 7,
      .random1P = 0,
      .random2P = 0,
      .doublePlay = 0,
  };
  session.configureSkinMenu(inputs);
  const auto plan = session.beginSkinMenuAttempt();
  expect(plan.has_value() && plan->startMicros == 1'000'000 &&
             plan->endMicros == 4'500'000,
         "practice start returns the frequency-scaled chart range");
  expect(session.configuration().startMicros == 1'000'000 &&
             session.configuration().endMicros == 4'500'000 &&
             session.configuration().playback.percent == 200,
         "active practice runtime uses the frequency-scaled chart range");

  auto retry = session.freshForRetry();
  retry.configureSkinMenu(inputs);
  const auto retryPlan = retry.beginSkinMenuAttempt();
  expect(retryPlan.has_value() && retryPlan->startMicros == 1'000'000 &&
             retryPlan->endMicros == 4'500'000,
         "fresh practice retry rebuilds the scaled range from the raw menu "
         "coordinates without scaling twice");
  expect(retry.configuration().startMicros == 1'000'000 &&
             retry.configuration().endMicros == 4'500'000,
         "activating the retained retry plan publishes its scaled range to "
         "built-in practice startup");
}

void testFreshPracticeRetryRetainsSourceOnlyMenuChoices() {
  practice::Session session(
      {.startMicros = 2'000'000, .endMicros = 9'000'000});
  const practice::SkinMenuInputs inputs{
      .lastTimelineMicros = 90'000'000,
      .judgeRank = 100,
      .chartTotal = 200.0,
      .keyMode = 14,
      .random1P = 0,
      .random2P = 0,
      .doublePlay = 0,
  };
  session.configureSkinMenu(inputs);
  expect(session.changeSkinMenuVisibleItem(3, true),
         "practice gauge category accepts a source menu change");
  expect(session.changeSkinMenuVisibleItem(5, true),
         "practice judge rank accepts a source menu change");
  expect(session.changeSkinMenuVisibleItem(6, true),
         "practice total accepts a source menu change");
  session.setSkinItemScrollPosition(1.0F);
  expect(session.changeSkinMenuVisibleItem(9, true),
         "practice DP option accepts a source menu change");
  const auto before = session.beginSkinMenuAttempt();

  auto retry = session.freshForRetry();
  retry.configureSkinMenu(inputs);
  const auto after = retry.skinMenuAttemptPlan();

  expect(before.has_value() && after.has_value() &&
             before->gaugeProfile == after->gaugeProfile &&
             before->judgeRank == after->judgeRank &&
             before->total == after->total &&
             before->doublePlayFlip == after->doublePlayFlip,
         "fresh practice retry retains source-only menu attempt choices");
  expect(retry.hasActivatedSkinMenuAttempt(),
         "fresh practice retry remembers that its retained menu was accepted");
}

void testFreshPracticeRetryDistinguishesUnacceptedSkinMenu() {
  const practice::Configuration configuration{
      .startMicros = 2'000'000,
      .endMicros = 9'000'000,
      .gaugeAutoShift = GaugeAutoShiftMode::Continue,
      .judge = {.kind = practice::JudgeOverrideKind::Scale,
                .scalePercent = 75},
      .playback = {.percent = 200,
                   .mode = audio::PlaybackMode::PitchShift},
  };
  practice::Session session(configuration);
  session.configureSkinMenu({.lastTimelineMicros = 90'000'000,
                             .judgeRank = 100,
                             .chartTotal = 200.0,
                             .keyMode = 7});

  auto retry = session.freshForRetry();

  expect(!retry.hasActivatedSkinMenuAttempt(),
         "an unaccepted synthesized menu does not become retained retry "
         "authority");
  expect(retry.configuration() == configuration,
         "an unaccepted menu retry preserves panel range, judge, rate, and "
         "gauge settings");
}

void testListenUsesPracticeStartInsteadOfCursorOrEndMarker() {
  practice::Configuration configuration{
      .startMicros = 2'250'000,
      .endMicros = 9'500'000,
  };

  expect(chart_viewer_listen::resolveStartMicros(
             configuration, 9'500'000, practice::Marker::End) == 2'250'000,
         "Listen resolves the configured practice start when the end marker "
         "is active and the cursor is elsewhere");
  expect(configuration.startMicros == 2'250'000,
         "Listen start resolution preserves the exact visible start marker");
}

void testConfigurationSanitization() {
  practice::Configuration input{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 8'000'000,
      .endMicros = 2'000'000,
      .loop = true,
      .countInBeats = 99,
      .startingGaugePercent = 120,
      .judge = {.kind = practice::JudgeOverrideKind::Scale, .scalePercent = 17},
      .playback = {.percent = 73, .mode = audio::PlaybackMode::PitchShift},
  };
  const auto sanitized = practice::sanitize(input, 10'000'000);
  expect(sanitized.configuration.startMicros == 2'000'000,
         "crossed section markers are ordered");
  expect(sanitized.configuration.endMicros == 8'000'000,
         "ordered section retains the later marker");
  expect(sanitized.configuration.countInBeats == 16,
         "count-in is clamped to sixteen beats");
  expect(sanitized.configuration.startingGaugePercent == 120,
         "generic practice data preserves supported 120 percent gauges");
  expect(sanitized.configuration.judge.scalePercent == 25,
         "judge scale is clamped to twenty-five percent");
  expect(sanitized.configuration.playback.percent == 75,
         "playback rate is rounded to a five-percent step");
  expect(sanitized.playable(), "the sanitized ordered range is playable");
  expect(!sanitized.diagnostics.empty(),
         "configuration adjustments are reported diagnostically");

  const auto hardGaugeSanitized =
      practice::sanitize(input, 10'000'000, 100);
  expect(hardGaugeSanitized.configuration.startingGaugePercent == 100,
         "contextual sanitization clamps a 100 percent gauge correctly");
}

void testGaugeAutoShiftDropdownModel() {
  const auto autoShiftOptions = practice::practiceGaugeAutoShiftOptions();
  const auto bestClear =
      std::ranges::find(autoShiftOptions, std::string_view("best_clear"),
                        &practice::GaugeOption::id);
  expect(bestClear != autoShiftOptions.end() &&
             bestClear->label == "Best Clear" &&
             bestClear->gaugeAutoShift == GaugeAutoShiftMode::BestClear,
         "practice exposes Best Clear as an explicit auto-shift option");

  practice::Configuration configuration;
  expect(practice::applyPracticeGaugeOption(configuration, "4") &&
             configuration.gaugeType == GaugeType::ExHard &&
             practice::practiceGaugeOptionId(configuration) == "4",
         "practice gauge selection uses stable numeric gauge ids");
  expect(practice::applyPracticeGaugeAutoShiftOption(configuration,
                                                     "best_clear") &&
             configuration.gaugeAutoShift == GaugeAutoShiftMode::BestClear &&
             practice::practiceGaugeAutoShiftOptionId(configuration) ==
                 "best_clear",
         "practice auto shift is selected independently from the gauge");
  expect(practice::applyPracticeGaugeOption(configuration, "2") &&
             configuration.gaugeType == GaugeType::Normal &&
             configuration.gaugeAutoShift == GaugeAutoShiftMode::BestClear &&
             practice::practiceGaugeOptionId(configuration) == "2",
         "changing the gauge preserves the independent auto-shift mode");
  expect(!practice::applyPracticeGaugeOption(configuration, "not-a-gauge"),
         "unknown nonnumeric gauge ids are rejected without parsing");
}

void testGaugeAutoShiftSanitizationPreservesSelection() {
  practice::Configuration configuration{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 0,
      .endMicros = 1'000'000,
      .gaugeType = GaugeType::Normal,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
  };
  const auto sanitized = practice::sanitize(configuration, 1'000'000);
  expect(sanitized.configuration.gaugeType == GaugeType::Normal &&
             sanitized.configuration.gaugeAutoShift ==
                 GaugeAutoShiftMode::BestClear,
         "sanitization preserves independent gauge and auto-shift choices");
}

void testDefaultStartingGaugeMatchesEffectiveGauge() {
  practice::Configuration normal;
  normal.gaugeType = GaugeType::Normal;
  expect(practice::defaultStartingGaugePercent(
             normal, GaugeProfile::Standard) == 20,
         "standard Normal practice starts at twenty percent");
  expect(practice::defaultStartingGaugePercent(
             normal, GaugeProfile::Standard9Keys) == 30,
         "PMS Normal practice starts at thirty percent");

  practice::Configuration bestClear = normal;
  bestClear.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  expect(practice::defaultStartingGaugePercent(
             bestClear, GaugeProfile::Standard9Keys) == 100,
         "Best Clear displays its active Hazard default");
}

void testEmptyConfigurationIsNotPlayable() {
  practice::Configuration input{.startMicros = -10, .endMicros = 2'000'000};
  const auto sanitized = practice::sanitize(input, 0);
  expect(sanitized.configuration.startMicros == 0 &&
             sanitized.configuration.endMicros == 0,
         "markers clamp to an empty chart");
  expect(!sanitized.playable(), "an empty section cannot start practice");
}

void testPlayabilityIssuesExplainBlockingConfiguration() {
  practice::Configuration input{
      .chartSha256 = "0123456789abcdef0123456789abcdef"
                     "0123456789abcdef0123456789abcdef",
      .startMicros = 1'000'000,
      .endMicros = 5'000'000,
  };
  expect(!practice::firstPlayabilityIssue(input, 8'000'000),
         "a valid practice configuration has no blocking issue");

  const std::string validHash = input.chartSha256;
  input.chartSha256 = "invalid";
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Chart SHA-256 is unavailable or invalid.",
         "an invalid chart hash receives an identity explanation");
  input.chartSha256 = validHash;

  input.gaugeType = static_cast<GaugeType>(99);
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Gauge selection is invalid.",
         "an invalid gauge receives a selection explanation");
  input.gaugeType = GaugeType::Normal;

  input.endMicros = input.startMicros;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Practice range must be non-empty.",
         "an empty range receives an actionable explanation");
  input.endMicros = 5'000'000;
  input.judge.kind = practice::JudgeOverrideKind::Custom;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Custom judge windows are not available.",
         "custom judge windows receive an availability explanation");
  input.judge.kind = practice::JudgeOverrideKind::Scale;
  input.playback.mode = audio::PlaybackMode::TimeStretch;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Time Stretch is not available.",
         "time stretch receives an availability explanation");
  input.playback.mode = audio::PlaybackMode::PitchShift;
  input.playback.percent = 73;
  expect(practice::firstPlayabilityIssue(input, 8'000'000) ==
             "Playback rate must be 50-200% in 5% steps.",
         "invalid playback rate receives a bounds explanation");
}
} // namespace

int main() {
  testPlaybackRateConversions();
  testFreshCountInUsesChartMeasureSize();
  testSkinMenuInitialJudgeRankUsesPinnedBmsRuleConversion();
  testFiveKeyPracticeUsesTheNormalRandomOptionDomain();
  testSkinMenuJudgeRankUsesPinnedWindowRule();
  testSkinMenuUsesPinnedInitialPracticeViewport();
  testSkinMenuScrollUsesPinnedDoublePlayViewport();
  testSkinMenuControllerRetainsSourceRowsAndActions();
  testSkinMenuControllerUsesPinnedRowDomains();
  testPracticeSessionUsesRetainedSkinMenuController();
  testSkinMenuPropertyProducesPinnedAttemptPlan();
  testSkinMenuAttemptPlanMovesOutOfRangeNotesToBackground();
  testSkinMenuAttemptPlanMovesBothCrossingLongNoteEndpoints();
  testSkinMenuPracticeModifierPreservesLandmineOnlyTotal();
  testSkinMenuPracticeModifierRemovesOutOfRangeLandmines();
  testSkinMenuDoublePlayFlipSwapsSourcePlayerHalves();
  testSkinMenuDoublePlayFlipSwapsLandminePlayerHalves();
  testSkinMenuShortChartDoesNotSelectNegativeStartTime();
  testSessionUsesScaledRangeDuringRateAdjustedAttempt();
  testFreshPracticeRetryRetainsSourceOnlyMenuChoices();
  testFreshPracticeRetryDistinguishesUnacceptedSkinMenu();
  testListenUsesPracticeStartInsteadOfCursorOrEndMarker();
  testConfigurationSanitization();
  testGaugeAutoShiftDropdownModel();
  testGaugeAutoShiftSanitizationPreservesSelection();
  testDefaultStartingGaugeMatchesEffectiveGauge();
  testEmptyConfigurationIsNotPlayable();
  testPlayabilityIssuesExplainBlockingConfiguration();
  if (failures == 0) {
    std::cout << "practice configuration tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
