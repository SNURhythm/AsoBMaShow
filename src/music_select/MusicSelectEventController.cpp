#include "MusicSelectEventController.h"

#include "../replay/ReplayOption.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>

namespace {
using Effect = MusicSelectEventEffectKind;

constexpr std::array<std::string_view, 9> kDifficulties{
    "ALL",     "BEGINNER",          "NORMAL",
    "HYPER",   "ANOTHER",           "INSANE",
    "SCRATCH CHART", "LONG NOTE CHART", "SPEED CHANGE CHART"};
constexpr std::array<std::string_view, 10> kModes{
    "ALL", "7KEY", "14KEY", "9KEY", "5KEY",
    "10KEY", "24KEY", "48KEY", "SINGLE", "DOUBLE"};
constexpr std::array<std::string_view, 8> kDefaultSorts{
    "TITLE", "ARTIST", "BPM", "LENGTH",
    "LEVEL", "CLEAR", "SCORE", "MISSCOUNT"};
constexpr std::array<std::string_view, 12> kAllSorts{
    "TITLE", "ARTIST", "BPM", "LENGTH", "LEVEL", "CLEAR",
    "SCORE", "MISSCOUNT", "DURATION", "LASTUPDATE",
    "RIVALCOMPARE_CLEAR", "RIVALCOMPARE_SCORE"};
constexpr std::array<std::string_view, 6> kGauges{
    "assisted_easy", "easy", "normal", "hard", "exhard", "hazard"};
constexpr std::array<std::string_view, 5> kGaugeAutoShift{
    "none", "continue", "survival_to_groove", "best_clear",
    "select_to_under"};
constexpr std::array<std::string_view, 3> kBottomShiftableGauge{
    "assisted_easy", "easy", "normal"};
constexpr std::array<std::string_view, 3> kLnModes{"LN", "CN", "HCN"};
constexpr std::array<std::string_view, 5> kChartReplicationModes{
    "NONE", "RIVALCHART", "RIVALOPTION", "REPLAYCHART", "REPLAYOPTION"};

template <std::size_t Size>
std::size_t sourceNextIndex(const std::array<std::string_view, Size> &values,
                            std::string_view current, int argument) {
  const auto found = std::ranges::find(values, current);
  const auto index = static_cast<std::size_t>(std::distance(values.begin(),
                                                            found));
  return (index + (argument >= 0 ? 1 : Size - 1)) % Size;
}

int cycleInteger(int value, int length, int argument) {
  return (value + (argument >= 0 ? 1 : length - 1)) % length;
}

void effect(MusicSelectEventOutcome &outcome, Effect kind, int value = 0) {
  outcome.effects.push_back({.kind = kind, .value = value});
}

void changedWithSound(MusicSelectEventOutcome &outcome) {
  outcome.settingsChanged = true;
  effect(outcome, Effect::OptionChangeSound);
}

void refreshWithSound(MusicSelectEventOutcome &outcome) {
  outcome.settingsChanged = true;
  effect(outcome, Effect::RefreshBars);
  effect(outcome, Effect::OptionChangeSound);
}

int eventIdForName(std::string_view name) {
  constexpr std::array names{
      std::pair{"difficulty", 10},
      std::pair{"mode", 11},
      std::pair{"sort", 12},
      std::pair{"keyconfig", 13},
      std::pair{"skinconfig", 14},
      std::pair{"play", 15},
      std::pair{"autoplay", 16},
      std::pair{"open_document", 17},
      std::pair{"replay1", 19},
      std::pair{"gauge1p", 40},
      std::pair{"option1p", 42},
      std::pair{"option2p", 43},
      std::pair{"optiondp", 54},
      std::pair{"hsfix", 55},
      std::pair{"hispeed1p", 57},
      std::pair{"duration1p", 59},
      std::pair{"bga", 72},
      std::pair{"bgaexpand", 73},
      std::pair{"notesdisplaytiming", 74},
      std::pair{"notesdisplaytimingautoadjust", 75},
      std::pair{"target", 77},
      std::pair{"gaugeautoshift", 78},
      std::pair{"rival", 79},
      std::pair{"favorite_song", 89},
      std::pair{"favorite_chart", 90},
      std::pair{"open_ir", 210},
      std::pair{"update_folder", 211},
      std::pair{"open_with_explorer", 212},
      std::pair{"open_download_site", 213},
      std::pair{"lnmode", 308},
      std::pair{"songbar_sort", 312},
      std::pair{"practice", 315},
      std::pair{"replay2", 316},
      std::pair{"replay3", 317},
      std::pair{"replay4", 318},
      std::pair{"autosavereplay1", 321},
      std::pair{"autosavereplay2", 322},
      std::pair{"autosavereplay3", 323},
      std::pair{"autosavereplay4", 324},
      std::pair{"lanecover", 330},
      std::pair{"lift", 331},
      std::pair{"hidden", 332},
      std::pair{"judgealgorithm", 340},
      std::pair{"bottomshiftablegauge", 341},
      std::pair{"hispeedautoadjust", 342},
      std::pair{"guidese", 343},
      std::pair{"chartreplicationmode", 344},
      std::pair{"extranotedepth", 350},
      std::pair{"minemode", 351},
      std::pair{"scrollmode", 352},
      std::pair{"longnotemode", 353},
      std::pair{"seventonine_pattern", 360},
      std::pair{"seventonine_type", 361},
      std::pair{"constant", 400},
  };
  const auto found = std::ranges::find(names, name, &decltype(names)::value_type::first);
  return found == names.end() ? -1 : found->second;
}

std::optional<int> notePriorityIndex(AppSettings::NotePriorityMode value) {
  switch (value) {
  case AppSettings::NotePriorityMode::Combo: return 0;
  case AppSettings::NotePriorityMode::Duration: return 1;
  case AppSettings::NotePriorityMode::Lowest: return 2;
  case AppSettings::NotePriorityMode::Score: return std::nullopt;
  }
  return std::nullopt;
}

AppSettings::NotePriorityMode notePriorityAt(int index) {
  constexpr std::array values{
      AppSettings::NotePriorityMode::Combo,
      AppSettings::NotePriorityMode::Duration,
      AppSettings::NotePriorityMode::Lowest,
  };
  return values[static_cast<std::size_t>(index)];
}
} // namespace

