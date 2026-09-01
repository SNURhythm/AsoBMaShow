#include "MusicSelectPropertyProjection.h"

#include "../AssistOptionUtils.h"
#include "../LongNoteModeUtils.h"
#include "../replay/ReplayOption.h"
#include "../scene/MainMenuProfileSelections.h"
#include "../scene/play/PlayfieldVisualState.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <limits>
#include <numeric>
#include <string_view>

namespace {

using Properties = skin::MusicSelectPropertyValues;

constexpr std::array<int, 4> kReplayExistsIds{197, 1197, 1200, 1203};
constexpr std::array<int, 4> kReplayMissingIds{196, 1196, 1199, 1202};
constexpr std::array<int, 4> kReplaySavedIds{198, 1198, 1201, 1204};
constexpr std::array<int, 4> kReplaySelectedIds{1205, 1206, 1207, 1208};
constexpr std::array<int, 11> kIrClearCountIds{
    202, 210, 204, 206, 212, 214, 216, 208, 218, 222, 224};
constexpr std::array<int, 11> kIrClearRateIds{
    203, 211, 205, 207, 213, 215, 217, 209, 219, 223, 225};
constexpr std::array<int, 11> kIrClearRateAfterDotIds{
    230, 234, 231, 232, 235, 236, 237, 233, 238, 239, 240};

int modeFilterSkinNumber(std::string_view value) {
  // select/ModeFilter.java: declaration order is not its skin number.
  constexpr std::array<std::pair<std::string_view, int>, 10> values{{
      {"ALL", 0}, {"7KEY", 2}, {"14KEY", 4}, {"9KEY", 5},
      {"5KEY", 1}, {"10KEY", 3}, {"24KEY", 6}, {"48KEY", 7},
      {"SINGLE", 8}, {"DOUBLE", 9},
  }};
  const auto found = std::ranges::find(values, value, &decltype(values)::value_type::first);
  return found == values.end() ? 0 : found->second;
}

int difficultyFilterSkinNumber(std::string_view value) {
  // select/DifficultyFilter.java exposes these exact display strings/numbers.
  constexpr std::array<std::pair<std::string_view, int>, 9> values{{
      {"ALL", 0},          {"BEGINNER", 1},        {"NORMAL", 2},
      {"HYPER", 3},        {"ANOTHER", 4},         {"INSANE", 5},
      {"SCRATCH CHART", 6}, {"LONG NOTE CHART", 7}, {"SPEED CHANGE CHART", 8},
  }};
  const auto found = std::ranges::find(values, value, &decltype(values)::value_type::first);
  return found == values.end() ? 0 : found->second;
}

int beatorajaGaugeAutoShift(GaugeAutoShiftMode value) {
  // PlayerConfig.GAUGEAUTOSHIFT_* is NONE, CONTINUE, SURVIVAL, BEST, SELECT.
  switch (value) {
  case GaugeAutoShiftMode::Continue: return 1;
  case GaugeAutoShiftMode::SurvivalToGroove: return 2;
  case GaugeAutoShiftMode::BestClear: return 3;
  case GaugeAutoShiftMode::SelectToUnder: return 4;
  case GaugeAutoShiftMode::None: return 0;
  }
  return 0;
}

int beatorajaLnMode(std::string_view value) {
  if (value == long_note_mode::kCnId) return 1;
  if (value == long_note_mode::kHcnId) return 2;
  return 0;
}

int songMode(const bms_parser::ChartMeta &meta) {
  if (meta.KeyMode == 5 && !meta.IsDP) return 5;
  if (meta.KeyMode == 7 && !meta.IsDP) return 7;
  if (meta.KeyMode == 9 && !meta.IsDP) return 9;
  if (meta.KeyMode == 10 || (meta.KeyMode == 5 && meta.IsDP)) return 10;
  if (meta.KeyMode == 14 || (meta.KeyMode == 7 && meta.IsDP)) return 14;
  if (meta.KeyMode == 24 && !meta.IsDP) return 25;
  if (meta.KeyMode == 48 || (meta.KeyMode == 24 && meta.IsDP)) return 50;
  return 0;
}

bool isPlayableBar(const MusicSelectBar &bar) {
  // BooleanPropertyFactory.playablebar tests these exact concrete classes.
  switch (bar.kind) {
  case skin::MusicSelectBarKind::Song:
    return bar.chart.has_value() && bar.presentation.exists;
  case skin::MusicSelectBarKind::Grade:
  case skin::MusicSelectBarKind::RandomCourse:
    return bar.presentation.exists;
  case skin::MusicSelectBarKind::Executable:
    return true;
  case skin::MusicSelectBarKind::Folder:
  case skin::MusicSelectBarKind::Table:
  case skin::MusicSelectBarKind::Hash:
  case skin::MusicSelectBarKind::Command:
  case skin::MusicSelectBarKind::Container:
  case skin::MusicSelectBarKind::SearchWord:
  case skin::MusicSelectBarKind::SameFolder:
    return false;
  }
  return false;
}

bool hasCourseConstraint(const MusicSelectBar &bar,
                         skin::MusicSelectCourseConstraint constraint) {
  return std::ranges::find(bar.courseConstraints, constraint) !=
         bar.courseConstraints.end();
}

void projectCourse(Properties &out, const MusicSelectBar &bar) {
  using Constraint = skin::MusicSelectCourseConstraint;
  constexpr std::array<std::pair<Constraint, int>, 14>
      constraints{{
          {Constraint::Class, 1002},
          {Constraint::Mirror, 1003},
          {Constraint::Random, 1004},
          {Constraint::NoSpeed, 1005},
          {Constraint::NoGood, 1006},
          {Constraint::NoGreat, 1007},
          {Constraint::GaugeLr2, 1010},
          {Constraint::Gauge5Keys, 1011},
          {Constraint::Gauge7Keys, 1012},
          {Constraint::Gauge9Keys, 1013},
          {Constraint::Gauge24Keys, 1014},
          {Constraint::Ln, 1015},
          {Constraint::Cn, 1016},
          {Constraint::Hcn, 1017},
      }};
  for (const auto &[constraint, id] : constraints) {
    out.booleans[id] = hasCourseConstraint(bar, constraint);
  }
  const std::size_t count = std::min<std::size_t>(10, bar.courseStages.size());
  for (std::size_t index = 0; index < count; ++index) {
    const auto &stage = bar.courseStages[index];
    const std::string title = stage.title.value_or("----");
    out.strings[150 + static_cast<int>(index)] =
        bar.kind == skin::MusicSelectBarKind::Grade && !stage.hasPath
            ? "(no song) " + title
            : title;
  }
}

void projectLastPlayed(Properties &out, std::int64_t unixSeconds) {
  if (unixSeconds <= 0) return;
  if (unixSeconds <= std::numeric_limits<int>::max()) {
    out.integers[243] = unixSeconds;
  }
  const std::time_t value = static_cast<std::time_t>(unixSeconds);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &value);
#else
  localtime_r(&value, &local);
#endif
  out.integers[244] = local.tm_year + 1900;
  out.integers[245] = local.tm_mon + 1;
  out.integers[246] = local.tm_mday;
  out.integers[247] = local.tm_hour;
  out.integers[248] = local.tm_min;
  out.integers[249] = local.tm_sec;
}

