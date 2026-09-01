#include "music_select/MusicSelectPropertyProjection.h"

#include "music_select_runtime_ledger_assertions.h"

#include "AssistOptionUtils.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

const MusicSelectBar &selectedSong(MusicSelectBarManagerSnapshot &bars) {
  MusicSelectBar song;
  song.id = {"sha256:abc"};
  song.kind = skin::MusicSelectBarKind::Song;
  song.title = "Title Subtitle";
  song.selectable = true;
  song.presentation = {.kind = skin::MusicSelectBarKind::Song,
                       .title = song.title,
                       .exists = true,
                       .lamp = 7,
                       .difficulty = 4,
                       .level = 12};
  song.chart.emplace();
  song.chart->hasDocument = true;
  song.chart->songReviewFavorite = 0x5;
  song.chart->meta.Title = "Title";
  song.chart->meta.SubTitle = "Subtitle";
  song.chart->meta.Genre = "Genre";
  song.chart->meta.Artist = "Artist";
  song.chart->meta.SubArtist = "Sub Artist";
  song.chart->meta.MD5 = "md5";
  song.chart->meta.SHA256 = "abc";
  song.chart->meta.PlayLevel = 12;
  song.chart->meta.TotalNotes = 500;
  song.chart->meta.TotalLongNotes = 25;
  song.chart->meta.TotalLandmineNotes = 2;
  song.chart->meta.MinBpm = 120;
  song.chart->meta.MaxBpm = 180;
  song.chart->meta.MostPrevalentBpm = 150;
  song.chart->meta.PlayLength = 154'000'000;
  song.chart->meta.Difficulty = 4;
  song.chart->meta.Rank = 2;
  song.chart->meta.KeyMode = 7;
  song.chart->meta.LnMode = 2;
  song.score = ScoreBestSnapshot{.score = 800,
                                 .maxScore = 1000,
                                 .judgementCounts = {250, 100, 50, 25, 10},
                                 .playCount = 9,
                                 .clearCount = 7,
                                 .lastPlayedUnixSeconds = 1'700'000'000,
                                 .maxCombo = 321,
                                 .comboBreak = 12,
                                 .clearType = kClearTypeExHardClearRank};
  song.rivalScore = ScoreBestSnapshot{.score = 750,
                                      .maxScore = 1000,
                                      .judgementCounts = {200, 100, 75, 40, 20},
                                      .maxCombo = 300,
                                      .comboBreak = 20,
                                      .clearType = kClearTypeHardClearRank};
  song.replayExists = {true, false, true, false};
  bars.rows.push_back(std::move(song));
  bars.directoryText = "Root > Folder > ";
  return bars.rows.front();
}

