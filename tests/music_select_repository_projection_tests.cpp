#include "music_select/MusicSelectRepositoryProjection.h"

#include "music_select_runtime_ledger_assertions.h"

#include "path.h"
#include "scene/play/GameplayGaugeTypes.h"

#include <iostream>
#include <string_view>
#include <stdexcept>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

ChartMetaRecord chart(std::string path, std::string sha256,
                      std::string title, std::string subtitle,
                      std::string folder) {
  ChartMetaRecord result;
  result.meta.BmsPath = std::move(path);
  result.meta.Folder = std::move(folder);
  result.meta.SHA256 = std::move(sha256);
  result.meta.Title = std::move(title);
  result.meta.SubTitle = std::move(subtitle);
  result.meta.PlayLevel = 12;
  result.meta.Difficulty = 4;
  result.meta.KeyMode = 7;
  result.meta.TotalLongNotes = 3;
  result.meta.LnMode = 2;
  result.meta.TotalLandmineNotes = 1;
  result.hasRandomSequence = true;
  result.meta.TotalNotes = 400;
  result.addDateSeconds = 1'700'000'000;
  return result;
}

const MusicSelectBar *child(const MusicSelectProjection &projection,
                            const MusicSelectBar *directory,
                            std::size_t index) {
  if (directory == nullptr || index >= directory->children.size()) return nullptr;
  return projection.find(directory->children[index]);
}

void testProjectsFoldersSongsScoresAndSourceFlags() {
  std::vector<ChartMetaRecord> records{
      chart("/songs/a/a.bms", "aaa", "Alpha", "Another", "/songs/a"),
      chart("/songs/b/b.bms", "bbb", "Beta", "", "/songs/b")};
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [](const bms_parser::ChartMeta &meta, int mode) {
         return meta.SHA256 == "aaa" && mode == 2
                    ? std::optional<ScoreBestSnapshot>(ScoreBestSnapshot{
                          .score = 1000,
                          .maxScore = 1200,
                          .clearType = kClearTypeExHardClearRank})
                    : std::nullopt;
       },
       .replayExistsFor = [](const ChartMetaRecord &record, int mode) {
         return record.meta.SHA256 == "aaa" && mode == 2
                    ? std::array<bool, 4>{true, false, true, false}
                    : std::array<bool, 4>{};
       },
       .selectedLongNoteMode = 2,
       .repositoryRevision = 9});
  require(projection.repositoryRevision == 9 && projection.root.size() == 4,
          "physical folders precede COURSE and update containers");
  const auto *folder = projection.find(projection.root.front());
  require(folder && folder->kind == skin::MusicSelectBarKind::Folder &&
              folder->children.size() == 2 && folder->selectable &&
              folder->directoryPath == "/songs",
          "physical folders group charts by SongData.parent");
  const auto *song = child(projection, folder, 1);
  require(song && song->kind == skin::MusicSelectBarKind::Song &&
              song->title == "Alpha Another" && song->chart &&
              song->presentation.exists && song->presentation.level == 12 &&
              song->presentation.difficulty == 4 &&
              song->presentation.addDateSeconds == 1'700'000'000,
          "SongBar values use SongData full-title and chart fields");
  require(song && song->presentation.lamp == 7 &&
              song->score && song->score->score == 1000 &&
              song->replayExists ==
                  std::array<bool, 4>{true, false, true, false} &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureChargeNote) != 0 &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureMine) != 0 &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureRandom) != 0,
          "score lamps and source feature bits use Beatoraja IDs");
  require(folder && folder->presentation.folderLampCounts[7] == 1,
          "DirectoryBar aggregates its child clear lamps");
}

void testOverlappingConfiguredRootsFormOnePhysicalHierarchy() {
  std::vector<ChartMetaRecord> records{
      chart("/songs/child/a.bms", "child", "Child", "", "/songs/child"),
      chart("/songs/root.bms", "root", "Root", "", "/songs")};
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/songs/child")});
  metadata.entries.push_back({.path = utf8_to_path_t("/songs")});
  metadata.entries.push_back({.path = utf8_to_path_t("/songs/empty")});
  metadata.folders.push_back(
      {.path = utf8_to_path_t("/songs"), .dateSeconds = 50,
       .addDateSeconds = 1'700'001'111});

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &metadata});

  require(projection.root.size() == 4,
          "overlapping configured folders project one physical root plus the "
          "three source command roots");
  const auto *root = projection.find(projection.root.front());
  require(root && root->directoryPath == "/songs" &&
              root->children.size() == 2 &&
              child(projection, root, 0)->title == "Root" &&
              child(projection, root, 1)->title == "Child" &&
              root->presentation.addDateSeconds == 1'700'001'111,
          "mixed physical folders flatten charts and retain FolderData.adddate");
}