int favoriteIndex(int flags, int favorite, int invisible) {
  if ((flags & (favorite | invisible)) == 0) return 0;
  return (flags & invisible) != 0 ? 2 : 1;
}

int integerRate(int score, int maximum) {
  return maximum > 0 ? static_cast<int>((static_cast<double>(score) /
                                         static_cast<double>(maximum)) * 100.0)
                     : 100;
}

int integerRateAfterDot(int score, int maximum) {
  return maximum > 0
             ? static_cast<int>((static_cast<double>(score) /
                                 static_cast<double>(maximum)) * 10'000.0) %
                   100
             : 0;
}

double scoreRate(int score, int maximum) {
  return maximum > 0 ? static_cast<double>(score) / maximum : 1.0;
}

int chartJudge(int rank) {
  if (rank == 0 || (rank >= 10 && rank < 35)) return 0;
  if (rank == 1 || (rank >= 35 && rank < 60)) return 1;
  if (rank == 2 || (rank >= 60 && rank < 85)) return 2;
  if (rank == 3 || (rank >= 85 && rank < 110)) return 3;
  return 4;
}

void projectClock(Properties &out,
                  const MusicSelectPropertyRuntimeSnapshot &runtime) {
  const auto &history = runtime.playerHistory;
  out.integers[17] = history.playDurationSeconds / 3600;
  out.integers[18] = (history.playDurationSeconds / 60) % 60;
  out.integers[19] = history.playDurationSeconds % 60;
  out.integers[20] = runtime.framesPerSecond;
  out.integers[21] = runtime.wallClock.year;
  out.integers[22] = runtime.wallClock.month;
  out.integers[23] = runtime.wallClock.day;
  out.integers[24] = runtime.wallClock.hour;
  out.integers[25] = runtime.wallClock.minute;
  out.integers[26] = runtime.wallClock.second;
  out.integers[27] = runtime.applicationUptimeMillis / 3'600'000;
  out.integers[28] = (runtime.applicationUptimeMillis / 60'000) % 60;
  out.integers[29] = (runtime.applicationUptimeMillis / 1'000) % 60;
  out.integers[30] = history.playCount;
  out.integers[31] = history.clearCount;
  out.integers[32] = history.playCount - history.clearCount;
  for (int index = 0; index < 5; ++index) {
    out.integers[33 + index] =
        history.judgementCounts[static_cast<std::size_t>(index)];
  }
  out.integers[333] = std::accumulate(history.judgementCounts.begin(),
                                      history.judgementCounts.begin() + 4, 0);
}

void projectRanking(Properties &out,
                    const MusicSelectPropertyRuntimeSnapshot &runtime) {
  const auto &ranking = runtime.ranking;
  out.booleans[50] = !runtime.irOnline;
  out.booleans[51] = runtime.irOnline;
  out.booleans[603] = ranking.state == MusicSelectRankingState::Finish &&
                      ranking.totalPlayers == 0;
  out.booleans[604] = ranking.state == MusicSelectRankingState::Fail;
  // BooleanPropertyFactory.ir_busy uses FAIL in the pinned source as written.
  out.booleans[608] = ranking.state == MusicSelectRankingState::Fail;
  out.booleans[606] = ranking.state == MusicSelectRankingState::None;
  if (ranking.pendingDurationMillis > 0) {
    out.integers[220] =
        static_cast<int>(ranking.pendingDurationMillis / 1'000 + 1);
  }

  if (ranking.state != MusicSelectRankingState::Finish) return;
  out.integers[179] = ranking.rank;
  out.integers[180] = ranking.totalPlayers;
  out.integers[200] = ranking.totalPlayers;
  for (std::size_t clear = 0; clear < ranking.clearCounts.size(); ++clear) {
    const int count = ranking.clearCounts[clear];
    out.integers[kIrClearCountIds[clear]] = count;
    if (ranking.totalPlayers > 0) {
      out.integers[kIrClearRateIds[clear]] =
          count * 100 / ranking.totalPlayers;
      out.integers[kIrClearRateAfterDotIds[clear]] =
          (count * 1'000 / ranking.totalPlayers) % 10;
      out.rates[static_cast<int>(std::array<int, 11>{
          203, 211, 205, 207, 213, 215, 217, 209, 219, 223, 225}[clear])] =
          static_cast<double>(count) / ranking.totalPlayers;
    }
  }
  const int totalClear = std::accumulate(ranking.clearCounts.begin() + 2,
                                         ranking.clearCounts.end(), 0);
  const int totalFullCombo = std::accumulate(ranking.clearCounts.begin() + 8,
                                             ranking.clearCounts.end(), 0);
  out.integers[226] = totalClear;
  out.integers[228] = totalFullCombo;
  if (ranking.totalPlayers > 0) {
    out.integers[227] = totalClear * 100 / ranking.totalPlayers;
    out.integers[241] =
        (totalClear * 1'000 / ranking.totalPlayers) % 10;
    out.integers[229] = totalFullCombo * 100 / ranking.totalPlayers;
    out.integers[242] =
        (totalFullCombo * 1'000 / ranking.totalPlayers) % 10;
    out.floats[227] = static_cast<double>(totalClear) / ranking.totalPlayers;
    out.floats[229] =
        static_cast<double>(totalFullCombo) / ranking.totalPlayers;
  }

  for (int visible = 0; visible < 10; ++visible) {
    const int sourceIndex = visible + ranking.offset;
    if (sourceIndex < 0 ||
        sourceIndex >= static_cast<int>(ranking.entries.size())) {
      continue;
    }
    const auto &entry = ranking.entries[static_cast<std::size_t>(sourceIndex)];
    out.strings[120 + visible] = entry.name;
    out.integers[380 + visible] = entry.score;
    out.integers[390 + visible] = entry.rank;
    out.imageIndexes[380 + visible] = entry.playerType;
    out.imageIndexes[390 + visible] = entry.clearType;
  }
}

void projectSelectedBar(Properties &out,
                        const MusicSelectBarManagerSnapshot &bars,
                        const MusicSelectPropertyRuntimeSnapshot &runtime) {
  const MusicSelectBar *selected =
      bars.selectedIndex < bars.rows.size() ? &bars.rows[bars.selectedIndex]
                                           : nullptr;
  const bool directory =
      selected && skin::musicSelectIsDirectoryBarKind(selected->kind);
  const bool song = selected && selected->kind == skin::MusicSelectBarKind::Song;
  const bool course = selected && selected->kind == skin::MusicSelectBarKind::Grade;
  out.booleans[1] = directory;
  out.booleans[2] = song;
  out.booleans[3] = course;
  out.booleans[5] = selected && isPlayableBar(*selected);
  out.booleans[100] =
      selected && (song || course) &&
      (!selected->score || selected->presentation.lamp == 0);
  out.booleans[1030] = selected &&
                       selected->kind == skin::MusicSelectBarKind::Executable;
  out.booleans[1031] = selected &&
                       selected->kind == skin::MusicSelectBarKind::RandomCourse;

  if (!selected) return;
  out.strings[10] = selected->title;
  out.strings[12] = selected->title;

  if (course || selected->kind == skin::MusicSelectBarKind::RandomCourse) {
    projectCourse(out, *selected);
  }

  const int clear = std::clamp(selected->presentation.lamp, 0, 10);
  constexpr std::array<int, 11> clearIds{
      100, 101, 1100, 1101, 102, 103, 104, 1102, 105, 1103, 1104};
  for (std::size_t index = 0; index < clearIds.size(); ++index) {
    out.booleans[clearIds[index]] =
        (song || course) && clear == static_cast<int>(index);
  }

  for (std::size_t replay = 0; replay < selected->replayExists.size(); ++replay) {
    const bool selectable =
        skin::musicSelectIsSelectableBarKind(selected->kind);
    const bool exists = selectable && selected->replayExists[replay];
    out.booleans[kReplayExistsIds[replay]] = exists;
    out.booleans[kReplayMissingIds[replay]] = selectable && !exists;
    // createReplayProperty handles both non-zero types as !exists on selector.
    out.booleans[kReplaySavedIds[replay]] = selectable && !exists;
    out.booleans[kReplaySelectedIds[replay]] =
        runtime.selectedReplay == static_cast<int>(replay);
  }

  if (directory) {
    const auto &counts = selected->presentation.folderLampCounts;
    out.integers[300] = std::accumulate(counts.begin(), counts.end(), 0);
    for (int clearType = 0; clearType < 11; ++clearType) {
      out.integers[320 + clearType] =
          counts[static_cast<std::size_t>(clearType)];
    }
    return;
  }
  if (selected->rivalScore) {
    out.imageIndexes[371] =
        std::clamp(selected->presentation.rivalLamp, 0, 10);
  }
  if (!selected->chart) return;

  const auto &record = *selected->chart;
  const auto &meta = record.meta;
  out.strings[10] = meta.Title;
  out.strings[11] = meta.SubTitle;
  out.strings[12] = selected->title;
  out.strings[13] = meta.Genre;
  out.strings[14] = meta.Artist;
  out.strings[15] = meta.SubArtist;
  out.strings[16] = meta.SubArtist.empty() ? meta.Artist
                                           : meta.Artist + " " + meta.SubArtist;
  out.strings[1030] = meta.MD5;
  out.strings[1031] = meta.SHA256;

  out.booleans[174] = !record.hasDocument;
  out.booleans[175] = record.hasDocument;
  const bool hasLongNote =
      meta.TotalLongNotes > 0 || meta.TotalBackSpinNotes > 0;
  out.booleans[172] = !hasLongNote;
  out.booleans[173] = hasLongNote;
  const bool hasRandom = meta.RandomSeed.has_value() || !meta.RandomValues.empty();
  out.booleans[178] = !hasRandom;
  out.booleans[179] = hasRandom;
  out.booleans[176] = meta.MinBpm == meta.MaxBpm;
  out.booleans[177] = meta.MinBpm < meta.MaxBpm;
  for (int difficulty = 0; difficulty <= 5; ++difficulty) {
    out.booleans[150 + difficulty] =
        difficulty == 0 ? meta.Difficulty <= 0 || meta.Difficulty > 5
                        : meta.Difficulty == difficulty;
  }
  const int judge = chartJudge(meta.Rank);
  for (int index = 0; index < 5; ++index) {
    out.booleans[180 + index] = judge == index;
  }
  const int mode = songMode(meta);
  out.booleans[160] = mode == 7;
  out.booleans[161] = mode == 5;
  out.booleans[162] = mode == 14;
  out.booleans[163] = mode == 10;
  out.booleans[164] = mode == 9;
  out.booleans[1160] = mode == 25;
  out.booleans[1161] = mode == 50;

  out.integers[74] = meta.TotalNotes;
  out.integers[106] = meta.TotalNotes;
  out.integers[90] = static_cast<int>(meta.MaxBpm);
  out.integers[91] = static_cast<int>(meta.MinBpm);
  out.integers[92] = static_cast<int>(meta.MostPrevalentBpm);
  out.integers[96] = static_cast<int>(meta.PlayLevel);
  out.integers[1163] = (meta.PlayLength / 1'000'000 / 60) % 60;
  out.integers[1164] = (meta.PlayLength / 1'000'000) % 60;
  out.imageIndexes[89] = favoriteIndex(record.songReviewFavorite, 1, 4);
  out.imageIndexes[90] = favoriteIndex(record.songReviewFavorite, 2, 8);

  // FloatPropertyFactory.getLevelRate intentionally falls through all
  // recognized Mode cases in the pinned source, leaving maxLevel at 10.
  const double levelRate = mode == 0 ? 0.0 : meta.PlayLevel / 10.0;
  out.rates[103] = levelRate;
  for (int difficulty = 1; difficulty <= 5; ++difficulty) {
    out.rates[104 + difficulty] =
        meta.Difficulty == difficulty ? levelRate : 0.0;
  }
  for (int judgeIndex = 0; judgeIndex < 5; ++judgeIndex) {
    out.rates[140 + judgeIndex] = 0.0;
  }
  out.rates[145] = 0.0;
  out.rates[147] = 0.0;

  if (!selected->score) return;
  const auto &score = *selected->score;
  const int maximum = score.maxScore;
  out.integers[71] = score.score;
  out.integers[72] = maximum;
  out.integers[75] = score.maxCombo.value_or(0);
  out.integers[76] = score.badPoints.value_or(0);
  out.integers[77] = score.playCount;
  out.integers[78] = score.clearCount;
  out.integers[79] = score.playCount - score.clearCount;
  if (score.lastPlayedUnixSeconds) {
    projectLastPlayed(out, *score.lastPlayedUnixSeconds);
  }
  out.integers[101] = score.score;
  out.integers[102] = integerRate(score.score, maximum);
  out.integers[103] = integerRateAfterDot(score.score, maximum);
  out.integers[105] = score.maxCombo.value_or(0);
  out.integers[150] = 0;
  out.integers[170] = 0;
  out.integers[171] = score.score;
  out.integers[174] = score.maxCombo.value_or(0);
  out.integers[177] = score.badPoints.value_or(0);
  out.integers[183] = 0;
  out.integers[184] = 0;
  out.imageIndexes[370] = clear;

  const double selectedRate = scoreRate(score.score, maximum);
  out.rates[110] = selectedRate;
  out.rates[111] = selectedRate;
  out.rates[112] = 0.0;
  out.rates[113] = 0.0;
  out.floats[1102] = selectedRate;
  out.floats[1115] = selectedRate;
  out.floats[155] = selectedRate;
  out.floats[183] = 0.0;
  if (meta.TotalNotes > 0) {
    for (int judgeIndex = 0; judgeIndex < 5; ++judgeIndex) {
      out.rates[140 + judgeIndex] =
          static_cast<double>(score.judgementCounts[
              static_cast<std::size_t>(judgeIndex)]) /
          meta.TotalNotes;
    }
    out.rates[145] =
        static_cast<double>(score.maxCombo.value_or(0)) / meta.TotalNotes;
    out.rates[147] =
        static_cast<double>(score.score) / meta.TotalNotes / 2.0;
  }

  if (selected->rivalScore) {
    const auto &rival = *selected->rivalScore;
    const double rivalRate = scoreRate(rival.score, rival.maxScore);
    out.integers[121] = rival.score;
    out.integers[122] = integerRate(rival.score, rival.maxScore);
    out.integers[123] = integerRateAfterDot(rival.score, rival.maxScore);
    out.integers[129] = out.integers[122];
    out.integers[130] = out.integers[123];
    out.integers[151] = rival.score;
    out.integers[157] = out.integers[122];
    out.integers[158] = out.integers[123];
    out.integers[271] = rival.score;
    out.rates[114] = 0.0;
    out.rates[115] = rivalRate;
    out.floats[122] = rivalRate;
    out.floats[135] = rivalRate;
    out.floats[157] = rivalRate;
  }
}

} // namespace

skin::MusicSelectPropertyValues projectMusicSelectProperties(
    const AppSettings &settings, const MusicSelectBarManagerSnapshot &bars,
    const MusicSelectPropertyRuntimeSnapshot &runtime) {
  Properties out;
  out.rates[1] = bars.rows.empty()
                     ? 0.0
                     : static_cast<double>(bars.selectedIndex) /
                           static_cast<double>(bars.rows.size());
  out.rates[8] = static_cast<double>(runtime.ranking.offset) /
                 std::max(1, runtime.ranking.totalPlayers);
  out.strings[1] = runtime.rivalName;
  out.strings[2] = runtime.playerName;
  out.strings[3] = runtime.targetName;
  out.strings[30] = runtime.searchWord;
  out.strings[60] = settings.skinModeFilterName;
  out.strings[61] = settings.skinSortId;
  out.strings[62] = settings.skinDifficultyFilterName;
  out.strings[86] = settings.skinChartReplicationMode;
  out.strings[1000] = bars.directoryText;
  out.strings[1001] = runtime.tableName;
  out.strings[1002] = runtime.tableLevel;
  out.strings[1003] = runtime.tableFullName;
  out.strings[1010] = runtime.version;
  out.strings[1020] = runtime.irName;
  out.strings[1021] = runtime.irUserName;

  out.booleans[21] = runtime.panelState == 1;
  out.booleans[22] = runtime.panelState == 2;
  out.booleans[23] = runtime.panelState == 3;
  out.booleans[60] = !runtime.updateScore;
  out.booleans[61] = runtime.updateScore;
  out.booleans[62] = runtime.updateScore;
  out.booleans[624] = runtime.rivalName.empty();
  out.booleans[625] = !runtime.rivalName.empty();
  out.booleans[400] = settings.constantScroll;

  projectClock(out, runtime);
  const auto &audio = settings.audioVideo.audio;
  out.integers[57] = static_cast<int>(audio.masterVolume * 100);
  out.integers[58] = static_cast<int>(audio.keysoundVolume * 100);
  out.integers[59] = static_cast<int>(audio.bgmVolume * 100);
  out.rates[17] = audio.masterVolume;
  out.rates[18] = audio.keysoundVolume;
  out.rates[19] = audio.bgmVolume;

  const auto selections = main_menu_profile::Selections::fromSettings(settings);
  out.imageIndexes[10] = difficultyFilterSkinNumber(
      settings.skinDifficultyFilterName);
  out.imageIndexes[11] = modeFilterSkinNumber(settings.skinModeFilterName);
  out.imageIndexes[12] = runtime.sortIndex;
  out.imageIndexes[40] = gaugeTypeIndex(selections.gaugeType);
  out.imageIndexes[42] = replay::projectedBeatorajaReplayOptionIndex(
                             settings.selectedPlayOption)
                             .value_or(0);
  out.imageIndexes[43] = settings.skinPlayer2RandomOption;
  out.imageIndexes[54] = settings.skinDoublePlayOption;
  out.imageIndexes[55] = static_cast<int>(settings.hispeedFixMode);
  out.imageIndexes[72] = settings.skinBgaMode;
  out.imageIndexes[73] = settings.skinBgaExpandMode;
  out.imageIndexes[75] = settings.notesDisplayTimingAutoAdjust ? 1 : 0;
  out.imageIndexes[78] = beatorajaGaugeAutoShift(selections.gaugeAutoShift);
  out.imageIndexes[341] = gaugeTypeIndex(selections.gaugeAutoShiftLowerBound);
  out.imageIndexes[301] = settings.customJudge ? 1 : 0;
  out.imageIndexes[303] = settings.showJudgeArea ? 1 : 0;
  out.imageIndexes[305] = settings.markProcessedNotes ? 1 : 0;
  out.imageIndexes[306] =
      assist_options::isBpmGuide(settings.selectedAssistOption) ? 1 : 0;
  out.imageIndexes[308] = beatorajaLnMode(settings.selectedLnMode);
  for (int replayIndex = 0; replayIndex < 4; ++replayIndex) {
    out.imageIndexes[321 + replayIndex] =
        settings.autoSaveReplay[static_cast<std::size_t>(replayIndex)];
  }
  out.imageIndexes[330] = settings.laneCoverEnabled ? 1 : 0;
  out.imageIndexes[331] = settings.liftEnabled ? 1 : 0;
  out.imageIndexes[332] = settings.hiddenEnabled ? 1 : 0;
  out.imageIndexes[340] =
      beatorajaJudgeAlgorithmImageIndex(settings.notePriorityMode);
  out.imageIndexes[342] = settings.hispeedAutoAdjust ? 1 : 0;
  out.imageIndexes[343] = settings.guideSoundEffects ? 1 : 0;
  out.imageIndexes[350] = settings.extraNoteDepth;
  out.imageIndexes[351] = settings.mineMode;
  out.imageIndexes[352] = settings.scrollMode;
  out.imageIndexes[353] = settings.longNoteModifierMode;
  out.imageIndexes[360] = settings.sevenToNinePattern;
  out.imageIndexes[361] = settings.sevenToNineType;
  out.imageIndexes[400] = settings.constantScroll ? 1 : 0;

  out.integers[12] = settings.notesDisplayTimingMilliseconds;
  out.integers[312] = settings.visibleTimeDurationMilliseconds;
  out.integers[313] = settings.visibleTimeGreenNumber();
  out.integers[10] = static_cast<int>(settings.gameplayHispeed * 100);
  out.integers[310] = static_cast<int>(settings.gameplayHispeed);
  out.integers[311] =
      static_cast<int>(settings.gameplayHispeed * 100) % 100;
  out.floats[310] = settings.gameplayHispeed;

  projectSelectedBar(out, bars, runtime);
  projectRanking(out, runtime);
  return out;
}
