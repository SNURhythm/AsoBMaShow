#include "scene/play/GameplaySkinIrTarget.h"
#include "ir/IrProfileSettings.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>

void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

struct RankingService {
  ir::IrRankingSnapshot current;
  bool paused = false;
  bool completePagesImmediately = false;
  int pagesRequested = 0;
  int blockedPageAttempts = 0;
  ir::IrRankingSnapshot snapshot() const { return current; }
  bool loadNextPage(std::uint64_t generation) {
    if (!completePagesImmediately) return false;
    require(generation == current.generation, "page request must own the generation");
    if (blockedPageAttempts > 0) {
      --blockedPageAttempts;
      return false;
    }
    ++pagesRequested;
    ++current.revision;
    auto ranking = std::make_shared<ir::IrChartRanking>(*current.ranking);
    ranking->entries.push_back({.rank = pagesRequested + 1, .score = 100 - pagesRequested});
    ranking->nextPageToken = pagesRequested < 2
                                ? std::optional<std::string>{"page-3"}
                                : std::nullopt;
    current.ranking = std::move(ranking);
    return true;
  }
};

struct GamePlayScene {
  struct {
    std::shared_ptr<RankingService> irRankingService = std::make_shared<RankingService>();
    std::atomic<std::uint64_t> irAccountEvidenceRevision{0};
    struct {
      std::string skinTargetId = "MAX";
      std::map<std::string, ir::IrProviderSettings> irProviders{
          {"tachi", {.enabled = true, .serverOrigin = "https://example.test"}}};
    } settings;
    struct {
      struct Profile { std::string id = "profile"; } profile;
      const Profile &activeProfile() const { return profile; }
    } profileManager;
  } context;
  struct State { bool isEnding = false; } ownedState;
  State *state = &ownedState;
  bool guidedAccessReminderBackground = false;
  std::optional<PlayfieldIrTargetState> activeSkinIrTarget;
  std::string skinIrTargetSelection = "MAX";
  std::optional<ir::IrRankingRequest> skinIrRankingRequest = ir::IrRankingRequest{
      .profileId = "profile", .providerId = "tachi",
      .serverOrigin = "https://example.test",
      .chart = {.keyMode = 7, .chartSha256 = std::string(64, 'a'), .totalNotes = 100}};
  std::uint64_t skinIrRankingGeneration = 1;
  std::uint64_t skinIrRankingRevision = 0;
  std::uint64_t skinIrAccountRevision = 0;
  int skinIrLocalBestScore = 0;
  std::optional<int> skinIrPreviousUserRank;
  int configured = 0;
  void configureSkinIrTarget() {
    ++configured;
    skinIrPreviousUserRank.reset();
    skinIrAccountRevision = context.irAccountEvidenceRevision.load();
    skinIrRankingRevision = 0;
    ++skinIrRankingGeneration;
    auto &service = *context.irRankingService;
    service.current = {
        .revision = service.current.revision + 1,
        .generation = skinIrRankingGeneration,
        .state = service.paused ? ir::IrRankingSnapshotState::Closed
                               : ir::IrRankingSnapshotState::Loading,
        .request = service.paused ? std::nullopt : skinIrRankingRequest};
  }
  void updateSkinIrTarget();
};

PRODUCTION_GAMEPLAY_IR_UPDATE

int main() {
  GamePlayScene scene;
  auto &service = *scene.context.irRankingService;
  auto ranking = std::make_shared<ir::IrChartRanking>();
  ranking->providerId = "tachi";
  ranking->chart = scene.skinIrRankingRequest->chart;
  ranking->totalPlayers = 1;
  ranking->entries = {{.rank = 4, .score = 150, .currentUser = true}};
  service.current = {.revision = 1, .generation = 1,
      .state = ir::IrRankingSnapshotState::Succeeded,
      .request = scene.skinIrRankingRequest, .ranking = ranking};
  scene.updateSkinIrTarget();
  require(scene.skinIrPreviousUserRank == 4 && !scene.activeSkinIrTarget,
          "ordinary pacemaker captures actual IR rank without changing target");

  scene.state->isEnding = true;
  ranking->entries[0].rank = 1;
  ++service.current.revision;
  scene.updateSkinIrTarget();
  require(scene.skinIrPreviousUserRank == 4,
          "ending freezes previous rank before asynchronous upload changes it");
  scene.state->isEnding = false;
  service.current.state = ir::IrRankingSnapshotState::Cancelled;
  service.current.ranking.reset();
  ++service.current.revision;
  scene.updateSkinIrTarget();
  require(scene.configured == 1 && !scene.skinIrPreviousUserRank &&
              service.current.state == ir::IrRankingSnapshotState::Loading,
          "mid-play invalidation starts one fresh request and clears old rank");
  scene.updateSkinIrTarget();
  require(scene.configured == 1, "loading request is not repeatedly reopened");

  service.paused = true;
  service.current.state = ir::IrRankingSnapshotState::Cancelled;
  ++service.current.revision;
  scene.updateSkinIrTarget();
  require(scene.configured == 2 &&
              service.current.state == ir::IrRankingSnapshotState::Closed,
          "paused service can refuse a recovery request");
  scene.updateSkinIrTarget();
  scene.updateSkinIrTarget();
  require(scene.configured == 2, "persistent Closed does not cause a retry loop");
  service.current.request = scene.skinIrRankingRequest;
  service.current.state = ir::IrRankingSnapshotState::AuthenticationRequired;
  ++service.current.revision;
  scene.updateSkinIrTarget();
  scene.updateSkinIrTarget();
  require(scene.configured == 2, "authentication failure is not retried every frame");
  ++scene.context.irAccountEvidenceRevision;
  scene.updateSkinIrTarget();
  scene.updateSkinIrTarget();
  require(scene.configured == 3,
          "account evidence change reconfigures once, even while service stays closed");

  GamePlayScene paged;
  paged.context.settings.skinTargetId = "IR_RANK_1";
  paged.skinIrTargetSelection = "IR_RANK_1";
  paged.activeSkinIrTarget = PlayfieldIrTargetState{.targetId = "IR_RANK_1"};
  auto &pagedService = *paged.context.irRankingService;
  auto first = std::make_shared<ir::IrChartRanking>();
  first->providerId = "tachi";
  first->chart = paged.skinIrRankingRequest->chart;
  first->totalPlayers = 3;
  first->entries = {{.rank = 1, .playerName = "Top", .score = 190}};
  first->nextPageToken = "page-2";
  pagedService.current = {.revision = 1, .generation = 1,
      .state = ir::IrRankingSnapshotState::Succeeded,
      .request = paged.skinIrRankingRequest, .ranking = first};
  pagedService.completePagesImmediately = true;
  paged.updateSkinIrTarget();
  paged.updateSkinIrTarget();
  require(pagedService.pagesRequested == 2 &&
              paged.activeSkinIrTarget->score == 190,
          "immediately completed pages keep pagination advancing to the final target");
  pagedService.current.ranking = first;
  ++pagedService.current.revision;
  pagedService.pagesRequested = 0;
  pagedService.blockedPageAttempts = 1;
  paged.updateSkinIrTarget();
  paged.updateSkinIrTarget();
  paged.updateSkinIrTarget();
  require(pagedService.pagesRequested == 2,
          "busy worker retries next-page queueing even without a new snapshot revision");
}