MusicSelectEventOutcome MusicSelectEventController::execute(
    MusicSelectEventContext &context, int eventId, std::string_view eventName,
    int argument1, int argument2) {
  MusicSelectEventOutcome outcome;
  if (!eventName.empty()) eventId = eventIdForName(eventName);
  auto &settings = context.settings;

  switch (eventId) {
  case 10:
    settings.skinDifficultyFilterName = std::string(
        kDifficulties[sourceNextIndex(kDifficulties,
                                      settings.skinDifficultyFilterName,
                                      argument1)]);
    refreshWithSound(outcome);
    break;
  case 11:
    settings.skinModeFilterName = std::string(
        kModes[sourceNextIndex(kModes, settings.skinModeFilterName,
                               argument1)]);
    refreshWithSound(outcome);
    break;
  case 12:
    context.sortIndex = cycleInteger(context.sortIndex, 8, argument1);
    settings.skinSortId =
        std::string(kDefaultSorts[static_cast<std::size_t>(context.sortIndex)]);
    refreshWithSound(outcome);
    break;
  case 312: {
    // Pinned songbar_sort writes PlayerConfig.sortid only. It intentionally
    // leaves MusicSelector.sort (integer property 12) unchanged.
    const auto found = std::ranges::find(kAllSorts, settings.skinSortId);
    if (found == kAllSorts.end()) break;
    const auto index = static_cast<int>(std::distance(kAllSorts.begin(), found));
    settings.skinSortId = std::string(kAllSorts[static_cast<std::size_t>(
        cycleInteger(index, static_cast<int>(kAllSorts.size()), argument1))]);
    refreshWithSound(outcome);
    break;
  }
  case 13:
    // No dedicated key-config surface exists yet (ruling: it would slot
    // here). Until then keyconfig shares the Settings destination.
    effect(outcome, Effect::OpenSettings);
    break;
  case 14: effect(outcome, Effect::OpenSettings); break;
  case 15: effect(outcome, Effect::Play); break;
  case 16: effect(outcome, Effect::Autoplay); break;
  case 315: effect(outcome, Effect::Practice); break;
  case 19: effect(outcome, Effect::Replay, 0); break;
  case 316: effect(outcome, Effect::Replay, 1); break;
  case 317: effect(outcome, Effect::Replay, 2); break;
  case 318: effect(outcome, Effect::Replay, 3); break;
  case 17: effect(outcome, Effect::OpenDocument); break;
  case 210: effect(outcome, Effect::OpenIr); break;
  case 211: effect(outcome, Effect::UpdateFolder); break;
  case 212: effect(outcome, Effect::OpenExplorer); break;
  case 213: effect(outcome, Effect::OpenDownloadSite); break;
  case 40: {
    const auto index = sourceNextIndex(kGauges, settings.selectedGaugeType,
                                       argument1);
    settings.selectedGaugeType = std::string(kGauges[index]);
    changedWithSound(outcome);
    break;
  }
  case 42: {
    const auto index = sourceNextIndex(replay::kBeatorajaReplayOptions,
                                       settings.selectedPlayOption, argument1);
    settings.selectedPlayOption =
        std::string(replay::kBeatorajaReplayOptions[index]);
    changedWithSound(outcome);
    break;
  }
  case 43:
    settings.skinPlayer2RandomOption =
        cycleInteger(settings.skinPlayer2RandomOption, 10, argument1);
    changedWithSound(outcome);
    break;
  case 54:
    settings.skinDoublePlayOption =
        cycleInteger(settings.skinDoublePlayOption, 4, argument1);
    changedWithSound(outcome);
    break;
  case 55:
    if (!context.hasSelectedPlayConfig) break;
    settings.hispeedFixMode = static_cast<AppSettings::HiSpeedFixMode>(
        cycleInteger(static_cast<int>(settings.hispeedFixMode), 5, argument1));
    changedWithSound(outcome);
    break;
  case 57: {
    if (!context.hasSelectedPlayConfig) break;
    const float candidate = std::clamp(
        settings.gameplayHispeed +
            (argument1 >= 0 ? settings.hispeedMargin : -settings.hispeedMargin),
        AppSettings::kMinGameplayHispeed, AppSettings::kMaxGameplayHispeed);
    if (candidate == settings.gameplayHispeed) break;
    settings.gameplayHispeed = candidate;
    changedWithSound(outcome);
    break;
  }
  case 59: {
    if (!context.hasSelectedPlayConfig) break;
    const int increment = argument2 > 0 ? argument2 : 1;
    const int candidate = std::clamp(
        settings.visibleTimeDurationMilliseconds +
            (argument1 >= 0 ? increment : -increment),
        AppSettings::kMinVisibleTimeMs, AppSettings::kMaxVisibleTimeMs);
    if (candidate == settings.visibleTimeDurationMilliseconds) break;
    settings.visibleTimeDurationMilliseconds = candidate;
    changedWithSound(outcome);
    break;
  }
  case 72:
    settings.skinBgaMode = cycleInteger(settings.skinBgaMode, 3, argument1);
    settings.bgaEnabled = settings.skinBgaMode != 2;
    changedWithSound(outcome);
    effect(outcome, Effect::ApplyBgaEnabled, settings.bgaEnabled ? 1 : 0);
    break;
  case 73:
    settings.skinBgaExpandMode =
        cycleInteger(settings.skinBgaExpandMode, 3, argument1);
    switch (settings.skinBgaExpandMode) {
    case 0:
      settings.bgaDisplayMode = AppSettings::BgaDisplayMode::Stretch;
      break;
    case 1:
      settings.bgaDisplayMode = AppSettings::BgaDisplayMode::Fit;
      break;
    case 2:
      settings.bgaDisplayMode = AppSettings::BgaDisplayMode::NoExpand;
      break;
    }
    changedWithSound(outcome);
    break;
  case 74: {
    const int increment = argument1 >= 0
                              ? (settings.notesDisplayTimingMilliseconds < 500)
                              : -(settings.notesDisplayTimingMilliseconds > -500);
    if (increment == 0) break;
    settings.notesDisplayTimingMilliseconds += increment;
    changedWithSound(outcome);
    break;
  }
  case 75:
    settings.notesDisplayTimingAutoAdjust =
        !settings.notesDisplayTimingAutoAdjust;
    changedWithSound(outcome);
    break;
  case 77: {
    if (settings.skinTargetList.empty()) {
      outcome.failure = "TargetProperty.getTargets() returned an empty list.";
      break;
    }
    const auto found = std::ranges::find(settings.skinTargetList,
                                         settings.skinTargetId);
    const auto index = static_cast<std::size_t>(
        std::distance(settings.skinTargetList.begin(), found));
    settings.skinTargetId = settings.skinTargetList[
        (index + (argument1 >= 0 ? 1 : settings.skinTargetList.size() - 1)) %
        settings.skinTargetList.size()];
    outcome.settingsChanged = true;
    break;
  }
  case 78: {
    const auto index = sourceNextIndex(kGaugeAutoShift,
                                       settings.selectedGaugeAutoShiftMode,
                                       argument1);
    settings.selectedGaugeAutoShiftMode = std::string(kGaugeAutoShift[index]);
    changedWithSound(outcome);
    break;
  }
  case 341: {
    const auto index = sourceNextIndex(
        kBottomShiftableGauge, settings.selectedGaugeAutoShiftLowerBound,
        argument1);
    settings.selectedGaugeAutoShiftLowerBound =
        std::string(kBottomShiftableGauge[index]);
    changedWithSound(outcome);
    break;
  }
  case 79: {
    const int length = context.rivalCount + 1;
    context.currentRivalIndex =
        (context.currentRivalIndex +
         (argument1 >= 0 ? 2 : context.rivalCount + 1)) %
            length -
        1;
    effect(outcome, Effect::SetRival, context.currentRivalIndex);
    effect(outcome, Effect::RefreshBars);
    effect(outcome, Effect::OptionChangeSound);
    break;
  }
  case 89:
  case 90:
    if (!context.selectedSongHasPath) break;
    effect(outcome,
           eventId == 89 ? Effect::ChangeFavoriteSong
                         : Effect::ChangeFavoriteChart,
           argument1 >= 0 ? 1 : -1);
    effect(outcome, Effect::RefreshBars);
    effect(outcome, Effect::OptionChangeSound);
    break;
  case 308: {
    const auto index = sourceNextIndex(kLnModes, settings.selectedLnMode,
                                       argument1);
    settings.selectedLnMode = std::string(kLnModes[index]);
    refreshWithSound(outcome);
    break;
  }
  case 321:
  case 322:
  case 323:
  case 324: {
    const auto index = static_cast<std::size_t>(eventId - 321);
    settings.autoSaveReplay[index] =
        cycleInteger(settings.autoSaveReplay[index], 11, argument1);
    changedWithSound(outcome);
    break;
  }
  case 330:
    if (!context.hasSelectedPlayConfig) break;
    settings.laneCoverEnabled = !settings.laneCoverEnabled;
    changedWithSound(outcome);
    break;
  case 331:
    if (!context.hasSelectedPlayConfig) break;
    settings.liftEnabled = !settings.liftEnabled;
    changedWithSound(outcome);
    break;
  case 332:
    if (!context.hasSelectedPlayConfig) break;
    settings.hiddenEnabled = !settings.hiddenEnabled;
    changedWithSound(outcome);
    break;
  case 340: {
    if (!context.hasSelectedPlayConfig) break;
    const auto index = notePriorityIndex(settings.notePriorityMode);
    if (!index) break;
    settings.notePriorityMode = notePriorityAt(
        cycleInteger(*index, 3, argument1));
    changedWithSound(outcome);
    break;
  }
  case 342:
    if (!context.hasSelectedPlayConfig) break;
    settings.hispeedAutoAdjust = !settings.hispeedAutoAdjust;
    changedWithSound(outcome);
    break;
  case 343:
    settings.guideSoundEffects = !settings.guideSoundEffects;
    changedWithSound(outcome);
    break;
  case 344: {
    // Pinned chartreplicationmode also reads and writes PlayerConfig.sortid,
    // including its exact no-op unless sortid already names a mode.
    const auto found = std::ranges::find(kChartReplicationModes,
                                         settings.skinSortId);
    if (found == kChartReplicationModes.end()) break;
    const auto index = static_cast<int>(
        std::distance(kChartReplicationModes.begin(), found));
    settings.skinSortId = std::string(kChartReplicationModes[
        static_cast<std::size_t>(cycleInteger(index, 5, argument1))]);
    changedWithSound(outcome);
    break;
  }
  case 350:
    settings.extraNoteDepth = cycleInteger(settings.extraNoteDepth, 4,
                                           argument1);
    changedWithSound(outcome);
    break;
  case 351:
    settings.mineMode = cycleInteger(settings.mineMode, 5, argument1);
    changedWithSound(outcome);
    break;
  case 352:
    settings.scrollMode = cycleInteger(settings.scrollMode, 3, argument1);
    changedWithSound(outcome);
    break;
  case 353:
    settings.longNoteModifierMode =
        cycleInteger(settings.longNoteModifierMode, 6, argument1);
    changedWithSound(outcome);
    break;
  case 360:
    settings.sevenToNinePattern =
        cycleInteger(settings.sevenToNinePattern, 7, argument1);
    changedWithSound(outcome);
    break;
  case 361:
    settings.sevenToNineType =
        cycleInteger(settings.sevenToNineType, 3, argument1);
    changedWithSound(outcome);
    break;
  case 400:
    if (!context.hasSelectedPlayConfig) break;
    settings.constantScroll = !settings.constantScroll;
    changedWithSound(outcome);
    break;
  default: break;
  }
  return outcome;
}
