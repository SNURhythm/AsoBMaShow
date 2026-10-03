#include "music_select/MusicSelectRanking.h"

#include "music_select_runtime_ledger_assertions.h"

#include <iostream>
#include <memory>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testProjectsServiceStateIntoBeatorajaRankingData() {
  ir::IrRankingSnapshot service;
  service.state = ir::IrRankingSnapshotState::Loading;
  auto projected = projectMusicSelectRanking(service, 0);
  expect(projected.state == MusicSelectRankingState::Access,
         "loading service state is RankingData.ACCESS");

  auto ranking = std::make_shared<ir::IrChartRanking>();
  ranking->totalPlayers = 3;
  ranking->entries = {
      {.rank = 99,
       .playerName = "Top",
       .score = 1900,
       .clearType = kClearTypeFullComboRank},
      {.rank = 99,
       .playerName = "Player",
       .score = 1700,
       .clearType = kClearTypeNormalClearRank,
       .currentUser = true},
      {.rank = 99,
       .playerName = "Failed",
       .score = 1900,
       .clearType = kClearTypeFailedRank},
  };
  service.state = ir::IrRankingSnapshotState::Succeeded;
  service.ranking = ranking;
  projected = projectMusicSelectRanking(service, 1);
  expect(projected.state == MusicSelectRankingState::Finish &&
             projected.totalPlayers == 3 && projected.rank == 3 &&
             projected.offset == 1,
         "finished ranking sorts scores, computes tied ranks, and retains the offset");
  expect(projected.clearCounts[8] == 1 && projected.clearCounts[5] == 1 &&
             projected.clearCounts[1] == 1,
         "Aso clear ranks map to Beatoraja ClearType IDs");
  expect(projected.entries.size() == 3 &&
             projected.entries[2].playerType == 1 &&
             projected.entries[0].clearType == 8 &&
             projected.entries[1].clearType == 1 &&
             projected.entries[0].rank == 1 &&
             projected.entries[1].rank == 1 &&
             projected.entries[2].rank == 3,
         "ranking rows preserve Beatoraja player and clear indexes");
  expect(projected.entries[2].name == "YOU" &&
             projected.entries[0].name == "Top" &&
             projected.entries[1].name == "Failed",
         "skin ranking names identify the current user as YOU and preserve other players");
  expect(ranking->entries[1].playerName == "Player" &&
             ranking->entries[1].currentUser,
         "skin name projection preserves the native provider account name");

  service.state = ir::IrRankingSnapshotState::TransientFailure;
  service.ranking.reset();
  projected = projectMusicSelectRanking(service, 0);
  expect(projected.state == MusicSelectRankingState::Fail,
         "a completed service failure is RankingData.FAIL");

  service.state = ir::IrRankingSnapshotState::Closed;
  projected = projectMusicSelectRanking(service, 0);
  expect(projected.state == MusicSelectRankingState::None,
         "a closed service is RankingData.NONE");
}

void testShowsFirstPageWithoutWaitingForTheLeaderboard() {
  auto ranking = std::make_shared<ir::IrChartRanking>();
  ranking->totalPlayers = 200;
  ranking->nextPageToken = "second-page";
  ranking->entries = {
      {.rank = 1, .playerName = "Top", .score = 1900,
       .clearType = kClearTypeHardClearRank},
      {.rank = 2, .playerName = "Account", .score = 1800,
       .clearType = kClearTypeNormalClearRank, .currentUser = true},
  };
  ir::IrRankingSnapshot service{
      .state = ir::IrRankingSnapshotState::Succeeded, .ranking = ranking};
  const auto checkVisible = [&](bool loading, bool blocked) {
    service.loadingNextPage = loading;
    service.paginationBlocked = blocked;
    MusicSelectPropertyRuntimeSnapshot runtime;
    runtime.irOnline = true;
    runtime.ranking = projectMusicSelectRanking(service, 0);
    expect(runtime.ranking.state == MusicSelectRankingState::Finish &&
               runtime.ranking.entries.size() == 2 &&
               runtime.ranking.totalPlayers == 200 && runtime.ranking.rank == 2,
           "received rows and authoritative totals remain visible during pagination");
    const auto values = projectMusicSelectProperties(AppSettings{}, MusicSelectBarManagerSnapshot{}, runtime);
    const auto integer = [&](int id, int value) {
      const auto found = values.integers.find(id);
      return found != values.integers.end() && found->second == value;
    };
    expect(integer(380, 1900) && integer(390, 1) && integer(179, 2) &&
               integer(180, 200) && values.strings.contains(121) &&
               values.strings.at(121) == "YOU",
           "first-page scores and own row reach the skin before later pages");
    expect(!values.integers.contains(216) && !values.integers.contains(226) &&
               !values.rates.contains(217) && !values.floats.contains(227),
           "partial pages do not publish incomplete clear counts or percentages");
  };
  checkVisible(false, false);
  checkVisible(true, false);
  checkVisible(false, true);

  service.loadingNextPage = false;
  service.paginationBlocked = false;
  ranking->nextPageToken.reset();
  ranking->totalPlayers = 2;
  MusicSelectPropertyRuntimeSnapshot runtime;
  runtime.ranking = projectMusicSelectRanking(service, 0);
  const auto values = projectMusicSelectProperties(AppSettings{}, MusicSelectBarManagerSnapshot{}, runtime);
  expect(values.integers.at(216) == 1 && values.integers.at(226) == 2 &&
             values.rates.at(217) == 0.5 && values.floats.at(227) == 1.0,
         "complete leaderboard publishes exact clear counts and percentages");
}

void testCacheIdentityIncludesIrAccountEvidence() {
  ir::IrRankingRequest request{
      .profileId = "profile",
      .providerId = "provider",
      .serverOrigin = "https://example.test",
      .chart = {.keyMode = 7,
                .chartMd5 = "md5",
                .chartSha256 = "sha256",
                .totalNotes = 1234},
  };
  const auto first = musicSelectRankingCacheKey(request, 11);
  expect(first == musicSelectRankingCacheKey(request, 11),
         "unchanged request and account evidence reuse ranking cache identity");
  expect(first != musicSelectRankingCacheKey(request, 12),
         "changed IR account evidence invalidates ranking cache identity");
}
} // namespace

int main(int argc, char **argv) {
  testProjectsServiceStateIntoBeatorajaRankingData();
  testShowsFirstPageWithoutWaitingForTheLeaderboard();
  testCacheIdentityIncludesIrAccountEvidence();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_ranking_tests", failures,
      "music-select ranking assertion(s) failed",
      "music-select ranking tests passed");
}
