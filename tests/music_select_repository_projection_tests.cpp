#include "music_select/MusicSelectRepositoryProjection.h"

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
  return result;
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
       .selectedLongNoteMode = 2,
       .repositoryRevision = 9});
  require(projection.repositoryRevision == 9 && projection.root.size() == 2,
          "physical folders retain first-seen root order");
  const auto *folder = projection.find(projection.root.front());
  require(folder && folder->kind == skin::MusicSelectBarKind::Folder &&
              folder->children.size() == 1 && folder->selectable,
          "each physical folder projects a selectable directory bar");
  const auto *song = folder ? projection.find(folder->children.front()) : nullptr;
  require(song && song->kind == skin::MusicSelectBarKind::Song &&
              song->title == "Alpha Another" && song->chart &&
              song->presentation.exists && song->presentation.level == 12 &&
              song->presentation.difficulty == 4,
          "SongBar values use SongData full-title and chart fields");
  require(song && song->presentation.lamp == 7 &&
              song->score && song->score->score == 1000 &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureChargeNote) != 0 &&
              (song->presentation.featureFlags &
               skin::MusicSelectFeatureMine) != 0,
          "score lamps and source feature bits use Beatoraja IDs");
  require(folder && folder->presentation.folderLampCounts[7] == 1,
          "DirectoryBar aggregates its child clear lamps");
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

} // namespace

int main() {
  testProjectsFoldersSongsScoresAndSourceFlags();
  testProjectionOwnsItsRepositoryValues();
  if (failures != 0) return 1;
  std::cout << "music-select repository projection tests passed\n";
  return 0;
}
