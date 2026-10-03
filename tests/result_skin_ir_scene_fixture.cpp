#include "skin/ResultSkinIr.h"

#include <cstdlib>
#include <iostream>
#include <memory>

#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1

static long long clockMicros = 1'100;
long long nowMicros() { return clockMicros; }
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct RankingService {
  ir::IrRankingSnapshot current;
  int opened = 0;
  int refreshed = 0;
  int pages = 0;
  int pageAttempts = 0;
  bool acceptPages = true;
  std::vector<std::shared_ptr<const ir::IrChartRanking>> immediatePages;
  std::uint64_t nextGeneration = 1;
  ir::IrRankingSnapshot snapshot() const { return current; }
  std::uint64_t open(ir::IrRankingRequest request) {
    ++opened;
    request.generation = nextGeneration++;
    current = {.revision = current.revision + 1,
               .generation = request.generation,
               .state = ir::IrRankingSnapshotState::Loading,
               .request = std::move(request)};
    return current.generation;
  }
  std::uint64_t refresh(ir::IrRankingRequest request) {
    ++refreshed;
    return open(std::move(request));
  }
  bool loadNextPage(std::uint64_t generation) {
    require(generation == current.generation, "paginate only current generation");
    ++pageAttempts;
    if (!acceptPages) return false;
    ++pages;
    ++current.revision;
    current.loadingNextPage = true;
    if (!immediatePages.empty()) {
      auto completed = immediatePages.front();
      immediatePages.erase(immediatePages.begin());
      finish(std::move(completed));
    }
    return true;
  }
  void finish(std::shared_ptr<const ir::IrChartRanking> ranking) {
    ++current.revision;
    current.state = ir::IrRankingSnapshotState::Succeeded;
    current.ranking = std::move(ranking);
    current.loadingNextPage = false;
  }
};
struct SubmissionService {
  ir::IrAttemptStatusSnapshot current;
  ir::IrAttemptStatusSnapshot status(std::string_view, std::string_view) const {
    return current;
  }
};
struct RankingModal {
  bool visible = false;
  bool isOpen() const { return visible; }
};
struct ResultScene {
  struct {
    std::shared_ptr<RankingService> irRankingService = std::make_shared<RankingService>();
    std::shared_ptr<SubmissionService> irSubmissionService = std::make_shared<SubmissionService>();
  } context;
  struct Submission { std::string attemptId = "attempt"; };
  struct Local {
    struct {
      std::shared_ptr<Submission> irSubmission = std::make_shared<Submission>();
    } persistenceOptions;
  } local;
  const Local *localSource() const { return &local; }
  bool resultSkinSession = true;
  long long resultSkinStartedMicros = 1'000;
  std::unique_ptr<RankingModal> rankingsModal = std::make_unique<RankingModal>();
  std::optional<ir::IrRankingRequest> resultSkinRankingRequest = ir::IrRankingRequest{
      .profileId = "profile", .providerId = "tachi", .serverOrigin = "https://example.test",
      .chart = {.keyMode = 7, .chartSha256 = std::string(64, 'a'), .totalNotes = 100}};
  std::uint64_t resultSkinRankingGeneration = 0;
  std::uint64_t resultSkinRankingRevision = 0;
  result_skin_ir::RankingData resultSkinRanking;
  int resultSkinRankingOffset = 0;
  bool resultSkinRankingOffsetManuallyChosen = false;
  bool resultSkinRankingRefreshPending = false;
  std::optional<int> resultSkinPreviousIrRank;
  result_skin_ir::SubmissionTimers resultSkinSubmissionTimers;
  void updateSelectedResultSkinRankings();
};

PRODUCTION_RESULT_IR_UPDATE

