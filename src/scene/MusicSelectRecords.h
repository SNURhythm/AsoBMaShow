#pragma once

#include "../ResultRecordSummary.h"
#include "../ReplayAutoPlay.h"
#include "MainMenuProfileSelections.h"
#include "../music_select/MusicSelectTypes.h"

#include <array>

inline ReplaySummary musicSelectAutoPlaySummary(
    const ChartMetaRecord &record, const main_menu_profile::Selections &selections,
    audio::PlaybackRate playback) {
  auto meta = record.meta;
  if (normalizeChartLongNoteModeValue(meta.LnMode) == 0) {
    meta.LnMode = long_note_mode::valueFromId(selections.longNoteMode);
  }
  const auto playOption = play_options::isNormalPlayOption(selections.playOption)
                              ? std::optional<std::string>{}
                              : std::optional<std::string>{selections.playOption};
  return replay_autoplay::BuildSummary(
      meta, selections.gaugeType, selections.gaugeAutoShift, playOption,
      std::nullopt, std::nullopt, std::nullopt, selections.assistOption,
      playback, selections.ruleset);
}

inline std::vector<ResultRecordSummary> musicSelectChartRecords(
    const ChartMetaRecord &record, const main_menu_profile::Selections &selections,
    audio::PlaybackRate playback, std::span<const ResultRecordSummary> projected) {
  const std::array synthetic{musicSelectAutoPlaySummary(record, selections, playback)};
  return mergeResultRecords(synthetic, projected,
                            std::span<const ir::IrRemoteScore>{},
                            std::string_view{}, std::string_view{});
}

// A saved course has its own identity; it must not inherit a stage's chart hash.
inline std::optional<ChartMetaRecord>
musicSelectRecordsTarget(const MusicSelectBar &bar) {
  if (bar.kind == skin::MusicSelectBarKind::Grade &&
      (!bar.courseKey.empty() || bar.courseId > 0)) {
    ChartMetaRecord record;
    record.courseStart = true;
    record.meta.Title = bar.title;
    record.meta.Artist = bar.courseGroupName;
    if (!bar.courseCharts.empty()) {
      record.meta.KeyMode = bar.courseCharts.front().meta.KeyMode;
    }
    return record;
  }
  if (bar.kind == skin::MusicSelectBarKind::Song && bar.chart &&
      !bar.chart->solidArchive && !bar.chart->unavailable &&
      !bar.chart->meta.BmsPath.empty()) {
    return bar.chart;
  }
  return std::nullopt;
}
