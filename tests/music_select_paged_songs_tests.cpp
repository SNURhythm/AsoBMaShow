#include "music_select/MusicSelectPagedSongs.h"
#include "music_select/MusicSelectRepositoryProjection.h"
#include "music_select/MusicSelectReplaySlots.h"
#include "path.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<MusicSelectPagedSongs>);
static_assert(!std::is_move_constructible_v<MusicSelectPagedSongs>);

namespace {

ChartMetaRecord recordAt(std::size_t number) {
  char title[32];
  std::snprintf(title, sizeof(title), "Song %06zu", number);
  ChartMetaRecord record;
  record.meta.Title = title;
  record.meta.BmsPath = "/songs/" + std::to_string(number) + ".bms";
  record.meta.SHA256 = std::to_string(number);
  record.meta.KeyMode = 7;
  return record;
}

MusicSelectSongIndex indexWith(std::size_t count) {
  MusicSelectSongIndex index("folder:/songs");
  for (std::size_t number = 0; number < count; ++number) {
    index.add(recordAt(number), std::nullopt, -1);
  }
  index.finish();
  index.configure("ALL", "ALL", "TITLE");
  return index;
}

ChartMetaRecord recordFor(const std::filesystem::path &path) {
  return recordAt(std::stoull(path.stem().string()));
}

void testRichReadsAndReplayWorkArePaged() {
  std::size_t readCalls = 0;
  std::size_t projected = 0;
  MusicSelectPagedSongs songs(
      indexWith(100'000),
      [&](std::span<const std::filesystem::path> paths) {
        ++readCalls;
        assert(paths.size() <= 128);
        ChartMetaPathBatchReadOutcome result{
            .status = ChartMetaPathBatchReadStatus::Loaded};
        for (const auto &path : paths) result.records.push_back(recordFor(path));
        std::ranges::reverse(result.records);
        return result;
      },
      [&](const ChartMetaRecord &record) {
        ++projected;
        return MusicSelectRepositoryProjection::projectSong(
            record, "folder:/songs", {});
      });
  assert(songs.size() == 100'000 && readCalls == 0 && projected == 0);
  assert(songs.at(0).chart->meta.BmsPath == "/songs/0.bms");
  assert(songs.at(127).chart->meta.BmsPath == "/songs/127.bms");
  assert(readCalls == 1 && projected == 128);
  assert(songs.at(128).chart->meta.BmsPath == "/songs/128.bms");
  assert(readCalls == 2 && projected == 256);
  const auto retained = songs.at(0);
  for (std::size_t page = 1; page <= 6; ++page) (void)songs.at(page * 128);
  const auto before = readCalls;
  assert(songs.at(0).id == retained.id && readCalls == before + 1);
  assert(retained.chart->meta.Title == "Song 000000");
  assert(songs.at(99'999).chart->meta.Title == "Song 099999");
  assert(songs.indexOf({"folder:/songs:sha256:99999"}) == 99'999);
  assert(!songs.indexOf({"absent"}));
}

void testMissingRecordsKeepTheirLogicalSlots() {
  MusicSelectPagedSongs songs(
      indexWith(3),
      [](std::span<const std::filesystem::path> paths) {
        ChartMetaPathBatchReadOutcome result{
            .status = ChartMetaPathBatchReadStatus::Loaded, .missingPaths = 1};
        result.records = {recordFor(paths.back()), recordFor(paths.front())};
        return result;
      },
      [](const ChartMetaRecord &record) {
        return MusicSelectRepositoryProjection::projectSong(
            record, "folder:/songs", {});
      });
  assert(songs.at(0).chart->meta.Title == "Song 000000");
  assert(songs.at(1).id.value == "folder:/songs:sha256:1");
  assert(!songs.at(1).chart && !songs.at(1).selectable &&
         !songs.at(1).presentation.exists);
  assert(songs.at(2).chart->meta.Title == "Song 000002");
  assert(songs.size() == 3);
}

void testStorageFailuresAreBoundedAndRetryable() {
  int reads = 0;
  MusicSelectPagedSongs songs(
      indexWith(5),
      [&](std::span<const std::filesystem::path> paths) {
        ++reads;
        ChartMetaPathBatchReadOutcome result;
        if (reads == 1) {
          result.diagnostic = "temporary failure";
          return result;
        }
        result.status = ChartMetaPathBatchReadStatus::Loaded;
        for (const auto &path : paths) result.records.push_back(recordFor(path));
        return result;
      },
      [](const ChartMetaRecord &record) {
        return MusicSelectRepositoryProjection::projectSong(
            record, "folder:/songs", {});
      });
  for (std::size_t row = 0; row < 5; ++row) {
    assert(!songs.at(row).selectable);
  }
  assert(reads == 1 && songs.diagnostic() == "temporary failure");
  songs.retryFailedPages();
  assert(songs.at(3).chart && reads == 2 && songs.diagnostic().empty());
}

void testFallbackConfigurationRetainsWorkerWarmedPages() {
  int reads = 0;
  MusicSelectPagedSongs songs(
      indexWith(300),
      [&](std::span<const std::filesystem::path> paths) {
        ++reads;
        ChartMetaPathBatchReadOutcome result{
            .status = ChartMetaPathBatchReadStatus::Loaded};
        for (const auto &path : paths) result.records.push_back(recordFor(path));
        return result;
      },
      [](const ChartMetaRecord &record) {
        return MusicSelectRepositoryProjection::projectSong(
            record, "folder:/songs", {});
      }, {.modeFilter = "14KEY"});
  (void)songs.at(0);
  (void)songs.at(299);
  assert(reads == 2);
  const auto resolved = songs.configure("14KEY", "ALL", "TITLE");
  (void)songs.at(0);
  (void)songs.at(299);
  assert(reads == 2 && "opening must not invalidate worker-warmed fallback pages");
  songs.configure(resolved.first, resolved.second, "TITLE");
  (void)songs.at(0);
  assert(reads == 2 && "resolved settings describe the same warmed ordering");
}

void benchmark(std::string_view mode, std::size_t count) {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  auto stage = start;
  const auto report = [&](std::string_view name) {
    const auto now = Clock::now();
    std::cout << name << "_ms="
              << std::chrono::duration<double, std::milli>(now - stage).count()
              << '\n';
    stage = now;
  };
  const auto makeRecord = [](std::size_t number) {
    auto record = recordAt(number);
    char hash[65];
    std::snprintf(hash, sizeof(hash), "%064zx", number);
    record.meta.SHA256 = hash;
    record.meta.Folder = "/songs";
    record.meta.Artist = "Synthetic benchmark artist";
    record.meta.StageFile = "/songs/artwork/stage.png";
    record.meta.Banner = "/songs/artwork/banner.png";
    record.meta.Preview = "/songs/preview.ogg";
    record.meta.TotalNotes = 1000;
    return record;
  };
  std::size_t replayChecks = 0;
  std::size_t richRows = 0;
  MusicSelectRepositoryProjectionInput input{
      .replayExistsFor = [&](const ChartMetaRecord &record, int longNoteMode) {
        replayChecks += 4;
        return musicSelectExistingChartReplaySlots(
            record, longNoteMode, "/tmp/asobmashow-selector-missing-profile");
      },
      .selectedLongNoteMode = 1};
  if (mode == "eager") {
    std::vector<ChartMetaRecord> records;
    records.reserve(count);
    for (std::size_t number = 0; number < count; ++number) {
      records.push_back(makeRecord(number));
    }
    richRows = count;
    report("records");
    MusicSelectRepositoryMetadata metadata;
    metadata.entries.push_back({.path = utf8_to_path_t("/songs")});
    input.records = records;
    input.metadata = &metadata;
    const auto projection = MusicSelectRepositoryProjection{}.project(input);
    auto children = musicSelectProjectionChildren(projection, {"folder:/songs"});
    report("projection");
    MusicSelectBarManager bars(MusicSelectRepositoryProjection{}.projectRoot(
        metadata, {}, 0));
    assert(bars.installChildren({"folder:/songs"}, std::move(children)));
    assert(bars.open({"folder:/songs"}));
    assert(bars.readView().rowCount() == count);
    report("install_sort");
  } else {
    MusicSelectSongIndex index("folder:/songs");
    for (std::size_t number = 0; number < count; ++number) {
      auto record = makeRecord(number);
      record.meta.StageFile.clear();
      record.meta.Banner.clear();
      record.meta.Preview.clear();
      index.add(record, std::nullopt, -1);
    }
    index.finish();
    index.configure("ALL", "ALL", "TITLE");
    report("index");
    auto songs = std::make_shared<MusicSelectPagedSongs>(
        std::move(index),
        [&](std::span<const std::filesystem::path> paths) {
          ChartMetaPathBatchReadOutcome result{
              .status = ChartMetaPathBatchReadStatus::Loaded};
          richRows += paths.size();
          for (const auto &path : paths) {
            result.records.push_back(makeRecord(std::stoull(path.stem().string())));
          }
          return result;
        },
        [&](const ChartMetaRecord &record) {
          return MusicSelectRepositoryProjection::projectSong(
              record, "folder:/songs", input);
        });
    if (count != 0) {
      (void)songs->at(0);
      (void)songs->at(count - 1);
    }
    report("first_pages");
    MusicSelectRepositoryMetadata metadata;
    metadata.entries.push_back({.path = utf8_to_path_t("/songs")});
    MusicSelectBarManager bars(MusicSelectRepositoryProjection{}.projectRoot(
        metadata, {}, 0));
    assert(bars.installRowProvider({"folder:/songs"}, songs));
    assert(bars.open({"folder:/songs"}));
    assert(bars.readView().rowCount() == count);
    report("install_sort");
  }
  std::cout << "mode=" << mode << " count=" << count
            << " rich_rows=" << richRows << " replay_checks=" << replayChecks
            << " total_ms="
            << std::chrono::duration<double, std::milli>(Clock::now() - start).count()
            << '\n';
}

}

int main(int argc, char **argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--benchmark") {
    benchmark(argv[2], std::stoull(argv[3]));
    return 0;
  }
  testRichReadsAndReplayWorkArePaged();
  testMissingRecordsKeepTheirLogicalSlots();
  testStorageFailuresAreBoundedAndRetryable();
  testFallbackConfigurationRetainsWorkerWarmedPages();
  std::cout << "music-select paged songs tests passed\n";
}
