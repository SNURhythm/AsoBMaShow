#pragma once

#include "../ResultRecordSummary.h"
#include "../ReplayAutoPlay.h"
#include "MainMenuProfileSelections.h"

#include <array>

inline std::vector<ResultRecordSummary> musicSelectChartRecords(
    const ChartMetaRecord &record, const main_menu_profile::Selections &selections,
    audio::PlaybackRate playback,
    std::span<const ResultRecordSummary> projected) {
  auto meta = record.meta;
  if (normalizeChartLongNoteModeValue(meta.LnMode) == 0) {
    meta.LnMode = long_note_mode::valueFromId(selections.longNoteMode);
  }
  const auto playOption = play_options::isNormalPlayOption(selections.playOption)
                              ? std::optional<std::string>{}
                              : std::optional<std::string>{selections.playOption};
  const std::array synthetic{replay_autoplay::BuildSummary(
      meta, selections.gaugeType, selections.gaugeAutoShift, playOption,
      std::nullopt, std::nullopt, std::nullopt, selections.assistOption,
      playback, selections.ruleset)};
  return mergeResultRecords(synthetic, projected,
                            std::span<const ir::IrRemoteScore>{},
                            std::string_view{}, std::string_view{});
}