void testProjectsSelectedSongAndPlayerConfiguration() {
  AppSettings settings;
  settings.skinModeFilterName = "14KEY";
  settings.skinDifficultyFilterName = "ANOTHER";
  settings.skinSortId = "SCORE";
  settings.skinChartReplicationMode = "RIVALOPTION";
  settings.selectedGaugeType = "hard";
  settings.selectedGaugeAutoShiftMode = "best_clear";
  settings.selectedGaugeAutoShiftLowerBound = "easy";
  settings.selectedPlayOption = "R-RANDOM";
  settings.skinPlayer2RandomOption = 9;
  settings.skinDoublePlayOption = 3;
  settings.skinBgaMode = 2;
  settings.skinBgaExpandMode = 1;
  settings.selectedLnMode = "CN";
  settings.selectedAssistOption = assist_options::kBpmGuide;
  settings.visibleTimeDurationMilliseconds = 600;
  settings.gameplayHispeed = 2.75F;
  settings.hispeedFixMode = AppSettings::HiSpeedFixMode::Max;
  settings.customJudge = true;
  settings.showJudgeArea = true;
  settings.markProcessedNotes = true;
  settings.notesDisplayTimingAutoAdjust = true;
  settings.notesDisplayTimingMilliseconds = -8;
  settings.autoSaveReplay = {1, 2, 3, 4};
  settings.guideSoundEffects = true;
  settings.extraNoteDepth = 3;
  settings.mineMode = 1;
  settings.scrollMode = 2;
  settings.longNoteModifierMode = 4;
  settings.sevenToNinePattern = 5;
  settings.sevenToNineType = 6;
  settings.constantScroll = true;
  settings.bgaEnabled = false;
  settings.laneCoverEnabled = true;
  settings.liftEnabled = true;
  settings.hiddenEnabled = false;
  settings.hispeedAutoAdjust = true;
  settings.notePriorityMode = AppSettings::NotePriorityMode::Duration;
  settings.audioVideo.audio.masterVolume = 0.75F;
  settings.audioVideo.audio.keysoundVolume = 0.5F;
  settings.audioVideo.audio.bgmVolume = 0.25F;

  MusicSelectBarManagerSnapshot bars;
  (void)selectedSong(bars);
  MusicSelectPropertyRuntimeSnapshot runtime;
  runtime.wallClock = {.year = 2026,
                       .month = 9,
                       .day = 2,
                       .hour = 17,
                       .minute = 4,
                       .second = 5};
  runtime.applicationUptimeMillis = 3'723'000;
  runtime.framesPerSecond = 144;
  runtime.panelState = 2;
  runtime.selectedReplay = 2;
  runtime.sortIndex = 6;
  runtime.playerName = "Player";
  runtime.targetName = "MAX";
  runtime.rivalName = "Rival";
  runtime.searchWord = "needle";
  runtime.tableName = "Table";
  runtime.tableLevel = "★12";
  runtime.tableFullName = "Table ★12";
  runtime.version = "version";
  runtime.irName = "IR";
  runtime.irUserName = "IR Player";
  runtime.irOnline = true;
  runtime.updateScore = false;
  runtime.playerHistory = {.playCount = 9,
                           .clearCount = 7,
                           .judgementCounts = {11, 12, 13, 14, 15},
                           .playDurationSeconds = 7'323};

  const auto values =
      projectMusicSelectProperties(settings, bars, runtime);

  require(values.strings.at(1) == "Rival" &&
              values.strings.at(2) == "Player" &&
              values.strings.at(3) == "MAX" &&
              values.strings.at(10) == "Title" &&
              values.strings.at(12) == "Title Subtitle" &&
              values.strings.at(16) == "Artist Sub Artist" &&
              values.strings.at(1000) == "Root > Folder > ",
          "StringPropertyFactory values follow the selected SongData and selector");
  require(values.booleans.at(2) && !values.booleans.at(1) &&
              values.booleans.at(5) && values.booleans.at(1102) &&
              values.booleans.at(197) && !values.booleans.at(1197) &&
              values.booleans.at(1200) && values.booleans.at(1207) &&
              values.booleans.at(22) && values.booleans.at(625) &&
              values.booleans.at(60) && !values.booleans.at(61),
          "BooleanPropertyFactory selector, clear, replay, panel, rival, and save flags are exact");
  require(values.integers.at(20) == 144 &&
              values.integers.at(21) == 2026 &&
              values.integers.at(22) == 9 &&
              values.integers.at(27) == 1 &&
              values.integers.at(28) == 2 &&
              values.integers.at(29) == 3 &&
              values.integers.at(30) == 9 &&
              values.integers.at(31) == 7 &&
              values.integers.at(32) == 2 &&
              values.integers.at(333) == 50,
          "ValueType clock and PlayerData values use source integer arithmetic");
  require(values.integers.at(71) == 800 &&
              values.integers.at(72) == 1000 &&
              values.integers.at(74) == 500 &&
              values.integers.at(75) == 321 &&
              values.integers.at(76) == 12 &&
              values.integers.at(77) == 9 &&
              values.integers.at(78) == 7 &&
              values.integers.at(79) == 2 &&
              values.integers.at(90) == 180 &&
              values.integers.at(91) == 120 &&
              values.integers.at(92) == 150 &&
              values.integers.at(96) == 12 &&
              values.integers.at(102) == 80 &&
              values.integers.at(103) == 0 &&
              values.integers.at(121) == 750 &&
              values.integers.at(271) == 750 &&
              values.integers.at(1163) == 2 &&
              values.integers.at(1164) == 34,
          "selected score, chart, rival, rate, BPM, and length integers match ValueType");
  require(values.imageIndexes.at(10) == 4 &&
              values.imageIndexes.at(11) == 4 &&
              values.imageIndexes.at(12) == 6 &&
              values.imageIndexes.at(40) == 3 &&
              values.imageIndexes.at(42) == 3 &&
              values.imageIndexes.at(43) == 9 &&
              values.imageIndexes.at(54) == 3 &&
              values.imageIndexes.at(55) == 2 &&
              values.imageIndexes.at(72) == 2 &&
              values.imageIndexes.at(73) == 1 &&
              values.imageIndexes.at(78) == 3 &&
              values.imageIndexes.at(301) == 1 &&
              values.imageIndexes.at(306) == 1 &&
              values.imageIndexes.at(308) == 1 &&
              values.imageIndexes.at(340) == 1 &&
              values.imageIndexes.at(400) == 1,
          "IndexType uses pinned filter, option, gauge, and PlayConfig numbering");
  require(values.rates.at(1) == 0.0 &&
              values.rates.at(103) == 1.2 &&
              values.rates.at(105) == 0.0 &&
              values.rates.at(108) == 1.2 &&
              values.rates.at(110) == 0.8 &&
              values.rates.at(113) == 0.0 &&
              values.rates.at(115) == 0.75 &&
              values.rates.at(140) == 0.5 &&
              values.rates.at(141) == 0.2 &&
              values.rates.at(142) == 0.1 &&
              values.rates.at(143) == 0.05 &&
              values.rates.at(144) == 0.02 &&
              values.rates.at(145) == 0.642 &&
              values.rates.at(147) == 0.8 &&
              values.floats.at(310) == 2.75 &&
              values.rates.at(17) == 0.75 &&
              values.rates.at(18) == 0.5 &&
              values.rates.at(19) == 0.25,
          "FloatPropertyFactory rates retain their source domains");
}

void testProjectsCourseContract() {
  AppSettings settings;
  MusicSelectBar course;
  course.kind = skin::MusicSelectBarKind::Grade;
  course.title = "Course";
  course.presentation = {.kind = skin::MusicSelectBarKind::Grade,
                         .exists = true};
  course.courseStages = {
      {.title = "One", .hasPath = true},
      {.title = std::nullopt, .hasPath = false},
  };
  course.courseConstraints = {
      skin::MusicSelectCourseConstraint::Class,
      skin::MusicSelectCourseConstraint::NoGood,
      skin::MusicSelectCourseConstraint::Gauge7Keys,
      skin::MusicSelectCourseConstraint::Hcn,
  };
  MusicSelectBarManagerSnapshot bars;
  bars.rows.push_back(std::move(course));
  const auto values = projectMusicSelectProperties(
      settings, bars, MusicSelectPropertyRuntimeSnapshot{});
  require(values.strings.at(150) == "One" &&
              values.strings.at(151) == "(no song) ----",
          "GradeBar course titles preserve missing-song source text");
  require(values.booleans.at(1002) && values.booleans.at(1006) &&
              values.booleans.at(1012) && values.booleans.at(1017) &&
              !values.booleans.at(1003) && !values.booleans.at(1015),
          "GradeBar constraints project the exact CourseData enum options");
}

void testProjectsExactBarClassConditions() {
  AppSettings settings;
  MusicSelectPropertyRuntimeSnapshot runtime;
  constexpr std::array directoryKinds{
      skin::MusicSelectBarKind::Folder,
      skin::MusicSelectBarKind::Table,
      skin::MusicSelectBarKind::Hash,
      skin::MusicSelectBarKind::Command,
      skin::MusicSelectBarKind::Container,
      skin::MusicSelectBarKind::SearchWord,
      skin::MusicSelectBarKind::SameFolder,
  };
  for (const auto kind : directoryKinds) {
    MusicSelectBarManagerSnapshot bars;
    bars.rows.push_back({.kind = kind,
                         .presentation = {.kind = kind, .exists = true},
                         .selectable = true});
    const auto values = projectMusicSelectProperties(settings, bars, runtime);
    require(values.booleans.at(1) && !values.booleans.at(5),
            "every and only DirectoryBar subtype is a non-playable folder bar");
  }

  for (const auto kind : {skin::MusicSelectBarKind::Grade,
                          skin::MusicSelectBarKind::RandomCourse,
                          skin::MusicSelectBarKind::Executable}) {
    MusicSelectBarManagerSnapshot bars;
    bars.rows.push_back({.kind = kind,
                         .presentation = {.kind = kind, .exists = true}});
    const auto values = projectMusicSelectProperties(settings, bars, runtime);
    require(!values.booleans.at(1) && values.booleans.at(5),
            "playable course and executable bar classes follow Beatoraja");
  }

  MusicSelectBarManagerSnapshot unplayed;
  MusicSelectBar song;
  song.kind = skin::MusicSelectBarKind::Song;
  song.presentation = {.kind = skin::MusicSelectBarKind::Song,
                       .exists = true,
                       .lamp = 0};
  song.chart.emplace();
  unplayed.rows.push_back(std::move(song));
  auto values = projectMusicSelectProperties(settings, unplayed, runtime);
  require(values.booleans.at(5) && values.booleans.at(100),
          "an existing unplayed SongBar is playable and not played");
  unplayed.rows.front().presentation.lamp = 1;
  unplayed.rows.front().score.emplace();
  values = projectMusicSelectProperties(settings, unplayed, runtime);
  require(!values.booleans.at(100),
          "a SongBar with a non-NoPlay clear is not unplayed");
}

void testProjectsDirectoryAndFinishedRanking() {
  AppSettings settings;
  MusicSelectBar folder;
  folder.kind = skin::MusicSelectBarKind::Folder;
  folder.title = "Folder";
  folder.presentation.kind = skin::MusicSelectBarKind::Folder;
  folder.presentation.folderLampCounts = {1, 2, 3, 4, 5, 6,
                                           7, 8, 9, 10, 11};
  MusicSelectBarManagerSnapshot bars;
  bars.rows.push_back(std::move(folder));
  MusicSelectPropertyRuntimeSnapshot runtime;
  runtime.irOnline = true;
  runtime.ranking.state = MusicSelectRankingState::Finish;
  runtime.ranking.rank = 12;
  runtime.ranking.totalPlayers = 34;
  runtime.ranking.offset = 1;
  runtime.ranking.pendingDurationMillis = 4'200;
  runtime.ranking.clearCounts = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  runtime.ranking.entries.push_back(
      {.name = "Hidden", .score = 1, .rank = 2});
  runtime.ranking.entries.push_back(
      {.name = "Top", .score = 999, .rank = 1});

  const auto values =
      projectMusicSelectProperties(settings, bars, runtime);
  require(values.booleans.at(1) && !values.booleans.at(2) &&
              values.integers.at(300) == 66 &&
              values.integers.at(320) == 1 &&
              values.integers.at(330) == 11,
          "DirectoryBar lamp totals use the exact folder ranges");
  require(values.booleans.at(51) && !values.booleans.at(50) &&
              !values.booleans.at(603) && !values.booleans.at(604) &&
              values.integers.at(179) == 12 &&
              values.integers.at(180) == 34 &&
              values.integers.at(220) == 5 &&
              values.integers.at(380) == 999 &&
              values.integers.at(390) == 1 &&
              values.strings.at(120) == "Top",
          "finished RankingData projects selector IR state and first row");
  require(values.rates.at(8) == 1.0 / 34.0,
          "ranking position divides the offset by the remote total player count");
}

} // namespace

int main(int argc, char **argv) {
  testProjectsSelectedSongAndPlayerConfiguration();
  testProjectsDirectoryAndFinishedRanking();
  testProjectsExactBarClassConditions();
  testProjectsCourseContract();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_property_projection_tests", failures,
      "music-select property projection assertion(s) failed",
      "music-select property projection tests passed");
}
