#include "music_select/MusicSelectSongIndex.h"
#include "music_select/MusicSelectBarManager.h"
#include "music_select/MusicSelectRepositoryProjection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
std::atomic<std::size_t> allocations = 0;
}

void *operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *memory = std::malloc(size == 0 ? 1 : size)) return memory;
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    if (failures < 20) std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Exception, typename Callback>
void requireThrows(Callback callback, std::string_view message) {
  try {
    callback();
    require(false, message);
  } catch (const Exception &) {
  }
}

ChartMetaRecord chart(std::string hash, std::string name) {
  ChartMetaRecord record;
  record.meta.BmsPath = "/songs/" + name + ".bms";
  record.meta.Folder = "/songs";
  record.meta.SHA256 = std::move(hash);
  record.meta.Title = std::move(name);
  record.meta.KeyMode = 7;
  record.meta.TotalNotes = 500;
  return record;
}

void testRepresentativeAndIdentitySemantics() {
  auto first = chart("same", "Alpha");
  auto duplicate = chart("same", "Zulu");
  auto second = chart("other", "Beta");
  auto empty = chart("", "Gamma");
  empty.meta.MD5 = "md5-first";
  auto emptyDuplicate = chart("", "Omega");
  emptyDuplicate.meta.MD5 = "md5-later";
  MusicSelectSongIndex index("folder:/songs");
  for (const auto &record : {first, second, empty, emptyDuplicate, duplicate}) {
    index.add(record, std::nullopt, kNoClearTypeRank);
  }
  index.finish();
  require(index.size() == 3, "first SHA256 representative, including empty SHA256");
  if (index.size() != 3) return;
  require(index.pathAt(0) == empty.meta.BmsPath &&
              index.pathAt(1) == second.meta.BmsPath &&
              index.pathAt(2) == first.meta.BmsPath,
          "reverse unique representatives rather than reverse before dedup");
  require(index.idAt(0).value == "folder:/songs:md5:md5-first" &&
              index.idAt(2).value == "folder:/songs:sha256:same",
          "identity uses directory context and representative hash fallback");
  require(index.titleAt(2) == "Alpha", "placeholder title retains original case");
  index.finish();
  require(index.pathAt(0) == empty.meta.BmsPath, "finish is idempotent");
  requireThrows<std::logic_error>(
      [&] { index.add(first, std::nullopt, kNoClearTypeRank); },
      "finished identity set cannot silently accept more records");
  requireThrows<std::out_of_range>([&] { (void)index.pathAt(3); },
                                  "path access checks logical bounds");
  requireThrows<std::out_of_range>([&] { (void)index.idAt(3); },
                                  "identity access checks logical bounds");
  requireThrows<std::out_of_range>([&] { (void)index.titleAt(3); },
                                  "title access checks logical bounds");
  require(!index.indexOf({"missing"}), "unknown identity is absent");

  MusicSelectSongIndex pathIndex("folder:/songs");
  empty.meta.MD5.clear();
  empty.meta.BmsPath = "/songs/sub/../Gamma.bms";
  pathIndex.add(empty, std::nullopt, kNoClearTypeRank);
  pathIndex.finish();
  require(pathIndex.size() == 1 &&
              pathIndex.idAt(0).value == "folder:/songs:path:/songs/Gamma.bms" &&
              pathIndex.pathAt(0) == empty.meta.BmsPath,
          "path identity is normalized without rewriting fetch path");
}

void testDedupBeforeHidingAndFallback() {
  auto hidden = chart("duplicate", "Alpha");
  hidden.songReviewFavorite = 4;
  auto visibleDuplicate = chart("duplicate", "Beta");
  auto visible = chart("visible", "Gamma");
  visible.meta.KeyMode = 14;
  MusicSelectSongIndex index("folder:/songs");
  for (const auto &record : {hidden, visibleDuplicate, visible}) {
    index.add(record, std::nullopt, kNoClearTypeRank);
  }
  index.finish();
  require(index.configure("7KEY", "NORMAL", "TITLE") ==
              std::pair<std::string, std::string>{"14KEY", "NORMAL"},
          "hidden first representative is not replaced by a visible duplicate");
  require(index.size() == 1 && index.pathAt(0) == visible.meta.BmsPath,
          "fallback cycles modes before trying the next difficulty");
  require(!index.indexOf({"folder:/songs:sha256:duplicate"}),
          "hidden identity has no visible index");

  MusicSelectSongIndex hiddenIndex("folder:/songs");
  hiddenIndex.add(hidden, std::nullopt, kNoClearTypeRank);
  hiddenIndex.finish();
  require(hiddenIndex.configure("9KEY", "INSANE", "TITLE") ==
              std::pair<std::string, std::string>{"9KEY", "INSANE"} &&
              hiddenIndex.size() == 1,
          "all-hidden exhaustion restores original rows and requested filters");

  MusicSelectSongIndex emptyIndex("folder:/songs");
  emptyIndex.finish();
  require(emptyIndex.configure("invalid", "invalid", "TITLE") ==
              std::pair<std::string, std::string>{"invalid", "invalid"} &&
              emptyIndex.size() == 0,
          "empty input does not resolve filters");
}