void testProjectsPersistedEmptyFolderBars() {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/songs")});
  metadata.folders = {
      {.path = utf8_to_path_t("/songs"), .dateSeconds = 1,
       .addDateSeconds = 2},
      {.path = utf8_to_path_t("/songs/empty"), .dateSeconds = 3,
       .addDateSeconds = 4},
  };

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = {}, .metadata = &metadata});
  const auto *root = projection.find(projection.root.front());
  const auto *empty = child(projection, root, 0);
  require(root && root->directoryPath == "/songs" && empty &&
              empty->kind == skin::MusicSelectBarKind::Folder &&
              empty->directoryPath == "/songs/empty" &&
              empty->presentation.addDateSeconds == 4,
          "FolderBar exposes persisted empty FolderData children with their "
          "source adddate");
}

void testProjectsExactRootHierarchyTablesCoursesAndCommands() {
  std::vector<ChartMetaRecord> records{
      chart("/songs/A/first.bms", "first", "First", "", "/songs/A"),
      chart("/songs/A/second.bms", "second", "Second", "", "/songs/A"),
      chart("/songs/A/duplicate.bms", "first", "Duplicate", "", "/songs/A"),
      chart("/songs/A/sub/nested.bms", "nested", "Nested", "", "/songs/A/sub")};

  auto tableFirst = chart("/songs/T/one.bms", "table-one", "Table One", "", "/songs/T");
  auto tableMissing = chart("", "table-missing", "Table Missing", "", "");
  tableMissing.unavailable = true;
  auto courseFirst = chart("/songs/C/one.bms", "course-one", "Course One", "", "/songs/C");
  courseFirst.meta.TotalNotes = 700;
  auto courseMissing = chart("", "course-missing", "Course Missing", "", "");
  courseMissing.meta.TotalNotes = 300;
  courseMissing.meta.LnMode = 3;
  courseMissing.unavailable = true;

  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/songs")});
  MusicSelectDifficultyTableSource table{
      .info = {.id = 42,
               .name = "Satellite",
               .symbol = "sl",
               .sourceUrl = "https://example.invalid/table"}};
  table.levels.push_back(
      {.info = {.tableId = 42,
                .tableName = "Satellite",
                .tableSymbol = "sl",
                .level = "12"},
       .records = {tableFirst, tableMissing}});
  table.courses.push_back(
      {.info = {.id = 73,
                .courseKey = "course-key",
                .tableId = 42,
                .tableName = "Satellite",
                .groupName = "GRADE",
                .level = "1",
                .name = "Course",
                .constraintJson = R"(["grade_mirror","no_speed"])",
                .trophies = {{.name = "bronzemedal",
                              .missRate = 8.0,
                              .scoreRate = 55.0},
                             {.name = "silvermedal",
                              .missRate = 5.0,
                              .scoreRate = 70.0},
                             {.name = "goldmedal",
                              .missRate = 2.5,
                              .scoreRate = 85.0}}},
       .stages = {courseFirst, courseMissing}});
  metadata.tables.push_back(std::move(table));

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records,
       .scoreFor = [](const bms_parser::ChartMeta &meta, int mode) {
         if (meta.SHA256 == "first" && mode == 2) {
           return std::optional<ScoreBestSnapshot>(ScoreBestSnapshot{
               .score = 1000,
               .maxScore = 1200,
               .clearType = kClearTypeExHardClearRank});
         }
         if (meta.SHA256 == "table-one") {
           return std::optional<ScoreBestSnapshot>(ScoreBestSnapshot{
               .score = 600,
               .maxScore = 800,
               .clearType = kClearTypeHardClearRank});
         }
         return std::optional<ScoreBestSnapshot>{};
       },
       .courseScoresFor = [](std::string_view key, int id, int mode, bool) {
         MusicSelectCourseOptionScores scores;
         if (key == "course-key" && id == 73 && mode == 2) {
           scores[0] = ScoreBestSnapshot{
               .score = 1'450,
               .maxScore = 2'000,
               .badPoints = 40,
               .clearType = kClearTypeHardClearRank};
           scores[2] = ScoreBestSnapshot{
               .score = 1'720,
               .maxScore = 2'200,
               .badPoints = 20,
               .clearType = kClearTypeNormalClearRank};
         }
         return scores;
       },
       .metadata = &metadata,
       .modeFilter = "ALL",
       .selectedLongNoteMode = 2,
       .repositoryRevision = 19});

  require(projection.root.size() == 5,
          "root has physical, COURSE, imported table, and two update bars");
  const auto *physical = projection.find(projection.root[0]);
  const auto *courses = projection.find(projection.root[1]);
  const auto *imported = projection.find(projection.root[2]);
  const auto *lampUpdate = projection.find(projection.root[3]);
  const auto *scoreUpdate = projection.find(projection.root[4]);
  require(physical && physical->kind == skin::MusicSelectBarKind::Folder &&
              physical->title == "songs" && physical->children.size() == 3,
          "configured chart entry becomes the physical root");
  require(courses && courses->kind == skin::MusicSelectBarKind::Table &&
              courses->title == "COURSE" && courses->children.empty(),
          "local COURSE TableBar follows physical roots");
  require(imported && imported->kind == skin::MusicSelectBarKind::Table &&
              imported->title == "Satellite" && imported->tableId == 42 &&
              imported->tableUrl == "https://example.invalid/table" &&
              imported->children.size() == 2 && imported->sortable,
          "difficulty TableBar preserves repository identity, metadata order, "
          "and URL");

  const auto *folder = physical;
  require(folder && folder->title == "songs" && folder->children.size() == 3 &&
              child(projection, folder, 0)->title == "Nested",
          "mixed FolderBar includes nested charts in the flat song list");
  const auto *second = child(projection, folder, 1);
  const auto *first = child(projection, folder, 2);
  require(second && first && second->title == "Second" &&
              first->title == "First",
          "physical SongBars deduplicate first SHA occurrence and reverse "
          "order without appending nested folders");
  require(folder && folder->presentation.folderLampCounts[0] == 2 &&
              folder->presentation.folderLampCounts[7] == 2 &&
              folder->presentation.folderRankCounts[0] == 2 &&
              folder->presentation.folderRankCounts[22] == 2 &&
              folder->presentation.lamp == 0,
          "DirectoryBar status counts source rows before SongBar deduplication");

  const auto *level = child(projection, imported, 0);
  const auto *grade = child(projection, imported, 1);
  require(level && level->kind == skin::MusicSelectBarKind::Hash &&
              level->title == "sl12" && level->tableLevel == "12" &&
              level->children.size() == 2 && level->sortable &&
              child(projection, level, 0)->title == "Table One" &&
              child(projection, level, 1)->title == "Table Missing",
          "HashBar keeps table element order including unavailable songs");
  require(grade && grade->kind == skin::MusicSelectBarKind::Grade &&
              grade->courseId == 73 && grade->courseKey == "course-key" &&
              grade->courseGroupName == "GRADE" &&
              grade->courseConstraintJson ==
                  R"(["grade_mirror","no_speed"])" &&
              grade->courseCharts.size() == 2 &&
              grade->courseCharts[0].meta.Title == "Course One" &&
              grade->courseStages.size() == 2 &&
              grade->courseTotalNotes == 1000 &&
              !grade->presentation.exists && grade->presentation.lamp == 6 &&
              grade->presentation.trophyName == "silvermedal" &&
              grade->courseConstraints.size() == 2 &&
              grade->courseConstraints[0] ==
                  skin::MusicSelectCourseConstraint::Mirror &&
              grade->courseConstraints[1] ==
                  skin::MusicSelectCourseConstraint::NoSpeed &&
              (grade->presentation.featureFlags &
               skin::MusicSelectFeatureHellChargeNote) != 0,
          "GradeBar preserves stages, constraints, features, existence, and rank");

  require(lampUpdate && scoreUpdate &&
              lampUpdate->kind == skin::MusicSelectBarKind::Container &&
              scoreUpdate->kind == skin::MusicSelectBarKind::Container &&
              lampUpdate->title == "LAMP UPDATE" &&
              scoreUpdate->title == "SCORE UPDATE" &&
              lampUpdate->children.size() == 30 &&
              scoreUpdate->children.size() == 30 &&
              child(projection, lampUpdate, 0)->title == "TODAY" &&
              child(projection, lampUpdate, 1)->title == "1DAYS AGO" &&
              child(projection, lampUpdate, 29)->title == "29DAYS AGO",
          "update ContainerBars expose the exact thirty command titles");
}

