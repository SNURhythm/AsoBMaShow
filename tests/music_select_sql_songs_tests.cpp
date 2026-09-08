#include "music_select/MusicSelectSqlSongs.h"
#include "music_select/MusicSelectRepositoryProjection.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<MusicSelectSqlSongs>);
static_assert(!std::is_copy_assignable_v<MusicSelectSqlSongs>);
static_assert(!std::is_move_constructible_v<MusicSelectSqlSongs>);
static_assert(!std::is_move_assignable_v<MusicSelectSqlSongs>);

namespace {

ChartMetaRecord recordAt(std::size_t number) {
  ChartMetaRecord record;
  record.meta.Title = "Song " + std::to_string(number);
  record.meta.BmsPath = "/songs/" + std::to_string(number) + ".bms";
  record.meta.SHA256 = std::to_string(number);
  record.meta.KeyMode = 7;
  return record;
}

struct FakeReader {
  std::size_t count = 100'000;
  std::size_t resolves = 0;
  std::size_t reads = 0;
  std::size_t projected = 0;
  std::size_t lookups = 0;
  bool failResolve = false;
  bool failPage = false;
  bool failLookup = false;
  std::string lookupFailure = "lookup failed";
  bool shortPage = false;
  bool longPage = false;
  bool invalidRecord = false;
  bool invalidProjection = false;
  bool failProjection = false;
  bool invalidIndex = false;
  std::string lastIdentity;
  std::vector<std::pair<std::size_t, std::size_t>> requests;

  MusicSelectSqlSongs::QueryResolver resolver() {
    return [this](const MusicSelectBarManagerConfig &config) {
      ++resolves;
      if (failResolve) throw std::runtime_error("count failed");
      const auto resolvedCount = config.modeFilter == "EMPTY" ? 0 : count;
      const auto reversed = config.sortId == "LEVEL";
      return MusicSelectSqlSongs::ResolvedQuery{
          .count = resolvedCount,
          .resolvedFilters = {"ALL", "ALL"},
          .loadPage = [this, resolvedCount, reversed](std::size_t offset,
                                                     std::size_t limit) {
            ++reads;
            requests.emplace_back(offset, limit);
            assert(limit <= 128 && offset < resolvedCount);
            if (failPage) throw std::runtime_error("page failed");
            const auto pageCount = std::min(limit, resolvedCount - offset);
            std::vector<ChartMetaRecord> records;
            for (std::size_t row = 0; row < pageCount; ++row) {
              const auto number = reversed ? resolvedCount - offset - row - 1
                                           : offset + row;
              records.push_back(recordAt(number));
            }
            if (shortPage) records.erase(records.begin());
            if (longPage) records.push_back(recordAt(resolvedCount));
            if (invalidRecord) records.front().meta.BmsPath.clear();
            return records;
          },
          .findIndex = [this, resolvedCount, reversed](std::string_view identity)
              -> std::optional<std::size_t> {
            ++lookups;
            lastIdentity = identity;
            if (failLookup) throw std::runtime_error(lookupFailure);
            if (invalidIndex) return resolvedCount;
            if (identity == "sha256:99999") return reversed ? 0 : 99'999;
            if (identity == "md5:legacy" || identity == "path:/songs/nohash.bms") {
              return 17;
            }
            return std::nullopt;
          }};
    };
  }