struct Fixture {
  std::vector<ChartMetaRecord> records;
  std::vector<std::optional<ScoreBestSnapshot>> scores;
  std::vector<int> clearRanks;
};

MusicSelectProjection eagerProjection(const Fixture &fixture) {
  MusicSelectRepositoryMetadata metadata;
  metadata.entries.push_back({.path = "/songs"});
  return MusicSelectRepositoryProjection{}.project(
      {.records = fixture.records,
       .scoreFor = [&](const bms_parser::ChartMeta &meta, int) {
         const auto found = std::ranges::find(fixture.records, meta.BmsPath,
             [](const auto &record) { return record.meta.BmsPath; });
         return fixture.scores.at(found - fixture.records.begin());
       },
       .clearFor = [&](const bms_parser::ChartMeta &meta, int) {
         const auto found = std::ranges::find(fixture.records, meta.BmsPath,
             [](const auto &record) { return record.meta.BmsPath; });
         return fixture.clearRanks.at(found - fixture.records.begin());
       },
       .metadata = &metadata});
}

void compareWithEager(MusicSelectSongIndex &index,
                      const MusicSelectProjection &projection,
                      std::string_view mode, std::string_view difficulty,
                      std::string_view sort) {
  MusicSelectBarManager eager(projection,
      {.modeFilter = std::string(mode),
       .difficultyFilter = std::string(difficulty),
       .sortId = std::string(sort)});
  require(eager.open({"folder:/songs"}), "eager fixture opens song directory");
  const auto snapshot = eager.readView();
  const auto resolved = index.configure(mode, difficulty, sort);
  require(resolved.first == snapshot.resolvedModeFilter &&
              resolved.second == snapshot.resolvedDifficultyFilter,
          "compact filter resolution matches eager filter resolution");
  if (index.size() != snapshot.rows.size()) {
    require(false, "compact visible count matches eager count");
    return;
  }
  for (std::size_t position = 0; position < index.size(); ++position) {
    const auto &bar = snapshot.rows[position];
    require(index.idAt(position) == bar.id && bar.chart &&
                index.pathAt(position) == bar.chart->meta.BmsPath,
            "compact ordered identities and representative paths match eager");
    require(index.indexOf(bar.id) == position,
            "identity lookup returns position in the current filtered order");
  }
}