void testProjectsReplaySlotsForCompleteGrades() {
  auto first =
      chart("/songs/C/one.bms", "course-one", "One", "", "/songs/C");
  auto second =
      chart("/songs/C/two.bms", "course-two", "Two", "", "/songs/C");
  MusicSelectDifficultyTableSource table{
      .info = {.id = 9, .name = "Table"}};
  table.courses.push_back(
      {.info = {.id = 73,
                .courseKey = "course-key",
                .tableId = 9,
                .name = "Course"},
       .stages = {first, second}});
  MusicSelectRepositoryMetadata metadata;
  metadata.tables.push_back(std::move(table));

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = {},
       .courseReplayExistsFor = [](const MusicSelectBar &bar, int mode) {
         return bar.courseKey == "course-key" && bar.courseId == 73 &&
                        bar.courseCharts.size() == 2 && mode == 2
                    ? std::array<bool, 4>{false, true, false, true}
                    : std::array<bool, 4>{};
       },
       .metadata = &metadata,
       .selectedLongNoteMode = 2});
  const auto *imported = projection.find({"table:9"});
  const auto *grade = child(projection, imported, 0);
  require(grade && grade->presentation.exists &&
              grade->replayExists ==
                  std::array<bool, 4>{false, true, false, true},
          "complete GradeBars project the exact four course replay slots");
}

