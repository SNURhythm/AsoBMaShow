// Embed complete production Scene/SceneManager and ResultScene::renderScene.
// Only graphics, application services, and the destination actions are faked.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::abort();
  }
}

namespace i18n {
std::uint64_t revision() { return 0; }
}
using Uint64 = std::uint64_t;
static long long clockMicros = 1'000'000;
Uint64 SDL_GetTicks64() { return static_cast<Uint64>(clockMicros / 1000); }
long long nowMicros() { return clockMicros; }
struct SDL_Event {};
struct UiLogicalPoint { float x = 0, y = 0; };
namespace rendering {
constexpr int window_width = 1280, window_height = 720;
constexpr float widthScale = 1, heightScale = 1;
void normalizedToUi(float x, float y, float &outX, float &outY) {
  outX = x * window_width;
  outY = y * window_height;
}
void screenToUi(float x, float y, float &outX, float &outY) {
  outX = x;
  outY = y;
}
}
struct FakeRenderer { int scopes = 0; };
struct RenderContext {
  explicit RenderContext(FakeRenderer &renderer) : renderer(renderer) {}
  FakeRenderer &renderer;
  struct UiBatchScope {
    explicit UiBatchScope(RenderContext &context) : context(context) {
      ++context.renderer.scopes;
    }
    ~UiBatchScope() { --context.renderer.scopes; }
    RenderContext &context;
  };
};
struct View {
  bool visible = false;
  int *renderCount = nullptr;
  virtual ~View() = default;
  bool handleEvents(SDL_Event &) { return true; }
  void propagateLanguageChange() {}
  void render(RenderContext &) { if (renderCount) ++*renderCount; }
  bool getVisible() const { return visible; }
  void setSize(int, int) {}
  View *findViewByName(const char *) { return nullptr; }
  static void dispatchTemporaryEventListeners(SDL_Event &) {}
  static void dispatchDeferredEventCallbacks() {}
};
class SceneManager;
struct BackgroundTasks { void setGameplayPaused(bool) {} };
struct InputDeviceRegistry {
  struct Pointer { float x = 0, y = 0; bool normalized = false; };
  std::optional<Pointer> pointerPosition() const { return std::nullopt; }
};
struct ApplicationContext {
  Uint64 currentFrame = 0;
  FakeRenderer uiBatchRenderer;
  SceneManager *sceneManager = nullptr;
  std::atomic_bool backgroundTasksPausedForForegroundScene = false;
  BackgroundTasks *chartLibraryTasks = nullptr;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;
  int gameplayBgaCompositeState = 0;
  InputDeviceRegistry inputDeviceRegistry;
};

PRODUCTION_SCENE_HEADER
PRODUCTION_MANAGER_HEADER
PRODUCTION_MANAGER_METHODS

struct Observations {
  bool rendering = false;
  int cleanups = 0;
  int destructions = 0;
  int exits = 0;
  int continuations = 0;
  int destinationInitializations = 0;
  int skinRenders = 0;
  int overlayRenders = 0;
};
struct DestinationScene final : Scene {
  DestinationScene(ApplicationContext &context, Observations &observations)
      : Scene(context), observations(observations) {}
  Observations &observations;
  void init() override { ++observations.destinationInitializations; }
  void update(float) override {}
  void renderScene() override {}
  void cleanupScene() override {}
};
struct ResultSkinData {};
struct FakeResultSkin {
  Observations &observations;
  int sceneMillis() const { return 100; }
  int fadeoutMillis() const { return 20; }
  void setPointerPosition(UiLogicalPoint) {}
  bool requiresRuntimeStringRefresh(const ResultSkinData &) const { return false; }
  bool refreshRuntimeStrings(const ResultSkinData &) { return true; }
  bool render(RenderContext &, const ResultSkinData &, std::uint64_t, long long) {
    ++observations.skinRenders;
    return true;
  }
};
struct CourseSession { bool courseReplayPlayback = false; };
struct LocalResult {
  struct { std::shared_ptr<CourseSession> session; } courseOptions;
};
struct ResultScene final : Scene {
  ResultScene(ApplicationContext &context, Observations &observations, bool course)
      : Scene(context), observations(observations), course(course),
        resultSkinSession(std::make_unique<FakeResultSkin>(observations)) {
    local.courseOptions.session = std::make_shared<CourseSession>();
  }
  ~ResultScene() override { ++observations.destructions; }
  Observations &observations;
  bool course;
  bool persistenceBlocked = false;
  LocalResult local;
  std::unique_ptr<FakeResultSkin> resultSkinSession;
  std::optional<UiLogicalPoint> resultSkinPointerUiPosition;
  long long resultSkinStartedMicros = clockMicros;
  std::optional<long long> resultSkinFadeoutStartedMillis;
  std::uint64_t resultSkinFrameSerial = 0;
  View *persistenceDetailsModalRoot = nullptr;
  View *courseDetailsModalRoot = nullptr;
  View *rankingOverlayPortal = nullptr;