void testPinnedAvailabilityOrdering() {
  for (const std::string_view sort : {"ARTIST", "BPM", "LENGTH", "LEVEL"}) {
    for (bool reverse : {false, true}) {
      for (bool emptyPath : {false, true}) {
        auto installed = chart("installed", "Zulu");
        installed.meta.Artist = "Zulu";
        installed.meta.MaxBpm = 200;
        installed.meta.PlayLength = 200;
        installed.meta.PlayLevel = 12;
        auto missing = chart("missing", "Alpha");
        missing.meta.Artist = "Alpha";
        missing.meta.MaxBpm = 0;
        missing.meta.PlayLength = 0;
        missing.meta.PlayLevel = 0;
        missing.unavailable = !emptyPath;
        if (emptyPath) missing.meta.BmsPath.clear();
        MusicSelectSongIndex index("folder:/songs");
        const auto add = [&](const ChartMetaRecord &record) {
          const bool isInstalled = record.meta.SHA256 == "installed";
          index.add(record, ScoreBestSnapshot{.score = isInstalled ? 100 : 10,
                                              .maxScore = 100},
                    isInstalled ? kClearTypeHardClearRank : kNoClearTypeRank);
        };
        if (reverse) { add(missing); add(installed); }
        else { add(installed); add(missing); }
        index.finish();
        index.configure("ALL", "ALL", sort);
        require(index.idAt(0).value == "folder:/songs:sha256:installed" &&
                    index.idAt(1).value == "folder:/songs:sha256:missing",
                "pinned metadata sorts put installed before unavailable or pathless songs in either input order");
        require(index.indexOf({"folder:/songs:sha256:installed"}) == 0 &&
                    index.indexOf({"folder:/songs:sha256:missing"}) == 1,
                "availability reordering updates compact identity positions");
        for (const std::string_view otherSort : {"TITLE", "SCORE", "CLEAR"}) {
          index.configure("ALL", "ALL", otherSort);
          require(index.idAt(0).value == "folder:/songs:sha256:missing",
                  "TITLE and score sorts remain independent of availability");
        }
        installed.unavailable = true;
        MusicSelectSongIndex unavailable("folder:/songs");
        if (reverse) {
          unavailable.add(missing, std::nullopt, kNoClearTypeRank);
          unavailable.add(installed, std::nullopt, kNoClearTypeRank);
        } else {
          unavailable.add(installed, std::nullopt, kNoClearTypeRank);
          unavailable.add(missing, std::nullopt, kNoClearTypeRank);
        }
        unavailable.finish();
        unavailable.configure("ALL", "ALL", "TITLE");
        unavailable.configure("ALL", "ALL", sort);
        require(unavailable.idAt(0).value == (reverse ? "folder:/songs:sha256:installed"
                                                     : "folder:/songs:sha256:missing") &&
                    unavailable.idAt(1).value == (reverse ? "folder:/songs:sha256:missing"
                                                         : "folder:/songs:sha256:installed"),
                "two unavailable songs compare equal and restore the authored reverse-unique order");
      }
    }
  }
}

void testEveryFilterAndSortAgainstEager() {
  Fixture fixture;
  constexpr std::array modes{0, 5, 7, 9, 10, 14, 24, 48};
  constexpr std::array noteCounts{0, 249, 250, 500, 599, 600,
                                  700, 999, 1000, 1300, 1999, 2000, 2700};
  for (int number = 0; number < 112; ++number) {
    auto record = chart(std::to_string(number), "song" + std::to_string(number));
    record.meta.Title = number % 4 == 0 ? "Alpha" :
                        number % 3 == 0 ? "Equal" : "equal";
    record.meta.SubTitle = std::to_string(200 - number);
    record.meta.Artist = number % 5 == 0 ? "Another" :
                         number % 2 ? "Artist" : "ARTIST";
    record.meta.KeyMode = modes[number % modes.size()];
    record.meta.IsDP = number % 3 == 0;
    record.meta.TotalNotes = noteCounts[number % noteCounts.size()];
    record.meta.Difficulty = number % 6;
    record.meta.PlayLevel = (number % 7) * 0.5;
    record.meta.MaxBpm = 100 + number % 5;
    record.meta.MinBpm = number % 4 == 0 ? 50 : record.meta.MaxBpm;
    record.meta.PlayLength = static_cast<std::int64_t>(number % 11) * 1'000'000;
    record.meta.TotalScratchNotes = number % 2 ? record.meta.TotalNotes / 8 : 0;
    record.meta.TotalLongNotes = number % 3 ? record.meta.TotalNotes / 20 : 0;
    record.meta.TotalBackSpinNotes = number % 4;
    record.songReviewFavorite = number % 9 == 0 ? 4 : number % 11 == 0 ? 8 : 0;
    record.hasBpmStop = number % 7 == 0;
    record.hasScrollChange = number % 5 == 0;
    record.unavailable = number % 17 == 0;
    if (number == 4 || number == 5) record.meta.SHA256.clear();
    if (number == 5) record.meta.MD5 = "ignored-empty-hash-duplicate";
    if (number == 10) record.meta.SHA256 = "9";
    if (number == 12) record.meta.SHA256 = "Case";
    if (number == 13) record.meta.SHA256 = "case";
    fixture.records.push_back(std::move(record));
    std::optional<ScoreBestSnapshot> score;
    if (number % 7 != 0) {
      score = ScoreBestSnapshot{};
      score->score = (number % 11) * 20;
      score->maxScore = number % 9 == 0 ? 1 : 101 + (number % 5) * 20;
      if (number % 2 == 0) score->badPoints = number % 13;
      if (number % 3 != 0) score->averageJudgeMicros = (number % 5) * 100;
      if (number % 4 != 0) score->lastPlayedUnixSeconds = 1'700'000'000 + number % 8;
      score->createdAt = std::to_string(200 - number);
      score->clearType = kClearTypeFailedRank;
    }
    fixture.scores.push_back(std::move(score));
    constexpr std::array ranks{kNoClearTypeRank, kClearTypeFailedRank,
        kClearTypeAssistedEasyClearRank, kClearTypeLightAssistedEasyClearRank,
        kClearTypeEasyClearRank, kClearTypeNormalClearRank,
        kClearTypeHardClearRank, kClearTypeExHardClearRank, kClearTypeFullComboRank};
    fixture.clearRanks.push_back(ranks[number % ranks.size()]);
  }
  MusicSelectSongIndex index("folder:/songs");
  for (std::size_t position = 0; position < fixture.records.size(); ++position) {
    index.add(fixture.records[position], fixture.scores[position],
              fixture.clearRanks[position]);
  }
  index.finish();
  const auto projection = eagerProjection(fixture);
  constexpr std::array<std::string_view, 11> modesToTry{
      "ALL", "7KEY", "14KEY", "9KEY", "5KEY", "10KEY", "24KEY",
      "48KEY", "SINGLE", "DOUBLE", "invalid"};
  constexpr std::array<std::string_view, 10> difficulties{
      "ALL", "BEGINNER", "NORMAL", "HYPER", "ANOTHER", "INSANE",
      "SCRATCH CHART", "LONG NOTE CHART", "SPEED CHANGE CHART", "invalid"};
  constexpr std::array<std::string_view, 13> sorts{
      "TITLE", "ARTIST", "BPM", "LENGTH", "LEVEL", "CLEAR", "SCORE",
      "MISSCOUNT", "DURATION", "LASTUPDATE", "RIVALCOMPARE_CLEAR",
      "RIVALCOMPARE_SCORE", "invalid"};
  for (const auto mode : modesToTry) {
    for (const auto difficulty : difficulties) {
      for (const auto sort : sorts) {
        compareWithEager(index, projection, mode, difficulty, sort);
      }
    }
  }
}