void testEmptyImportedCourseIsUnavailable() {
  MusicSelectDifficultyTableSource table{
      .info = {.id = 9, .name = "Table"}};
  table.courses.push_back(
      {.info = {.id = 74,
                .courseKey = "empty-course",
                .tableId = 9,
                .name = "Empty Course"},
       .stages = {}});
  MusicSelectRepositoryMetadata metadata;
  metadata.tables.push_back(std::move(table));

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = {}, .metadata = &metadata});
  const auto *imported = projection.find({"table:9"});
  const auto *grade = child(projection, imported, 0);
  require(grade && grade->courseCharts.empty() &&
              !grade->presentation.exists,
          "an imported GradeBar without stages is unavailable");
}

void testProjectionOwnsItsRepositoryValues() {
  std::vector<ChartMetaRecord> records{
      chart("/songs/a/a.bms", "stable", "Before", "", "/songs/a")};
  auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .repositoryRevision = 1});
  records.front().meta.Title = "After";
  records.clear();
  const auto *folder = projection.find(projection.root.front());
  const auto *song = projection.find(folder->children.front());
  require(song && song->title == "Before" && song->chart &&
              song->chart->meta.Title == "Before",
          "projection never retains repository row references");
}

void testProjectsSearchHistoryAfterCommands() {
  auto first = chart("/songs/a.bms", "first", "First", "", "/songs");
  auto second = chart("/songs/b.bms", "second", "Second", "", "/songs");
  auto duplicate = chart("/songs/c.bms", "first", "Duplicate", "", "/songs");
  std::vector<MusicSelectSearchSource> searches{
      {.text = "needle", .records = {first, second, duplicate}}};
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = {}, .searches = searches});
  require(projection.root.size() == 4,
          "search history follows COURSE and both update containers");
  const auto *search = projection.find(projection.root.back());
  require(search && search->kind == skin::MusicSelectBarKind::SearchWord &&
              search->title == "Search : 'needle'" &&
              search->children.size() == 2 &&
              child(projection, search, 0)->title == "Second" &&
              child(projection, search, 1)->title == "First",
          "SearchWordBar uses physical SongBar deduplication and reversal");
}

void testProjectsRecentScoreImprovementCommandChildren() {
  std::vector<ChartMetaRecord> records{
      chart("/songs/a.bms", "first", "First", "", "/songs"),
      chart("/songs/b.bms", "second", "Second", "", "/songs"),
      chart("/copy/a.bms", "first", "Copy", "", "/copy")};
  RecentScoreImprovements updates;
  updates.lamp[0].insert("first");
  updates.score[1].insert("second");
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .recentScoreImprovements = &updates});
  const auto *lamp = projection.find({"container:lamp-update"});
  const auto *score = projection.find({"container:score-update"});
  const auto *today = child(projection, lamp, 0);
  const auto *yesterday = child(projection, score, 1);
  require(today && today->children.size() == 1 &&
              child(projection, today, 0)->title == "First" &&
              yesterday && yesterday->children.size() == 1 &&
              child(projection, yesterday, 0)->title == "Second",
          "update CommandBars expose the charts whose lamp or score first "
          "improved during the exact UTC day");
  require(today && today->presentation.folderLampCounts[0] == 2,
          "command status counts duplicate source paths while rows deduplicate");
}

