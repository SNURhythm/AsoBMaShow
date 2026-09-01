#include "music_select/MusicSelectRepositoryProjection.h"

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
  result.meta.TotalNotes = 400;
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
              song->presentation.difficulty == 4,
          "SongBar values use SongData full-title and chart fields");
  require(song && song->presentation.lamp == 7 &&
              song->score && song->score->score == 1000 &&
              song->replayExists ==
                  std::array<bool, 4>{true, false, true, false} &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureChargeNote) != 0 &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureMine) != 0,
          "score lamps and source feature bits use Beatoraja IDs");
  require(folder && folder->presentation.folderLampCounts[7] == 1,
          "DirectoryBar aggregates its child clear lamps");
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
                .constraintJson = R"(["grade_mirror","no_speed"])"},
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
       .courseRankFor = [](std::string_view key, int id, int mode) {
         return key == "course-key" && id == 73 && mode == 2
                    ? kClearTypeHardClearRank
                    : kNoClearTypeRank;
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
  require(folder && folder->title == "A" && folder->children.size() == 2,
          "FolderBar returns direct songs instead of nested folders");
  const auto *second = child(projection, folder, 0);
  const auto *first = child(projection, folder, 1);
  require(second && first && second->title == "Second" &&
              first->title == "First",
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
              grade->courseStages.size() == 2 &&
              grade->courseTotalNotes == 1000 &&
              !grade->presentation.exists && grade->presentation.lamp == 6 &&
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

} // namespace

int main() {
  testProjectsFoldersSongsScoresAndSourceFlags();
  testProjectsExactRootHierarchyTablesCoursesAndCommands();
  testProjectionOwnsItsRepositoryValues();
  testProjectsSearchHistoryAfterCommands();
  if (failures != 0) return 1;
  std::cout << "music-select repository projection tests passed\n";
  return 0;
}