  void init() override {
    courseDetailsModalRoot = new View;
    courseDetailsModalRoot->renderCount = &observations.overlayRenders;
    addView(courseDetailsModalRoot);
  }
  void update(float) override {}
  void renderScene() override;
  bool renderViewBeforeScene(const View *) const override { return false; }
  void cleanupScene() override {
    require(!observations.rendering,
            "result timeout must not clean up its scene inside manager.render()");
    require(context.uiBatchRenderer.scopes == 0,
            "result timeout must wait for every render batch scope to unwind");
    ++observations.cleanups;
    resultSkinSession.reset();
  }
  LocalResult *localSource() { return &local; }
  bool isCourseStageResult() const { return course; }
  bool persistenceDecisionRequired() const { return persistenceBlocked; }
  ResultSkinData makeResultSkinData() { return {}; }
  void appendResultSkinRenderDiagnostics() {}
  void handleResultSkinRenderFailure() { require(false, "unexpected skin failure"); }
  void consumeResultSkinBuiltinEvents() {}
  void exitResult() {
    ++observations.exits;
    context.sceneManager->changeScene("destination");
  }
  void continueCourse() {
    ++observations.continuations;
    context.sceneManager->changeScene("destination");
  }
};

PRODUCTION_RESULT_RENDER

struct Fixture {
  Observations observations;
  ApplicationContext context;
  SceneManager manager{context};
  ResultScene *result;
  long long started = clockMicros;

  explicit Fixture(bool course = false) {
    manager.registerScene("destination",
        std::make_unique<DestinationScene>(context, observations));
    auto scene = std::make_unique<ResultScene>(context, observations, course);
    result = scene.get();
    manager.changeScene(std::move(scene));
  }
  void renderAt(long long elapsedMillis) {
    clockMicros = started + elapsedMillis * 1000;
    observations.rendering = true;
    manager.render();
    observations.rendering = false;
    require(context.uiBatchRenderer.scopes == 0, "render batches must balance");
  }
  void nextFrame() {
    ++context.currentFrame;
    manager.handleDeferred();
  }
  void requireStillResult() {
    require(manager.currentScene == result && observations.cleanups == 0,
            "result must stay alive until deferred transition is due");
  }
  void requireTransition(bool course) {
    require(manager.currentScene != result, "deferred timeout must navigate");
    require(observations.cleanups == 1 && observations.destructions == 1,
            "timeout must release its dynamic result exactly once");
    require(observations.exits == (course ? 0 : 1) &&
                observations.continuations == (course ? 1 : 0),
            "ordinary timeout exits; course timeout continues its course");
    require(observations.destinationInitializations == 1,
            "timeout must initialize its destination exactly once");
  }
};

void testTimeout(bool course) {
  Fixture fixture(course);
  fixture.renderAt(99);
  fixture.renderAt(100);
  require(!fixture.result->resultSkinFadeoutStartedMillis,
          "scene duration equality must not begin fadeout");
  fixture.nextFrame();
  fixture.requireStillResult();
  fixture.renderAt(101);
  require(fixture.result->resultSkinFadeoutStartedMillis == 101,
          "first frame beyond scene duration must begin fadeout");
  fixture.renderAt(120);
  fixture.renderAt(121);
  fixture.nextFrame();
  fixture.requireStillResult();
  const int previousOverlays = fixture.observations.overlayRenders;
  fixture.renderAt(122);
  fixture.requireStillResult();
  require(fixture.observations.overlayRenders == previousOverlays + 1,
          "result overlays must remain alive for the post-scene render pass");
  fixture.manager.handleDeferred();
  fixture.requireStillResult();
  // More than one render before deferred dispatch must not duplicate navigation.
  fixture.renderAt(123);
  fixture.nextFrame();
  fixture.requireTransition(course);
  fixture.nextFrame();
  fixture.renderAt(200);
  fixture.requireTransition(course);
}

void testBlockedFadeout(bool courseDetails) {
  Fixture fixture;
  fixture.renderAt(101);
  if (courseDetails) fixture.result->courseDetailsModalRoot->visible = true;
  else fixture.result->persistenceBlocked = true;
  fixture.renderAt(500);
  fixture.nextFrame();
  fixture.requireStillResult();
  require(!fixture.result->resultSkinFadeoutStartedMillis,
          "persistence and course details must cancel an in-progress fadeout");
  fixture.result->courseDetailsModalRoot->visible = false;
  fixture.result->persistenceBlocked = false;
  fixture.renderAt(501);
  fixture.renderAt(521);
  fixture.nextFrame();
  fixture.requireStillResult();
  fixture.renderAt(522);
  fixture.requireStillResult();
  fixture.nextFrame();
  fixture.requireTransition(false);
}

void testCourseReplayOwnsTransition() {
  Fixture fixture(true);
  fixture.result->local.courseOptions.session->courseReplayPlayback = true;
  fixture.renderAt(101);
  fixture.renderAt(10'000);
  fixture.nextFrame();
  fixture.requireStillResult();
  require(!fixture.result->resultSkinFadeoutStartedMillis,
          "course replay rest scheduling owns transitions instead of skin timeout");
  require(fixture.observations.skinRenders == 2,
          "course replay guard must keep rendering the result skin");
}

int main() {
  testTimeout(false);
  testTimeout(true);
  testBlockedFadeout(false);
  testBlockedFadeout(true);
  testCourseReplayOwnsTransition();
}