  MusicSelectSqlSongs::Projector projector() {
    return [this](const ChartMetaRecord &record) {
      ++projected;
      if (failProjection) throw std::runtime_error("projection failed");
      auto bar = MusicSelectRepositoryProjection::projectSong(
          record, "folder:/songs", {});
      if (invalidProjection) bar.id.value = "folder:/songs:sha256:changed";
      return bar;
    };
  }
};

void assertUnavailable(const MusicSelectBar &bar) {
  assert(bar.kind == skin::MusicSelectBarKind::Song);
  assert(!bar.chart && !bar.selectable && !bar.presentation.exists);
}

void testFirstAndWrappedPagesDoNotEnumerateTheLibrary() {
  FakeReader reader;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  assert(songs.size() == 100'000);
  assert(reader.resolves == 1 && reader.reads == 0 && reader.projected == 0);
  assert(songs.at(0).chart->meta.BmsPath == "/songs/0.bms");
  assert(songs.at(127).chart->meta.BmsPath == "/songs/127.bms");
  assert(songs.at(99'999).chart->meta.Title == "Song 99999");
  assert(reader.reads == 2 && reader.projected == 160);
  assert(reader.requests[0].first == 0 && reader.requests[1].first == 99'968);
  const auto retained = songs.at(0);
  for (std::size_t page = 1; page <= 6; ++page) (void)songs.at(page * 128);
  const auto before = reader.reads;
  assert(songs.at(0).id == retained.id && reader.reads == before + 1);
  assert(retained.chart->meta.Title == "Song 0");
  bool threw = false;
  try {
    (void)songs.at(100'000);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw);
}

void testFallbackAliasesKeepPrimedPagesAndReconfigureChangesOrdering() {
  FakeReader reader;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector(),
                           {.modeFilter = "14KEY", .difficultyFilter = "INSANE"});
  (void)songs.at(0);
  (void)songs.at(99'999);
  const auto resolved = songs.configure("14KEY", "INSANE", "TITLE");
  assert(resolved.first == "ALL" && resolved.second == "ALL");
  songs.configure("ALL", "ALL", "TITLE");
  assert(songs.at(0).chart->meta.Title == "Song 0");
  assert(songs.at(99'999).chart->meta.Title == "Song 99999");
  assert(reader.resolves == 1 && reader.reads == 2);
  songs.retryFailedPages();
  (void)songs.at(0);
  assert(reader.reads == 2);
  songs.configure("ALL", "ALL", "LEVEL");
  assert(songs.at(0).chart->meta.Title == "Song 99999");
  assert(reader.resolves == 2 && reader.reads == 3);
  songs.configure("EMPTY", "ALL", "TITLE");
  assert(songs.size() == 0);
  bool threw = false;
  try {
    (void)songs.at(0);
  } catch (const std::out_of_range &) {
    threw = true;
  }
  assert(threw && reader.reads == 3);
}

void testClonesOwnConfigurationCallbacksAndPages() {
  FakeReader reader;
  auto source = std::make_shared<MusicSelectSqlSongs>(
      "folder:/songs", reader.resolver(), reader.projector());
  (void)source->at(0);
  reader.count = 3;
  reader.failResolve = true;
  const auto clone = source->clone();
  assert(reader.resolves == 1 && reader.reads == 1 && clone->size() == 100'000);
  reader.count = 100'000;
  reader.failResolve = false;
  clone->configure("ALL", "ALL", "LEVEL");
  assert(reader.resolves == 2);
  assert(clone->at(0).chart->meta.Title == "Song 99999");
  assert(source->at(0).chart->meta.Title == "Song 0");
  assert(reader.reads == 2);
  assert(source->at(128).chart->meta.Title == "Song 128");
  assert(source->indexOf({"folder:/songs:sha256:99999"}) == 99'999);
  assert(clone->indexOf({"folder:/songs:sha256:99999"}) == 0);
  source.reset();
  assert(clone->at(128).chart->meta.Title == "Song 99871");
}

void testIdentityLookupUsesSqlWithoutLoadingPages() {
  FakeReader reader;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  assert(songs.indexOf({"folder:/songs:sha256:99999"}) == 99'999);
  assert(reader.lastIdentity == "sha256:99999");
  assert(songs.indexOf({"folder:/songs:md5:legacy"}) == 17);
  assert(reader.lastIdentity == "md5:legacy");
  assert(songs.indexOf({"folder:/songs:path:/songs/nohash.bms"}) == 17);
  assert(reader.lastIdentity == "path:/songs/nohash.bms");
  assert(!songs.indexOf({"folder:/other:sha256:99999"}));
  assert(!songs.indexOf({"folder:/songs-extra:sha256:99999"}));
  assert(!songs.indexOf({"folder:/songs:unavailable:0"}));
  assert(reader.lookups == 3);
  assert(!songs.indexOf({"folder:/songs:sha256:absent"}));
  reader.failLookup = true;
  assert(!songs.indexOf({"folder:/songs:sha256:99999"}));
  assert(songs.diagnostic() == "lookup failed");
  reader.lookupFailure.clear();
  assert(!songs.indexOf({"folder:/songs:sha256:99999"}));
  assert(!songs.diagnostic().empty());
  reader.failLookup = false;
  songs.retryFailedPages();
  assert(songs.indexOf({"folder:/songs:sha256:99999"}) == 99'999);
  reader.invalidIndex = true;
  assert(!songs.indexOf({"folder:/songs:sha256:99999"}));
  assert(!songs.diagnostic().empty());
  assert(reader.reads == 0 && reader.projected == 0);
}

void testPageErrorsKeepBoundedLogicalSlotsUntilRetry() {
  FakeReader reader;
  reader.failPage = true;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  for (std::size_t row = 0; row < 128; ++row) assertUnavailable(songs.at(row));
  assert(songs.at(0).id != songs.at(1).id);
  assert(reader.reads == 1 && songs.size() == 100'000);
  assert(songs.diagnostic() == "page failed");
  reader.failPage = false;
  assertUnavailable(songs.at(0));
  assert(reader.reads == 1);
  songs.retryFailedPages();
  assert(songs.at(0).chart && reader.reads == 2 && songs.diagnostic().empty());
}

void testMiddlePageFailureDoesNotRetryAfterCacheEviction() {
  FakeReader reader;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  const auto retained = songs.at(0);
  (void)songs.at(99'999);
  reader.failPage = true;
  assertUnavailable(songs.at(50'000));
  const auto failedReads = reader.reads;
  for (std::size_t frame = 0; frame < 100; ++frame) {
    for (std::size_t page = 1; page <= 10; ++page) {
      assertUnavailable(songs.at(page * 128));
    }
    assertUnavailable(songs.at(50'000));
  }
  assert(reader.reads == failedReads);
  assert(retained.chart->meta.Title == "Song 0");
  reader.failPage = false;
  songs.retryFailedPages();
  assert(songs.at(50'000).chart && reader.reads == failedReads + 1);
}

void testReplacingFailedProviderRetainsFrameAndMiddlePosition() {
  FakeReader reader;
  MusicSelectBar directory{.id = {"folder:/songs"},
      .kind = skin::MusicSelectBarKind::Folder, .childrenLoaded = false};
  MusicSelectBarManager bars({.bars = {directory}, .root = {directory.id}});
  auto provider = std::make_shared<MusicSelectSqlSongs>(
      directory.id.value, reader.resolver(), reader.projector());
  assert(bars.installRowProvider(directory.id, provider) && bars.open(directory.id));
  bars.setSelectedPosition(0.5f);
  reader.failPage = true;
  const auto retained = bars.songListFrame();
  const auto selectedIndex = bars.readView().selectedIndex;
  assertUnavailable(provider->at(selectedIndex));
  reader.failPage = false;
  const auto replacement = std::make_shared<MusicSelectSqlSongs>(
      directory.id.value, reader.resolver(), reader.projector());
  assert(bars.installRowProvider(directory.id, replacement));
  const auto refreshed = bars.readView();
  assert(refreshed.selectedIndex == selectedIndex && refreshed.rowAt(selectedIndex).chart);
  assert(retained.rowProvider == provider && !retained.at(selectedIndex).exists);
  assert(reader.resolves == 2 && reader.reads == 2);
  assert(bars.close());
  assert(!bars.readView().rowAt(0).childrenLoaded);
}

void testMalformedPagesDoNotShiftRowsOrPublishLaunchablePlaceholders() {
  for (const auto failure : {0, 1, 2, 3, 4}) {
    FakeReader reader;
    reader.count = 3;
    reader.shortPage = failure == 0;
    reader.longPage = failure == 1;
    reader.invalidRecord = failure == 2;
    reader.invalidProjection = failure == 3;
    reader.failProjection = failure == 4;
    MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
    assertUnavailable(songs.at(0));
    if (failure < 2) {
      assertUnavailable(songs.at(1));
      assertUnavailable(songs.at(2));
    }
    assert(songs.size() == 3 && !songs.diagnostic().empty() && reader.reads == 1);
    reader.shortPage = reader.longPage = reader.invalidRecord = false;
    reader.invalidProjection = reader.failProjection = false;
    songs.retryFailedPages();
    assert(songs.at(0).chart->meta.Title == "Song 0");
    assert(songs.at(2).chart->meta.Title == "Song 2");
    assert(reader.reads == 2 && songs.diagnostic().empty());
  }
}

void testCountFailuresPropagateAndFailedReconfigurationPreservesRows() {
  FakeReader reader;
  reader.failResolve = true;
  bool threw = false;
  try {
    MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  } catch (const std::runtime_error &error) {
    threw = std::string_view(error.what()) == "count failed";
  }
  assert(threw);
  reader.failResolve = false;
  MusicSelectSqlSongs songs("folder:/songs", reader.resolver(), reader.projector());
  (void)songs.at(0);
  reader.failResolve = true;
  threw = false;
  try {
    songs.configure("ALL", "ALL", "LEVEL");
  } catch (const std::runtime_error &) {
    threw = true;
  }
  assert(threw && songs.size() == 100'000);
  assert(songs.at(0).chart->meta.Title == "Song 0" && reader.reads == 1);
  assert(songs.at(128).chart->meta.Title == "Song 128");
  reader.failResolve = false;
  songs.configure("ALL", "ALL", "LEVEL");
  assert(songs.at(0).chart->meta.Title == "Song 99999");
}

}

int main() {
  testMiddlePageFailureDoesNotRetryAfterCacheEviction();
  testReplacingFailedProviderRetainsFrameAndMiddlePosition();
  testFirstAndWrappedPagesDoNotEnumerateTheLibrary();
  testFallbackAliasesKeepPrimedPagesAndReconfigureChangesOrdering();
  testClonesOwnConfigurationCallbacksAndPages();
  testIdentityLookupUsesSqlWithoutLoadingPages();
  testPageErrorsKeepBoundedLogicalSlotsUntilRetry();
  testMalformedPagesDoNotShiftRowsOrPublishLaunchablePlaceholders();
  testCountFailuresPropagateAndFailedReconfigurationPreservesRows();
  std::cout << "music-select SQL songs tests passed\n";
}
