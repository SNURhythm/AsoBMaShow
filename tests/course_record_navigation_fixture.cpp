#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <string>

namespace bms_parser { struct Chart {}; }
struct ReplayData { struct { std::string BmsPath = "stage.bms"; } chartMeta; };
struct CoursePlaySession {
  int currentIndex = 1;
  bool prepared = true;
  bool hasCourseReplayStage(int) const { return true; }
  auto currentCourseReplayStageReplay() { return std::make_shared<ReplayData>(); }
  void applyReplayStagePlayOptions(const ReplayData &) {}
  std::unique_ptr<bms_parser::Chart> takePreparedCourseChart(int) {
    return prepared ? std::make_unique<bms_parser::Chart>() : nullptr;
  }
};
namespace play_options {
auto prepareReplayChart(const std::string &, const ReplayData &, std::atomic_bool &) {
  return std::unique_ptr<bms_parser::Chart>{};
}
}
struct StartOptions {
  std::shared_ptr<CoursePlaySession> courseSession;
  void *returnScene = nullptr;
  std::string pacemakerTarget = "OFF", tableName, tableLevel;
};
StartOptions makeCourseReplayStageStartOptions(std::shared_ptr<CoursePlaySession> session,
                                               std::shared_ptr<ReplayData>) {
  return {.courseSession = std::move(session)};
}
struct Context;
struct GamePlayScene {
  Context &context;
  StartOptions options;
  GamePlayScene(Context &value, std::unique_ptr<bms_parser::Chart>, StartOptions start)
      : context(value), options(std::move(start)) {}
  bool startCourseReplayChartAtCurrentIndex();
};
struct SceneManager {
  std::unique_ptr<GamePlayScene> gameplay;
  void *returned = nullptr;
  bool fallback = false;
  void changeScene(std::unique_ptr<GamePlayScene> scene, bool retained) {
    assert(!retained);
    gameplay = std::move(scene);
  }
  void changeScene(void *scene, bool retained) { assert(!retained); returned = scene; }
  void changeScene(const char *) { fallback = true; }
};
struct Context {
  struct {
    bool cancelled = false;
    void stop() {}
    void loadChart(bms_parser::Chart &, bool, std::atomic_bool &cancel) { cancel = cancelled; }
  } jukebox;
  SceneManager manager;
  SceneManager *sceneManager = &manager;
};
struct ResultScene {
  Context &context;
  struct Local { struct { void *returnScene = nullptr; } practiceOptions;
    std::string pacemakerTarget = "AAA"; } local;
  struct Remote { void *returnScene = nullptr; } remote;
  bool isRemote = false;
  const Local *localSource() const { return isRemote ? nullptr : &local; }
  const Remote *remoteSource() const { return isRemote ? &remote : nullptr; }
  bool persistenceDecisionRequired() const { return false; }
  void exitResult();
  void startCourseReplayStage(std::shared_ptr<CoursePlaySession>);
};
bool executeRemoteResultBack(const std::function<void()> &back) { back(); return true; }

SCENE_METHODS

int main() {
  int owner;
  Context context;
  auto session = std::make_shared<CoursePlaySession>();
  GamePlayScene stage(context, {}, {.courseSession = session, .returnScene = &owner,
                                    .pacemakerTarget = "AAA", .tableName = "Table", .tableLevel = "12"});
  assert(stage.startCourseReplayChartAtCurrentIndex());
  const auto &next = context.manager.gameplay->options;
  assert(next.returnScene == &owner && next.pacemakerTarget == "AAA");
  assert(next.tableName == "Table" && next.tableLevel == "12");
  assert(next.courseSession == session && session->currentIndex == 1);

  for (bool audioCancellation : {false, true}) {
    Context resultContext;
    ResultScene result{resultContext};
    result.local.practiceOptions.returnScene = &owner;
    session->prepared = audioCancellation;
    resultContext.jukebox.cancelled = audioCancellation;
    result.startCourseReplayStage(session);
    assert(resultContext.manager.returned == &owner && !resultContext.manager.fallback);
    assert(!resultContext.manager.gameplay);
  }
  Context watchContext;
  ResultScene result{watchContext};
  result.local.practiceOptions.returnScene = &owner;
  session->prepared = true;
  result.startCourseReplayStage(session);
  assert(watchContext.manager.gameplay->options.pacemakerTarget == "AAA");
  assert(watchContext.manager.gameplay->options.returnScene == &owner);
  for (bool explicitOwner : {false, true}) {
    Context remoteContext;
    ResultScene remote{remoteContext};
    remote.isRemote = true;
    remote.remote.returnScene = explicitOwner ? &owner : nullptr;
    remote.exitResult();
    assert(explicitOwner ? remoteContext.manager.returned == &owner : remoteContext.manager.fallback);
  }
}