void testRootProjectionDefersDirectoryContents() {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/songs/")});
  metadata.tables.push_back(
      {.info = {.id = 42, .name = "Satellite", .symbol = "sl"}});
  const std::vector<std::string> searches{"needle"};

  const auto projection = MusicSelectRepositoryProjection{}.projectRoot(
      metadata, searches, 88);
  const auto *folder = projection.find({"folder:/songs"});
  const auto *table = projection.find({"table:42"});
  const auto *search = projection.find({"search:needle"});
  require(projection.repositoryRevision == 88 && folder && table && search &&
              folder->children.empty() && table->children.empty() &&
              search->children.empty() && !folder->childrenLoaded &&
              !table->childrenLoaded && !search->childrenLoaded,
          "root projection retains only unopened DirectoryBar descriptors");
  require(std::ranges::none_of(projection.bars, [](const auto &bar) {
            return bar.kind == skin::MusicSelectBarKind::Song ||
                   bar.kind == skin::MusicSelectBarKind::Hash ||
                   bar.kind == skin::MusicSelectBarKind::Grade ||
                   bar.kind == skin::MusicSelectBarKind::Command;
          }),
          "root projection does not materialize table, command, or song children");
  const auto eager = MusicSelectRepositoryProjection{}.project({.metadata = &metadata});
  require(eager.find({"folder:/songs"}) != nullptr,
          "trailing root separators do not create a self-referencing hierarchy");
}

void testMixedFolderFlattensDescendantsAndStatus() {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/pack")});
  std::vector<ChartMetaRecord> records{
      chart("/pack/direct.bms", "direct", "Direct", "", "/pack"),
      chart("/pack/song/chart.bms", "song", "Song", "", "/pack/song"),
      chart("/pack/copy/chart.bms", "song", "Copy", "", "/pack/copy"),
      chart("/pack/sub/song/deep.bms", "deep", "Deep", "", "/pack/sub/song")};
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &metadata});
  const auto *folder = projection.find({"folder:/pack"});
  require(folder && folder->presentation.folderLampCounts[0] == 4 &&
              folder->presentation.folderRankCounts[0] == 4,
          "mixed folder status includes direct files and recursive descendants");
  require(folder && folder->children.size() == 3 &&
              child(projection, folder, 0)->title == "Deep" &&
              child(projection, folder, 1)->title == "Song" &&
              child(projection, folder, 2)->title == "Direct",
          "mixed folders flatten descendants with SongBar deduplication");
  require(std::ranges::all_of(folder->children, [&](const auto &id) {
            return projection.find(id)->kind == skin::MusicSelectBarKind::Song;
          }), "flattened lists never introduce subfolder rows");
}

void testCategoryOnlyFolderKeepsSubfolderNavigation() {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = utf8_to_path_t("/pack")});
  const std::vector<ChartMetaRecord> records{
      chart("/pack/category/song/chart.bms", "song", "Song", "",
            "/pack/category/song")};
  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &metadata});
  const auto *root = projection.find({"folder:/pack"});
  const auto *category = child(projection, root, 0);
  require(root && root->children.size() == 1 && category &&
              category->kind == skin::MusicSelectBarKind::Folder &&
              category->directoryPath == "/pack/category" &&
              category->children.size() == 1 &&
              child(projection, category, 0)->title == "Song" &&
              root->presentation.folderLampCounts == std::array<int, 11>{},
          "folders without immediate song entries retain Beatoraja navigation");
}