void testDurationNarrowingAndStableReconfiguration() {
  Fixture fixture;
  fixture.records = {chart("first", "same"), chart("second", "Same")};
  fixture.scores = {ScoreBestSnapshot{.averageJudgeMicros = 0},
                    ScoreBestSnapshot{.averageJudgeMicros = 4'294'967'297}};
  fixture.clearRanks = {kNoClearTypeRank, kNoClearTypeRank};
  MusicSelectSongIndex index("folder:/songs");
  for (std::size_t position = 0; position < fixture.records.size(); ++position) {
    index.add(fixture.records[position], fixture.scores[position],
              fixture.clearRanks[position]);
  }
  index.finish();
  compareWithEager(index, eagerProjection(fixture), "ALL", "ALL", "DURATION");
  require(index.size() == 2 && index.idAt(0).value.ends_with("sha256:first"),
          "duration compares signed low 32 bits of the difference");
  index.configure("ALL", "ALL", "TITLE");
  require(index.size() == 2 && index.idAt(0).value.ends_with("sha256:second"),
          "new sorts start from reversed raw order rather than prior sort");

  fixture.scores[1]->averageJudgeMicros = 4'294'967'295;
  MusicSelectSongIndex wrapped("folder:/songs");
  for (std::size_t position = 0; position < fixture.records.size(); ++position) {
    wrapped.add(fixture.records[position], fixture.scores[position],
                fixture.clearRanks[position]);
  }
  wrapped.finish();
  compareWithEager(wrapped, eagerProjection(fixture), "ALL", "ALL", "DURATION");
  require(wrapped.size() == 2 &&
              wrapped.idAt(0).value.ends_with("sha256:second"),
          "wrapped duration ordering differs from ordinary int64 ordering");
}

void testCancellationAndRetry() {
  std::stop_source cancellation;
  cancellation.request_stop();
  MusicSelectSongIndex index("folder:/songs");
  index.add(chart("first", "Alpha"), std::nullopt, kNoClearTypeRank);
  index.add(chart("second", "Beta"), std::nullopt, kNoClearTypeRank);
  requireThrows<std::runtime_error>([&] { index.finish(cancellation.get_token()); },
                                    "cancelled finish aborts before publishing");
  index.finish();
  require(index.size() == 2, "finish can retry after cancellation");
  if (index.size() != 2) return;
  const auto before = index.idAt(0);
  requireThrows<std::runtime_error>([&] {
    index.configure("ALL", "ALL", "TITLE", cancellation.get_token());
  }, "cancelled configure aborts before publishing");
  require(index.idAt(0) == before && index.indexOf(before) == 0,
          "cancelled configure preserves published order and lookup");
  index.configure("ALL", "ALL", "TITLE");
  require(index.idAt(0).value.ends_with("sha256:first"),
          "configuration can retry after cancellation");
  auto copied = index;
  copied.configure("ALL", "ALL", "RIVALCOMPARE_SCORE");
  require(copied.indexOf(before) == 0 && index.indexOf(before) == 1,
          "copied compact indexes own independent order and identity lookups");
}

void testRepeatedConfigurationDoesNotRebuildTheIndex() {
  MusicSelectSongIndex index("folder:/songs");
  for (int number = 0; number < 512; ++number) {
    index.add(chart(std::to_string(number), std::to_string(number)),
              std::nullopt, kNoClearTypeRank);
  }
  index.finish();
  index.configure("9KEY", "NORMAL", "TITLE");
  auto copied = index;
  const auto firstId = index.idAt(0);
  const auto before = allocations.load(std::memory_order_relaxed);
  const auto resolved = index.configure("9KEY", "NORMAL", "TITLE");
  const auto copiedResolved = copied.configure("9KEY", "NORMAL", "TITLE");
  const auto after = allocations.load(std::memory_order_relaxed);
  require(after == before,
          "identical requested configuration does not allocate or rebuild rows");
  require(resolved == std::pair<std::string, std::string>{"SINGLE", "NORMAL"} &&
              copiedResolved == resolved && index.idAt(0) == firstId,
          "cached result preserves fallback resolution, including after copy");
  std::stop_source cancellation;
  cancellation.request_stop();
  requireThrows<std::runtime_error>([&] {
    index.configure("9KEY", "NORMAL", "TITLE", cancellation.get_token());
  }, "cache hit still honors requested cancellation");
  index.finish();
  const auto beforeFinishReconfigure = allocations.load(std::memory_order_relaxed);
  index.configure("9KEY", "NORMAL", "TITLE");
  require(allocations.load(std::memory_order_relaxed) > beforeFinishReconfigure,
          "finish invalidates the requested-configuration cache");
}

std::uint64_t peakMemoryBytes() {
#if defined(__APPLE__) || defined(__linux__)
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return 0;
#endif
}

void testHundredThousandCompactEntries() {
  const auto before = peakMemoryBytes();
  MusicSelectSongIndex index("folder:/songs");
  auto record = chart("", "same");
  record.meta.SubTitle = std::string(16'384, 'x');
  record.meta.Genre = std::string(16'384, 'y');
  record.difficultyTableLabels = std::string(16'384, 'z');
  std::optional<ScoreBestSnapshot> score = ScoreBestSnapshot{};
  score->createdAt = std::string(16'384, 't');
  for (int number = 0; number < 100'000; ++number) {
    record.meta.SHA256 = std::to_string(number);
    record.meta.BmsPath = "/songs/" + std::to_string(number) + ".bms";
    index.add(record, score, kNoClearTypeRank);
  }
  index.finish();
  index.configure("ALL", "ALL", "TITLE");
  require(index.size() == 100'000, "100k narrow records retain global count");
  if (index.size() == 100'000) {
    require(index.pathAt(0) == "/songs/99999.bms" &&
                index.pathAt(99'999) == "/songs/0.bms" &&
                index.indexOf({"folder:/songs:sha256:50000"}) == 49'999,
            "100k equal-key order and random identity lookup remain exact");
  }
  const auto after = peakMemoryBytes();
  require(after <= before + 160ULL * 1024 * 1024,
          "100k index does not retain rich chart or score payloads");
  std::cout << "100k compact index peak RSS growth: " << (after - before) << '\n';
}

}

int main() {
  testPinnedAvailabilityOrdering();
  testRepresentativeAndIdentitySemantics();
  testDedupBeforeHidingAndFallback();
  testEveryFilterAndSortAgainstEager();
  testDurationNarrowingAndStableReconfiguration();
  testCancellationAndRetry();
  testRepeatedConfigurationDoesNotRebuildTheIndex();
  testHundredThousandCompactEntries();
  if (failures != 0) std::cerr << failures << " assertions failed\n";
  return failures == 0 ? 0 : 1;
}