int main() {
  ResultScene nearby;
  auto &nearbyService = *nearby.context.irRankingService;
  nearby.resultSkinRankingGeneration = nearbyService.open(*nearby.resultSkinRankingRequest);
  auto distant = std::make_shared<ir::IrChartRanking>();
  distant->totalPlayers = 6000;
  distant->nextPageToken = "page-2";
  distant->entries = {{.rank = 1, .score = 200}};
  distant->nearbyEntries = {{.rank = 4999, .score = 101},
      {.rank = 5000, .score = 100, .currentUser = true},
      {.rank = 5001, .score = 99}};
  nearbyService.finish(distant);
  nearby.updateSelectedResultSkinRankings();
  require(nearbyService.pages == 1 && nearbyService.current.loadingNextPage &&
              nearby.resultSkinRanking.currentUserRank == 5000 &&
              nearby.resultSkinRankingOffset == 4995 && !nearby.resultSkinRanking.clearCounts,
          "result scene centers distant own rank immediately while page two is still pending");
  const auto nearbyRows = ir::rankingWindow(nearby.resultSkinRanking.entries,
      nearby.resultSkinRanking.nearbyEntries, nearby.resultSkinRanking.nearbyOffset,
      nearby.resultSkinRankingOffset);
  require(nearbyRows.size() == 3 && nearbyRows[1].currentUser,
          "scene-selected offset exposes the fetched nearby player window");
  nearby.context.irSubmissionService->current = {
      .found = true, .state = ir::IrOutboxState::Succeeded};
  nearby.updateSelectedResultSkinRankings();
  require(nearbyService.refreshed == 1, "score submission refreshes the early nearby rank");
  auto improved = std::make_shared<ir::IrChartRanking>(*distant);
  for (auto &entry : improved->nearbyEntries) entry.rank -= 500;
  nearbyService.finish(improved);
  nearby.updateSelectedResultSkinRankings();
  require(nearby.resultSkinRanking.currentUserRank == 4500 &&
              nearby.resultSkinRankingOffset == 4495,
          "automatic result window follows improved own rank after upload refresh");
  nearby.resultSkinRankingOffset = 0;
  nearby.resultSkinRankingOffsetManuallyChosen = true;
  nearbyService.finish(distant);
  nearby.updateSelectedResultSkinRankings();
  require(nearby.resultSkinRankingOffset == 0,
          "explicit user navigation survives later rank changes");

  ResultScene scene;
  auto &service = *scene.context.irRankingService;
  scene.resultSkinRankingGeneration = service.open(*scene.resultSkinRankingRequest);
  auto first = std::make_shared<ir::IrChartRanking>();
  first->totalPlayers = 12;
  first->nextPageToken = "second";
  for (int rank = 1; rank <= 10; ++rank) {
    first->entries.push_back({.rank = rank, .score = 200 - rank});
  }
  service.finish(first);
  scene.updateSelectedResultSkinRankings();
  require(service.pages == 1 && scene.resultSkinRanking.totalPlayers == 12 &&
              !scene.resultSkinRanking.clearCounts,
          "result scene automatically requests page two and retains authoritative total");
  scene.updateSelectedResultSkinRankings();
  require(service.pages == 1, "same service revision does not duplicate next-page request");
  auto complete = std::make_shared<ir::IrChartRanking>(*first);
  complete->nextPageToken.reset();
  complete->entries.push_back({.rank = 11, .score = 150, .currentUser = true});
  complete->entries.push_back({.rank = 12, .score = 140});
  service.finish(complete);
  scene.updateSelectedResultSkinRankings();
  require(scene.resultSkinRanking.currentUserRank == 11 &&
              scene.resultSkinRanking.clearCounts && scene.resultSkinRankingOffset == 6,
          "result scene exposes completed later-page own rank and centers ranking window");
  scene.context.irSubmissionService->current = {
      .found = true, .state = ir::IrOutboxState::Uploading,
      .activeRequest = ir::IrActiveRequestKind::Submit, .requestAttemptCount = 1};
  scene.updateSelectedResultSkinRankings();
  require(scene.resultSkinSubmissionTimers.startedMicros[0] == 100,
          "upload begin is relative to the result scene clock");
  clockMicros = 1'300;
  scene.context.irSubmissionService->current.state = ir::IrOutboxState::Succeeded;
  scene.context.irSubmissionService->current.activeRequest = ir::IrActiveRequestKind::None;
  scene.updateSelectedResultSkinRankings();
  require(service.refreshed == 1 && scene.resultSkinSubmissionTimers.startedMicros[1] == 300,
          "successful upload refreshes rankings and activates transmission success timer");
  scene.updateSelectedResultSkinRankings();
  require(service.refreshed == 1, "stable success does not continuously restart fetching");

  // Opening the native modal changes the service generation. The skin follows
  // the matching request without replacing a modal owned request.
  scene.rankingsModal->visible = true;
  const auto modalGeneration = service.open(*scene.resultSkinRankingRequest);
  service.finish(complete);
  scene.resultSkinRankingRefreshPending = true;
  const int opened = service.opened;
  scene.updateSelectedResultSkinRankings();
  require(service.opened == opened && scene.resultSkinRankingGeneration == modalGeneration &&
              scene.resultSkinRanking.currentUserRank == 11,
          "native modal keeps ownership while matching data remains visible to skin");
  auto partialModalRanking = std::make_shared<ir::IrChartRanking>(*complete);
  partialModalRanking->nextPageToken = "more";
  service.finish(partialModalRanking);
  const int attemptsBeforeModalUpdates = service.pageAttempts;
  for (int frame = 0; frame < 30; ++frame) {
    scene.updateSelectedResultSkinRankings();
  }
  require(service.pageAttempts == attemptsBeforeModalUpdates,
          "result skin leaves pagination to the open native modal");
  scene.rankingsModal->visible = false;
  scene.updateSelectedResultSkinRankings();
  require(service.refreshed == 2, "deferred refresh resumes when the modal closes");
  service.current.state = ir::IrRankingSnapshotState::Cancelled;
  ++service.current.revision;
  scene.updateSelectedResultSkinRankings();
  require(service.current.state == ir::IrRankingSnapshotState::Loading,
          "submission invalidation during a fetch restarts the scene request");
  auto unrelated = *scene.resultSkinRankingRequest;
  unrelated.profileId = "other";
  scene.rankingsModal->visible = true;
  service.open(unrelated);
  service.finish(complete);
  const auto ownRank = scene.resultSkinRanking.currentUserRank;
  scene.updateSelectedResultSkinRankings();
  require(service.current.request->profileId == "other" &&
              scene.resultSkinRanking.currentUserRank == ownRank,
          "unrelated modal request is neither overwritten nor projected");

  ResultScene paginated;
  auto &immediate = *paginated.context.irRankingService;
  paginated.resultSkinRankingGeneration =
      immediate.open(*paginated.resultSkinRankingRequest);
  auto pageOne = std::make_shared<ir::IrChartRanking>();
  pageOne->totalPlayers = 3;
  pageOne->nextPageToken = "second";
  pageOne->entries.push_back({.rank = 1, .score = 190});
  auto pageTwo = std::make_shared<ir::IrChartRanking>(*pageOne);
  pageTwo->nextPageToken = "third";
  pageTwo->entries.push_back({.rank = 2, .score = 180});
  auto pageThree = std::make_shared<ir::IrChartRanking>(*pageTwo);
  pageThree->nextPageToken.reset();
  pageThree->entries.push_back({.rank = 3, .score = 170, .currentUser = true});
  immediate.immediatePages = {pageTwo, pageThree};
  immediate.finish(pageOne);
  paginated.updateSelectedResultSkinRankings();
  require(immediate.pages == 1 && !paginated.resultSkinRanking.clearCounts,
          "immediate second page retains partial aggregate state");
  paginated.updateSelectedResultSkinRankings();
  require(immediate.pages == 2 && paginated.resultSkinRanking.clearCounts &&
              paginated.resultSkinRanking.currentUserRank == 3,
          "immediate page completion cannot skip the third page and its own rank");
  paginated.updateSelectedResultSkinRankings();
  require(immediate.pages == 2, "complete ranking never repeats pagination");

  ResultScene queued;
  auto &busy = *queued.context.irRankingService;
  queued.resultSkinRankingGeneration = busy.open(*queued.resultSkinRankingRequest);
  busy.finish(pageTwo);
  busy.immediatePages = {pageThree};
  busy.acceptPages = false;
  queued.updateSelectedResultSkinRankings();
  require(busy.pageAttempts == 1 && busy.pages == 0,
          "an older active request temporarily prevents next-page queuing");
  busy.acceptPages = true;
  queued.updateSelectedResultSkinRankings();
  require(busy.pages == 1 && queued.resultSkinRanking.clearCounts,
          "unchanged snapshot retries pagination after the old worker exits");
  busy.finish(pageTwo);
  busy.current.paginationBlocked = true;
  const int blockedAttempts = busy.pageAttempts;
  for (int frame = 0; frame < 5; ++frame) queued.updateSelectedResultSkinRankings();
  require(busy.pageAttempts == blockedAttempts,
          "blocked pagination does not cause a per-frame retry loop");
}
