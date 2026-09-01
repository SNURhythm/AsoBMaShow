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
  ranking->totalPlayers = 12;
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
  service.loadingNextPage = true;
  projected = projectMusicSelectRanking(service, 1);
  expect(projected.state == MusicSelectRankingState::Access,
         "ranking remains ACCESS while continuation pages are loading");

  service.loadingNextPage = false;
  service.paginationBlocked = true;
  projected = projectMusicSelectRanking(service, 1);
  expect(projected.state == MusicSelectRankingState::Fail,
         "a failed continuation leaves selector RankingData in FAIL");

  service.paginationBlocked = false;
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
} // namespace

int main(int argc, char **argv) {
  testProjectsServiceStateIntoBeatorajaRankingData();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_ranking_tests", failures,
      "music-select ranking assertion(s) failed",
      "music-select ranking tests passed");
}