void testFolderStatusCancellationStopsAggregationWithoutPartialPublication() {
  std::vector<ChartMetaRecord> records(10000,
      chart("/songs/a/a.bms", "aaa", "Alpha", "", "/songs/a"));
  MusicSelectBar folder{.kind = skin::MusicSelectBarKind::Folder};
  folder.presentation.folderRankCounts[0] = 17;
  std::stop_source cancellation;
  int scoreReads = 0;
  bool cancelled = false;
  try {
    MusicSelectRepositoryProjection::updateFolderStatus(folder,
        {.records = records,
         .scoreFor = [&](const bms_parser::ChartMeta &, int) {
           if (++scoreReads == 3) cancellation.request_stop();
           return std::optional<ScoreBestSnapshot>{};
         }}, cancellation.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  require(cancelled && scoreReads == 3,
          "cancellation interrupts aggregation between score lookups");
  require(folder.presentation.folderRankCounts[0] == 17,
          "cancelled aggregation never publishes partial counts");
}

void testFolderStatusReplacesCountsAndFiltersOnlyMode() {
  std::vector<ChartMetaRecord> records{
      chart("/pack/song/chart.bms", "scored", "Scored", "", "/pack/song"),
      chart("/pack/song/copy.bms", "scored", "Copy", "", "/pack/song"),
      chart("/pack/other/chart.bms", "unplayed", "Unplayed", "", "/pack/other"),
      chart("/pack/dp/chart.bms", "dp", "DP", "", "/pack/dp"),
      chart("", "missing", "Missing", "", "")};
  records[0].songReviewFavorite = 2;
  records[1].meta.Difficulty = 1;
  records[3].meta.IsDP = true;
  records[4].unavailable = true;
  MusicSelectBar folder{.kind = skin::MusicSelectBarKind::Folder};
  MusicSelectRepositoryProjectionInput input{
      .records = records,
      .scoreFor = [](const bms_parser::ChartMeta &meta, int mode) {
        if (meta.SHA256 != "scored") return std::optional<ScoreBestSnapshot>{};
        return std::optional<ScoreBestSnapshot>{{
            .score = mode == 2 ? 800 : 600,
            .maxScore = 800,
            .clearType = mode == 2 ? kClearTypeFullComboRank
                                   : kClearTypeHardClearRank}};
      },
      .modeFilter = "7KEY",
      .selectedLongNoteMode = 1};
  MusicSelectRepositoryProjection::updateFolderStatus(folder, input);
  require(folder.presentation.folderLampCounts[0] == 1 &&
              folder.presentation.folderLampCounts[6] == 2 &&
              folder.presentation.folderRankCounts[20] == 2,
          "folder status includes hidden/different-difficulty/duplicate records "
          "but excludes missing and wrong-mode records");
  input.selectedLongNoteMode = 2;
  MusicSelectRepositoryProjection::updateFolderStatus(folder, input);
  require(folder.presentation.folderLampCounts[6] == 0 &&
              folder.presentation.folderLampCounts[8] == 2 &&
              folder.presentation.folderRankCounts[27] == 2 &&
              folder.presentation.folderRankCounts[20] == 0,
          "LN changes replace rather than accumulate lamp and rank arrays");
  input.modeFilter = "14KEY";
  MusicSelectRepositoryProjection::updateFolderStatus(folder, input);
  require(folder.presentation.folderLampCounts[0] == 1 &&
              folder.presentation.folderLampCounts[8] == 0,
          "a mode change removes the previous mode's folder status");
  for (const auto kind : {skin::MusicSelectBarKind::Table,
                          skin::MusicSelectBarKind::Container,
                          skin::MusicSelectBarKind::SameFolder}) {
    MusicSelectBar noStatus{.kind = kind};
    MusicSelectRepositoryProjection::updateFolderStatus(noStatus, input);
    require(noStatus.presentation.folderLampCounts == std::array<int, 11>{},
            "directory subclasses without updateFolderStatus remain empty");
  }
}

} // namespace

int main(int argc, char **argv) {
  testFolderStatusReplacesCountsAndFiltersOnlyMode();
  testFolderStatusCancellationStopsAggregationWithoutPartialPublication();
  testMixedFolderFlattensDescendantsAndStatus();
  testCategoryOnlyFolderKeepsSubfolderNavigation();
  testProjectsFoldersSongsScoresAndSourceFlags();
  testOverlappingConfiguredRootsFormOnePhysicalHierarchy();
  testProjectsPersistedEmptyFolderBars();
  testProjectsExactRootHierarchyTablesCoursesAndCommands();
  testProjectsReplaySlotsForCompleteGrades();
  testEmptyImportedCourseIsUnavailable();
  testProjectionOwnsItsRepositoryValues();
  testProjectsSearchHistoryAfterCommands();
  testProjectsRecentScoreImprovementCommandChildren();
  testRootProjectionDefersDirectoryContents();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_repository_projection_tests", failures,
      "music-select repository projection assertion(s) failed",
      "music-select repository projection tests passed");
}
