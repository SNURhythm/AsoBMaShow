#include "music_select/MusicSelectRepositoryProjection.h"

#include "music_select_runtime_ledger_assertions.h"

#include "path.h"
#include "scene/play/GameplayGaugeTypes.h"

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
  require(projection.repositoryRevision == 9 && projection.root.size() == 5,
          "physical folders precede COURSE and update containers");
  const auto *folder = projection.find(projection.root.front());
  require(folder && folder->kind == skin::MusicSelectBarKind::Folder &&
              folder->children.size() == 1 && folder->selectable &&
              folder->directoryPath == "/songs/a",
          "each physical folder projects its exact selectable directory");
  const auto *song = folder ? projection.find(folder->children.front()) : nullptr;
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

  const auto projection = MusicSelectRepositoryProjection{}.project(
      {.records = records, .metadata = &metadata});

  require(projection.root.size() == 4,
          "overlapping configured folders project one physical root plus the "
          "three source command roots");
  const auto *root = projection.find(projection.root.front());
  require(root && root->directoryPath == "/songs" &&
              root->children.size() == 3,
          "the configured ancestor owns its direct song and descendant folder");
  const auto *nested = child(projection, root, 1);
  require(nested && nested->directoryPath == "/songs/child" &&
              nested->children.size() == 1 &&
              child(projection, nested, 0)->title == "Child",
          "the configured descendant appears only below its ancestor");
  const auto *empty = child(projection, root, 2);
  require(empty && empty->directoryPath == "/songs/empty" &&
              empty->children.empty(),
          "an empty configured descendant remains reachable below its "
          "configured ancestor");
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
              physical->title == "songs" && physical->children.size() == 1,
          "configured chart entry becomes the physical root");
  require(courses && courses->kind == skin::MusicSelectBarKind::Table &&
              courses->title == "COURSE" && courses->children.empty(),
          "local COURSE TableBar follows physical roots");
  require(imported && imported->kind == skin::MusicSelectBarKind::Table &&
              imported->title == "Satellite" && imported->tableId == 42 &&
              imported->tableUrl == "https://example.invalid/table" &&
              imported->children.size() == 2,
          "difficulty TableBar preserves repository identity and URL");

  const auto *folder = child(projection, physical, 0);
  require(folder && folder->title == "A" && folder->children.size() == 3,
          "FolderBar preserves direct songs alongside nested folders");
  const auto *second = child(projection, folder, 0);
  const auto *first = child(projection, folder, 1);
  const auto *nestedFolder = child(projection, folder, 2);
  require(second && first && second->title == "Second" &&
              first->title == "First" && nestedFolder &&
              nestedFolder->kind == skin::MusicSelectBarKind::Folder &&
              nestedFolder->title == "sub" &&
              nestedFolder->children.size() == 1 &&
              child(projection, nestedFolder, 0)->title == "Nested",
          "physical SongBars deduplicate first SHA occurrence and reverse order");
  require(folder && folder->presentation.folderLampCounts[0] == 1 &&
              folder->presentation.folderLampCounts[7] == 2 &&
              folder->presentation.folderRankCounts[0] == 1 &&
              folder->presentation.folderRankCounts[22] == 2 &&
              folder->presentation.lamp == 0,
          "DirectoryBar status counts source rows before SongBar deduplication");

  const auto *level = child(projection, imported, 0);
  const auto *grade = child(projection, imported, 1);
  require(level && level->kind == skin::MusicSelectBarKind::Hash &&
              level->title == "sl12" && level->tableLevel == "12" &&
              level->children.size() == 2 &&
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
      chart("/songs/b.bms", "second", "Second", "", "/songs")};
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
}

} // namespace

int main(int argc, char **argv) {
  testProjectsFoldersSongsScoresAndSourceFlags();
  testOverlappingConfiguredRootsFormOnePhysicalHierarchy();
  testProjectsExactRootHierarchyTablesCoursesAndCommands();
  testProjectsReplaySlotsForCompleteGrades();
  testEmptyImportedCourseIsUnavailable();
  testProjectionOwnsItsRepositoryValues();
  testProjectsSearchHistoryAfterCommands();
  testProjectsRecentScoreImprovementCommandChildren();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_repository_projection_tests", failures,
      "music-select repository projection assertion(s) failed",
      "music-select repository projection tests passed");
}
