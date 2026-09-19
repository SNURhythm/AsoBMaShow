#include "scene/play/Pacemaker.h"

// Substitute the repository factory boundary only. The unchanged scene methods
// below run with the production task, resolver, and pacemaker policy.
struct BestReplaySceneSource {
  replay::BestReplayResolver resolver;
};
namespace replay {
BestReplayResolver makeRuntimeBestReplayResolver(BestReplaySceneSource &source) {
  return source.resolver;
}
} // namespace replay

namespace {
class GamePlayScene {
public:
  explicit GamePlayScene(BestReplaySceneSource &source) : context{source} {}
  struct Context { BestReplaySceneSource &replayRepository; } context;
  bms_parser::Chart *chart = nullptr;
  std::optional<ScoreBestSnapshot> activePacemakerBest;
  pacemaker::Target activeBestScoreTarget;
  pacemaker::Target activePacemakerTarget;
  ReplayRecordTask bestReplayLoadTask;
  void startBestReplayLoad(std::string attemptId, std::filesystem::path chartPath);
  void applyPendingBestReplay();
  void applyLoadedBestReplay(const ReplayData &loaded);
  void stopBestReplayLoad();
};

#include "best_replay_scene_methods.inc"

std::shared_ptr<ReplayData> threeNoteGhost() {
  auto data = std::make_shared<ReplayData>();
  data->finalScore = 3;
  for (const auto [judgement, score] :
       {std::pair{PGreat, 2}, std::pair{Great, 3}, std::pair{Good, 3}}) {
    ReplayEvent event;
    event.action = ReplayEventAction::Press;
    event.judgement = judgement;
    event.score = score;
    data->events.push_back(event);
  }
  return data;
}

void configureFixtureTargets(GamePlayScene &scene, bms_parser::Chart &chart) {
  chart.Meta.TotalNotes = 3;
  scene.chart = &chart;
  ScoreBestSnapshot best;
  best.score = 3;
  best.maxScore = 6;
  scene.activePacemakerBest = best;
  scene.activeBestScoreTarget = pacemaker::targetFromBestSnapshot(chart, best);
  scene.activePacemakerTarget = pacemaker::targetFromSelection(
      chart, pacemaker::kTargetBest, best, nullptr);
}

void finishScene(GamePlayScene &scene) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (scene.bestReplayLoadTask.active() && std::chrono::steady_clock::now() < deadline) {
    scene.applyPendingBestReplay();
    std::this_thread::yield();
  }
  assert(!scene.bestReplayLoadTask.active());
}

void testSceneUpdatesPersonalBestGhostWithoutChangingSelectedTarget() {
  BestReplaySceneSource source{resolverWith([](const auto &, const auto &, auto &) {
    return threeNoteGhost();
  })};
  bms_parser::Chart chart;
  GamePlayScene scene(source);
  configureFixtureTargets(scene, chart);
  scene.startBestReplayLoad("best", "song.bms");
  // The worker cannot update scene state before the application consumes it.
  assert(!scene.activeBestScoreTarget.usesReplayProgression);
  finishScene(scene);
  assert(scene.activeBestScoreTarget.usesReplayProgression);
  assert(pacemaker::targetScoreAtPlayedNotes(scene.activeBestScoreTarget, 1) == 2);
  assert(!scene.activePacemakerTarget.usesReplayProgression);
  assert(pacemaker::targetScoreAtPlayedNotes(scene.activePacemakerTarget, 1) == 1);
  scene.applyPendingBestReplay();
  assert(scene.activeBestScoreTarget.scoreAfterNotes == std::vector<int>({0, 2, 3, 3}));
}

void testSceneIgnoresLoadedReplayWithoutChartOrBestSnapshot() {
  BestReplaySceneSource source{resolverWith([](const auto &, const auto &, auto &) {
    return threeNoteGhost();
  })};
  bms_parser::Chart chart;
  GamePlayScene scene(source);
  configureFixtureTargets(scene, chart);
  scene.startBestReplayLoad("best", "song.bms");
  scene.chart = nullptr;
  finishScene(scene);
  assert(!scene.activeBestScoreTarget.usesReplayProgression);
  scene.chart = &chart;
  scene.startBestReplayLoad("best", "song.bms");
  scene.activePacemakerBest.reset();
  finishScene(scene);
  assert(!scene.activeBestScoreTarget.usesReplayProgression);
}

void testSceneStopPreventsLateReplayFromApplyingToAReplacementChart() {
  std::promise<void> entered;
  BestReplaySceneSource source{resolverWith([&](const auto &, const auto &, auto &cancelled) {
    entered.set_value();
    waitCancelled(cancelled);
    return threeNoteGhost();
  })};
  bms_parser::Chart first, replacement;
  GamePlayScene scene(source);
  configureFixtureTargets(scene, first);
  scene.startBestReplayLoad("old", "old.bms");
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  scene.stopBestReplayLoad();
  configureFixtureTargets(scene, replacement);
  scene.applyPendingBestReplay();
  assert(!scene.activeBestScoreTarget.usesReplayProgression);
  assert(!scene.bestReplayLoadTask.active());
}
} // namespace
