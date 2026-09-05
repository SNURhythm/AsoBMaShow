#include "music_select/MusicSelectEventController.h"

#include "music_select_runtime_ledger_assertions.h"

#include <algorithm>
#include <iostream>
#include <string_view>

namespace {

using Effect = MusicSelectEventEffectKind;
int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool has(const MusicSelectEventOutcome &outcome, Effect kind, int value = 0) {
  return std::ranges::any_of(outcome.effects, [&](const auto &effect) {
    return effect.kind == kind && effect.value == value;
  });
}

MusicSelectEventOutcome run(MusicSelectEventContext &context, int id,
                            int argument1 = 0, int argument2 = 0) {
  return MusicSelectEventController::execute(context, id, {}, argument1,
                                             argument2);
}

void testFilterAndSortEventsUseLiteralSourceOrders() {
  AppSettings settings;
  MusicSelectEventContext context{.settings = settings};

  settings.skinDifficultyFilterName = "ALL";
  auto result = run(context, 10);
  require(settings.skinDifficultyFilterName == "BEGINNER" &&
              result.settingsChanged && has(result, Effect::RefreshBars) &&
              has(result, Effect::OptionChangeSound),
          "difficulty advances in DifficultyFilter declaration order");
  settings.skinDifficultyFilterName = "ALL";
  (void)run(context, 10, -1);
  require(settings.skinDifficultyFilterName == "SPEED CHANGE CHART",
          "difficulty reverses across all nine source values");
  settings.skinDifficultyFilterName = "not-a-filter";
  (void)run(context, 10);
  require(settings.skinDifficultyFilterName == "BEGINNER",
          "invalid difficulty follows the source's declaration-length edge");

  settings.skinModeFilterName = "ALL";
  (void)run(context, 11, -1);
  require(settings.skinModeFilterName == "DOUBLE",
          "mode reverses across all ten declaration values");
  settings.skinModeFilterName = "not-a-mode";
  (void)run(context, 11);
  require(settings.skinModeFilterName == "7KEY",
          "invalid mode follows the source's declaration-length edge");

  context.sortIndex = 7;
  settings.skinSortId = "MISSCOUNT";
  (void)run(context, 12);
  require(context.sortIndex == 0 && settings.skinSortId == "TITLE",
          "default sort wraps its eight-entry source list");
  (void)run(context, 12, -1);
  require(context.sortIndex == 7 && settings.skinSortId == "MISSCOUNT",
          "default sort reverses through its eight-entry source list");

  settings.skinSortId = "MISSCOUNT";
  result = run(context, 312);
  require(settings.skinSortId == "DURATION" &&
              has(result, Effect::RefreshBars) &&
              has(result, Effect::OptionChangeSound),
          "song-bar sort continues into allSorter-only values");
  settings.skinSortId = "not-a-sort";
  result = run(context, 312);
  require(!result.settingsChanged && result.effects.empty(),
          "unknown song-bar sort is the source-defined exact no-op");
}

void testLaunchAndExternalEventsEmitSourceActions() {
  AppSettings settings;
  MusicSelectEventContext context{.settings = settings,
                                  .selectedSongHasPath = true};
  const auto keyconfig = run(context, 13);
  const auto skinconfig = run(context, 14);
  require(keyconfig.effects.size() == 1 && !keyconfig.settingsChanged &&
              skinconfig.effects.size() == 1 && !skinconfig.settingsChanged &&
              keyconfig.effects[0].kind == Effect::OpenSettings &&
              skinconfig.effects[0].kind == Effect::OpenSettings,
          "keyconfig and skinconfig stay separate controller cases, each "
          "emitting exactly the settings-open effect");
  require(has(run(context, 15), Effect::Play) &&
              has(run(context, 16), Effect::Autoplay) &&
              has(run(context, 315), Effect::Practice),
          "play events retain their three launch modes");
  require(has(run(context, 19), Effect::Replay, 0) &&
              has(run(context, 316), Effect::Replay, 1) &&
              has(run(context, 317), Effect::Replay, 2) &&
              has(run(context, 318), Effect::Replay, 3),
          "all four replay events retain their source slot");
  require(has(run(context, 17), Effect::OpenDocument) &&
              has(run(context, 210), Effect::OpenIr) &&
              has(run(context, 211), Effect::UpdateFolder) &&
              has(run(context, 212), Effect::OpenExplorer) &&
              has(run(context, 213), Effect::OpenDownloadSite),
          "OS and library events remain distinct host actions");

  auto favorite = run(context, 89, -1);
  require(has(favorite, Effect::ChangeFavoriteSong, -1) &&
              has(favorite, Effect::RefreshBars) &&
              has(favorite, Effect::OptionChangeSound),
          "favorite-song event carries direction and selector side effects");
  favorite = run(context, 90, 1);
  require(has(favorite, Effect::ChangeFavoriteChart, 1) &&
              has(favorite, Effect::RefreshBars) &&
              has(favorite, Effect::OptionChangeSound),
          "favorite-chart event carries direction and selector side effects");
  context.selectedSongHasPath = false;
  require(run(context, 89).effects.empty() && run(context, 90).effects.empty(),
          "favorite events are exact no-ops without a local selected song");
}

void testPlayerAndApplicationOptionsUseExactDomains() {
  AppSettings settings;
  MusicSelectEventContext context{.settings = settings,
                                  .hasSelectedPlayConfig = true};

  settings.selectedGaugeType = "hazard";
  (void)run(context, 40);
  require(settings.selectedGaugeType == "assisted_easy",
          "gauge wraps its six source values");
  (void)run(context, 40, -1);
  require(settings.selectedGaugeType == "hazard",
          "gauge reverses by five modulo six");

  settings.selectedPlayOption = "S-RANDOM-EX";
  (void)run(context, 42);
  require(settings.selectedPlayOption == "NORMAL",
          "1P random wraps all ten Beatoraja options");
  settings.skinPlayer2RandomOption = 0;
  (void)run(context, 43, -1);
  require(settings.skinPlayer2RandomOption == 9,
          "2P random reverses through ten values");
  settings.skinDoublePlayOption = 0;
  (void)run(context, 54, -1);
  require(settings.skinDoublePlayOption == 3,
          "DP option reverses through four values");

  settings.hispeedFixMode = AppSettings::HiSpeedFixMode::Min;
  (void)run(context, 55);
  require(settings.hispeedFixMode == AppSettings::HiSpeedFixMode::Off,
          "Hi-Speed fix wraps five source values");
  settings.gameplayHispeed = AppSettings::kMaxGameplayHispeed;
  require(!run(context, 57).settingsChanged,
          "Hi-Speed at the upper clamp is an exact no-op without sound");
  auto changed = run(context, 57, -1);
  require(settings.gameplayHispeed == 19.75F &&
              has(changed, Effect::OptionChangeSound),
          "Hi-Speed changes by the configured source margin");
  settings.visibleTimeDurationMilliseconds = 10'000;
  require(!run(context, 59, 1, 50).settingsChanged,
          "duration at the upper clamp is an exact no-op without sound");
  changed = run(context, 59, -1, 50);
  require(settings.visibleTimeDurationMilliseconds == 9'950 &&
              has(changed, Effect::OptionChangeSound),
          "duration uses positive arg2 as its exact increment");
  changed = run(context, 59, -1, 0);
  require(settings.visibleTimeDurationMilliseconds == 9'949,
          "non-positive duration arg2 uses the source increment of one");

  settings.skinBgaMode = 0;
  changed = run(context, 72, -1);
  require(settings.skinBgaMode == 2 && !settings.bgaEnabled &&
              has(changed, Effect::ApplyBgaEnabled, 0),
          "BGA cycles ON/AUTO/OFF and synchronizes native playback");
  changed = run(context, 72);
  require(settings.skinBgaMode == 0 && settings.bgaEnabled &&
              has(changed, Effect::ApplyBgaEnabled, 1),
          "re-enabling BGA resumes the live jukebox visual authority");
  settings.skinBgaExpandMode = 0;
  (void)run(context, 73, -1);
  require(settings.skinBgaExpandMode == 2 &&
              settings.bgaDisplayMode ==
                  AppSettings::BgaDisplayMode::NoExpand,
          "BGA expand OFF reaches the live no-expansion renderer mode");
  (void)run(context, 73);
  require(settings.skinBgaExpandMode == 0 &&
              settings.bgaDisplayMode == AppSettings::BgaDisplayMode::Stretch,
          "BGA expand FULL reaches the live stretch renderer mode");
  (void)run(context, 73);
  require(settings.skinBgaExpandMode == 1 &&
              settings.bgaDisplayMode == AppSettings::BgaDisplayMode::Fit,
          "BGA expand KEEP_ASPECT reaches the live fit renderer mode");

  settings.notesDisplayTimingMilliseconds = 500;
  require(!run(context, 74).settingsChanged,
          "display timing upper bound emits no option sound");
  changed = run(context, 74, -1);
  require(settings.notesDisplayTimingMilliseconds == 499 &&
              has(changed, Effect::OptionChangeSound),
          "display timing changes one millisecond inside source bounds");
  const bool oldAuto = settings.notesDisplayTimingAutoAdjust;
  (void)run(context, 75);
  require(settings.notesDisplayTimingAutoAdjust != oldAuto,
          "display timing auto-adjust toggles");

  settings.skinTargetList = {"A", "B", "C"};
  settings.skinTargetId = "A";
  changed = run(context, 77, -1);
  require(settings.skinTargetId == "C" && changed.effects.empty(),
          "target cycles configured targets without option sound");

  settings.selectedGaugeAutoShiftMode = "select_to_under";
  (void)run(context, 78);
  require(settings.selectedGaugeAutoShiftMode == "none",
          "gauge auto shift wraps five source values");
  settings.selectedGaugeAutoShiftLowerBound = "assisted_easy";
  (void)run(context, 341, -1);
  require(settings.selectedGaugeAutoShiftLowerBound == "normal",
          "bottom shiftable gauge reverses through three source values");
}

void testSelectedPlayConfigGuardsAndRemainingModifiers() {
  AppSettings settings;
  MusicSelectEventContext context{.settings = settings,
                                  .hasSelectedPlayConfig = false};
  const AppSettings before = settings;
  for (const int id : {55, 57, 59, 330, 331, 332, 340, 342, 400}) {
    require(run(context, id).effects.empty() && settings == before,
            "selected PlayConfig events are no-ops when the source returns null");
  }

  context.hasSelectedPlayConfig = true;
  settings.selectedLnMode = "LN";
  (void)run(context, 308, -1);
  require(settings.selectedLnMode == "HCN",
          "LN mode reverses across three source values");
  settings.autoSaveReplay = {10, 0, 0, 0};
  (void)run(context, 321);
  (void)run(context, 322, -1);
  require(settings.autoSaveReplay[0] == 0 && settings.autoSaveReplay[1] == 10,
          "autosave replay cycles all eleven source constraints");

  const bool laneCover = settings.laneCoverEnabled;
  const bool lift = settings.liftEnabled;
  const bool hidden = settings.hiddenEnabled;
  const bool autoHispeed = settings.hispeedAutoAdjust;
  (void)run(context, 330);
  (void)run(context, 331);
  (void)run(context, 332);
  (void)run(context, 342);
  require(settings.laneCoverEnabled != laneCover &&
              settings.liftEnabled != lift && settings.hiddenEnabled != hidden &&
              settings.hispeedAutoAdjust != autoHispeed,
          "selected PlayConfig boolean events toggle exact fields");

  settings.notePriorityMode = AppSettings::NotePriorityMode::Combo;
  (void)run(context, 340, -1);
  require(settings.notePriorityMode == AppSettings::NotePriorityMode::Lowest,
          "judge algorithm reverses in Combo/Duration/Lowest order");
  settings.notePriorityMode = AppSettings::NotePriorityMode::Score;
  require(!run(context, 340).settingsChanged &&
              settings.notePriorityMode == AppSettings::NotePriorityMode::Score,
          "Score judge algorithm is the source-defined exact no-op");

  settings.skinSortId = "RIVALOPTION";
  (void)run(context, 344);
  require(settings.skinSortId == "REPLAYCHART",
          "chart replication reproduces the pinned sortid mutation bug");
  settings.skinSortId = "TITLE";
  require(!run(context, 344).settingsChanged,
          "chart replication is an exact no-op unless sortid names a mode");

  settings.guideSoundEffects = false;
  settings.extraNoteDepth = 3;
  settings.mineMode = 4;
  settings.scrollMode = 2;
  settings.longNoteModifierMode = 5;
  settings.sevenToNinePattern = 6;
  settings.sevenToNineType = 2;
  settings.constantScroll = false;
  (void)run(context, 343);
  (void)run(context, 350);
  (void)run(context, 351);
  (void)run(context, 352);
  (void)run(context, 353);
  (void)run(context, 360);
  (void)run(context, 361);
  (void)run(context, 400);
  require(settings.guideSoundEffects && settings.extraNoteDepth == 0 &&
              settings.mineMode == 0 && settings.scrollMode == 0 &&
              settings.longNoteModifierMode == 0 &&
              settings.sevenToNinePattern == 0 && settings.sevenToNineType == 0 &&
              settings.constantScroll,
          "remaining modifier events use their literal source moduli");
}

void testRivalAndNameResolution() {
  AppSettings settings;
  MusicSelectEventContext context{.settings = settings,
                                  .rivalCount = 2,
                                  .currentRivalIndex = -1};
  auto result = run(context, 79);
  require(context.currentRivalIndex == 0 &&
              has(result, Effect::SetRival, 0) &&
              has(result, Effect::RefreshBars) &&
              has(result, Effect::OptionChangeSound),
          "rival advances from none to the first source rival");
  (void)run(context, 79, -1);
  require(context.currentRivalIndex == -1,
          "rival reverses from first rival to none");

  result = MusicSelectEventController::execute(context, -1, "option1p", -1,
                                                0);
  require(settings.selectedPlayOption == "S-RANDOM-EX",
          "event names resolve to the same source behavior as IDs");
  result = MusicSelectEventController::execute(context, 999'999, "unknown",
                                                0, 0);
  require(result.effects.empty() && !result.settingsChanged,
          "unknown event is delegated as an exact host no-op");
}

} // namespace

int main(int argc, char **argv) {
  testFilterAndSortEventsUseLiteralSourceOrders();
  testLaunchAndExternalEventsEmitSourceActions();
  testPlayerAndApplicationOptionsUseExactDomains();
  testSelectedPlayConfigGuardsAndRemainingModifiers();
  testRivalAndNameResolution();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_event_controller_tests", failures,
      "music-select event assertion(s) failed",
      "music-select event controller tests passed");
}
