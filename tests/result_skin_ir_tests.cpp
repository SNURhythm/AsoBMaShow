#include "skin/ResultSkinIr.h"

#include <iostream>
#include <limits>
#include <memory>

int main() {
  int failures = 0;
  const auto check = [&](bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
  };
  auto ranking = std::make_shared<ir::IrChartRanking>();
  ranking->totalPlayers = 12;
  ranking->nextPageToken = "second";
  for (int index = 0; index < 10; ++index) {
    ranking->entries.push_back({.rank = index + 1,
        .playerName = "Player " + std::to_string(index + 1),
        .score = 100 - index, .clearType = kClearTypeHardClearRank});
  }
  ir::IrRankingSnapshot source{.state = ir::IrRankingSnapshotState::Succeeded,
                              .ranking = ranking};
  auto projected = result_skin_ir::projectRanking(source);
  check(projected.totalPlayers == 12 && projected.entries.size() == 10 &&
            !projected.currentUserRank && !projected.clearCounts,
        "partial ranking retains authoritative total without inventing aggregates or own rank");
  ranking->entries.push_back({.rank = 11, .playerName = "AccountName", .score = 70,
      .clearType = kClearTypeFullComboRank, .currentUser = true});
  ranking->entries.push_back({.rank = 12, .playerName = "Last", .score = 60,
      .clearType = kClearTypeFailedRank});
  ranking->nextPageToken.reset();
  projected = result_skin_ir::projectRanking(source);
  check(projected.currentUserRank == 11 && projected.entries.size() == 12 &&
            projected.clearCounts && (*projected.clearCounts)[6] == 10 &&
            (*projected.clearCounts)[8] == 1 && (*projected.clearCounts)[1] == 1,
        "completed pages supply own rank beyond top ten and full clear histogram");
  check(projected.entries[10].playerName == "YOU" &&
            projected.entries[10].currentUser &&
            projected.entries[11].playerName == "Last",
        "result skin identifies the current user as YOU and preserves other players");
  check(ranking->entries[10].playerName == "AccountName" &&
            ranking->entries[10].currentUser,
        "result skin projection preserves the native provider account name");
  source.paginationBlocked = true;
  check(!result_skin_ir::projectRanking(source).clearCounts,
        "blocked pagination never claims complete histogram");
  source.paginationBlocked = false;
  ranking->totalPlayers = 13;
  check(!result_skin_ir::projectRanking(source).clearCounts,
        "missing entry without next-page token is not a complete population");
  source.state = ir::IrRankingSnapshotState::Cancelled;
  check(!result_skin_ir::projectRanking(source).totalPlayers,
        "cancelled request does not publish retained rows");
  ranking = std::make_shared<ir::IrChartRanking>();
  source = {.state = ir::IrRankingSnapshotState::Succeeded, .ranking = ranking};
  projected = result_skin_ir::projectRanking(source);
  check(projected.totalPlayers == 0 && projected.clearCounts,
        "successful empty ranking supplies zero counts");

  ranking->totalPlayers = 6000;
  ranking->nextPageToken = "more";
  ranking->entries = {{.rank = 1, .playerName = "Top", .score = 190}};
  ranking->nearbyEntries = {
      {.rank = 4999, .playerName = "Above", .score = 102},
      {.rank = 5000, .score = 100, .currentUser = true},
      {.rank = 5001, .playerName = "Below", .score = 98}};
  projected = result_skin_ir::projectRanking(source);
  check(projected.currentUserRank == 5000 && !projected.clearCounts &&
            projected.entries.size() == 1,
        "nearby own rank is available without mistaking sparse rows for complete pages");
  auto rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                                 projected.nearbyOffset, 4995);
  check(rows.size() == 3 && rows[1].currentUser && rows[1].rank == 5000 &&
            rows[1].playerName == "YOU",
        "result's initial own-rank offset shows available nearby rows without needing page 50");
  rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                           projected.nearbyOffset, 0);
  check(rows.size() == 1 && rows[0].rank == 1,
        "top-of-leaderboard navigation still shows the top page");
  ranking->nearbyEntries[0].rank = 4998;
  ranking->nearbyEntries.insert(ranking->nearbyEntries.begin(),
      {.rank = 4998, .playerName = "Tied above", .score = 102});
  projected = result_skin_ir::projectRanking(source);
  rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                           projected.nearbyOffset, 4995);
  check(rows.size() == 4 && rows[0].rank == 4998 && rows[1].rank == 4998 &&
            rows[2].rank == 5000 && rows[2].currentUser,
        "nearby window preserves competition ranks and tied neighbors");

  ranking->entries.clear();
  for (int rank = 1; rank <= 100; ++rank) ranking->entries.push_back({.rank = rank});
  ranking->nearbyEntries.clear();
  for (int rank = 96; rank <= 106; ++rank) {
    ranking->nearbyEntries.push_back({.rank = rank, .currentUser = rank == 101});
  }
  projected = result_skin_ir::projectRanking(source);
  rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                           projected.nearbyOffset, 90);
  check(rows.size() == 10 && rows.front().rank == 91 && rows.back().rank == 100,
        "nearby overlap cannot displace loaded top rows during explicit navigation");
  rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                           projected.nearbyOffset, 96, 10, true);
  check(rows.size() == 10 && rows[4].currentUser && rows[4].rank == 101,
        "automatic result centering uses nearby rows across the prefix boundary");
  ranking->nearbyEntries = {{.rank = 1}, {.rank = 2, .currentUser = true}, {.rank = 102}};
  projected = result_skin_ir::projectRanking(source);
  rows = ir::rankingWindow(projected.entries, projected.nearbyEntries,
                           projected.nearbyOffset, 0, 10, true);
  check(rows.size() == 3 && rows[1].currentUser && rows[1].rank == 2,
        "own player tied near the top remains visible even when outside the first hundred rows");

  result_skin_ir::SubmissionTimers timers;
  ir::IrAttemptStatusSnapshot status;
  check(!timers.observe(status, 100) && !timers.startedMicros[0],
        "absent upload leaves IR timers off");
  status.found = true;
  timers.observe(status, 200);
  check(!timers.startedMicros[0], "queued upload does not claim network activity");
  status.state = ir::IrOutboxState::Uploading;
  status.activeRequest = ir::IrActiveRequestKind::Submit;
  status.requestAttemptCount = 1;
  timers.observe(status, 300);
  timers.observe(status, 400);
  check(timers.startedMicros[0] == 300 && !timers.startedMicros[1] &&
            !timers.startedMicros[2],
        "begin timer is latched to actual submission activity");
  status.state = ir::IrOutboxState::Pending;
  status.activeRequest = ir::IrActiveRequestKind::None;
  status.consecutiveFailureCount = 1;
  timers.observe(status, 500);
  check(timers.startedMicros[2] == 500 && !timers.startedMicros[1],
        "retryable failure activates failed timer");
  status.state = ir::IrOutboxState::Succeeded;
  status.consecutiveFailureCount = 0;
  check(timers.observe(status, 600) && !timers.observe(status, 700) &&
            timers.startedMicros[1] == 600,
        "upload success activates success timer and requests one ranking refresh");
  return failures ? 1 : 0;
}
