#include "streak/StreakModel.h"
#include "streak/StreakResultPresentation.h"
#include <cassert>
#include <atomic>
#include "input/InputTypes.h"
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
static std::uint64_t ticks = 1000;
std::uint64_t SDL_GetTicks() { return ticks; }
long long nowMicros() { return ticks * 1000; }
namespace ui { void invalidate() {} }
enum { SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP, SDL_EVENT_FINGER_DOWN,
       SDL_EVENT_FINGER_UP, SDL_EVENT_FINGER_CANCELED, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP };
constexpr int SDLK_ESCAPE = 27, SDL_TOUCH_MOUSEID = -1, SDL_MOUSE_TOUCHID = -1;
struct SDL_Event {
  int type;
  struct { int key = 0; } key;
  struct { float x = 0, y = 0; int which = 0; } button;
  struct { float x = 0, y = 0; int touchID = 0; } tfinger;
};
struct UiLogicalPoint { float x = 0, y = 0; };
namespace platform_open { bool openExternalUrl(std::string_view, std::string &) { return true; } }
namespace rendering {
constexpr int render_width=1000, render_height=1000;
constexpr float widthScale = 1, heightScale = 1;
void screenToUi(float x, float y, float &ox, float &oy) { ox=x; oy=y; }
void normalizedToUi(float x, float y, float &ox, float &oy) { ox=x*1000; oy=y*1000; }
}
struct View {
  int cancelled = 0, events = 0;
  bool visible = true;
  std::function<void()> release;
  float getX() const { return 500; }
  float getY() const { return 0; }
  float getWidth() const { return 400; }
  float getHeight() const { return 700; }
  void cancelPointerInput() { ++cancelled; }
  void setVisible(bool value) { visible=value; }
  void handleEvents(SDL_Event &e) { ++events; if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && release) release(); }
};
struct Attempt {
  struct {
    std::string attemptId = "test";
    struct { struct { bool autoPlay = false; } provenance; } score;
    std::int64_t playedAtUnixMillis = std::time(nullptr) * 1000;
  } result;
};
struct Outcome { bool saved() const { return saved_; } bool saved_ = false; };
struct Local {
  bool autoPlayResult=false, replayResult=false;
  struct { bool enabled=false; } practiceOptions;
  struct { bool savedResultBrowsing=false; } courseOptions;
  struct {
    std::shared_ptr<Attempt> chartAttempt = std::make_shared<Attempt>();
    std::optional<Outcome> chartOutcome = Outcome{};
  } persistenceOptions;
};
struct ResultScene {
  struct {
    std::uint64_t currentFrame = 11;
    std::atomic_bool replayVideoExportActive{false};
    struct { bool exportRequested() const { return false; } } rendererAccess;
    struct {
      input::LegacyInputGeneration legacyInputGeneration(int w, int h) const {
        input::LegacyInputGeneration g;
        g.drawableWidth=w; g.drawableHeight=h; g.sequence=17;
        g.anyKeyPressed=true; g.justTouched=true; g.pressedGdxKeys.set(29); g.justPressedGdxKeys.set(29);
        g.pressedMouseButtons.set(0); g.controllerCount=1;
        return g;
      }
      bool hasCurrentLegacyPressEdge() const { return true; }
    } inputDeviceRegistry;
    struct {
      int loads=0;
      streak::State state;
      std::optional<streak::PlayProgress> TakeStreakProgress(std::string_view) { return {}; }
      std::optional<streak::State> LoadStreak(std::string &) { ++loads; return state; }
    } scoreRepository;
  } context;
  Local local;
  Local *localSource() { return &local; }
  bool isCourseStageResult() const { return false; }
  bool isCourseFinalResult() const { return false; }
  streak::ResultPresentation streakPresentation_;
  std::optional<streak::State> streakSnapshot_;
  std::optional<streak::PlayProgress> streakProgress_;
  View viewport, panel, overlay;
  View *viewportLayout=&viewport, *streakPanel_=nullptr, *streakOverlay_=nullptr;
  std::optional<int> resultSkinMouseCapture = 1;
  std::unordered_map<int,int> resultSkinTouchCaptures{{1,1}};
  std::optional<long long> resultSkinFadeoutStartedMillis = 900;
  long long resultSkinStartedMicros = 0, streakSkinResumeMillis_ = 0;
  bool streakDismissGesture_ = false;
  std::optional<std::uint64_t> streakInputBlockedThroughFrame_;
  bool streakOwnsInput() const;
  auto inputCallback() { return CAPTURE_INPUT; }
  auto browserCallback() { return OPEN_BROWSER; }
  void prepareStreakProgress();
  void dismissStreakProgress(bool swallowRelease=false);
  bool handleStreakProgressEvent(SDL_Event &);
};
PRODUCTION_METHODS
int main() {
  ResultScene scene;
  streak::Day day;
  day.assignments.push_back({});
  scene.context.scoreRepository.state.days.emplace(streak::today(), day);
  scene.prepareStreakProgress();
  assert(scene.context.scoreRepository.loads == 0 && !scene.streakPresentation_.active());
  scene.local.persistenceOptions.chartOutcome->saved_ = true; // Explicit persistence retry.
  scene.prepareStreakProgress();
  assert(scene.context.scoreRepository.loads == 1 && scene.streakPresentation_.active());
  const auto blocked = scene.inputCallback()();
  assert(!blocked.anyKeyPressed && !blocked.justTouched && blocked.pressedGdxKeys.none() &&
         blocked.justPressedGdxKeys.none() && blocked.pressedMouseButtons.none() && blocked.controllerCount == 0);
  assert(blocked.drawableWidth == 1000 && blocked.sequence == 17);
  assert(!scene.browserCallback()("https://example.com"));
  assert(scene.viewport.cancelled == 1 && !scene.resultSkinMouseCapture &&
         scene.resultSkinTouchCaptures.empty() && !scene.resultSkinFadeoutStartedMillis);
  scene.prepareStreakProgress();
  assert(scene.context.scoreRepository.loads == 1);
  SDL_Event released{.type=SDL_EVENT_MOUSE_BUTTON_UP};
  assert(scene.handleStreakProgressEvent(released)); // Held pointer cannot reach the skin.
  ticks += 500;
  assert(scene.streakPresentation_.open(ticks));
  scene.streakPanel_=&scene.panel; scene.streakOverlay_=&scene.overlay;
  SDL_Event outside{.type=SDL_EVENT_MOUSE_BUTTON_DOWN};
  assert(scene.handleStreakProgressEvent(outside));
  assert(!scene.streakPresentation_.active() && !scene.overlay.visible);
  assert(scene.handleStreakProgressEvent(released)); // Full dismiss gesture is swallowed.
  assert(!scene.handleStreakProgressEvent(released));
  scene.prepareStreakProgress(); // Rebuild/retry does not repeat the presentation.
  assert(!scene.streakPresentation_.active() && scene.context.scoreRepository.loads == 1);

  ResultScene button;
  assert(button.streakPresentation_.arm({.saved=true, .manual=true, .currentDay=true, .activeRoutine=true}, ticks));
  ticks += 500; button.streakPresentation_.open(ticks);
  button.streakPanel_=&button.panel; button.streakOverlay_=&button.overlay;
  button.overlay.release=[&] { button.dismissStreakProgress(); };
  SDL_Event inside{.type=SDL_EVENT_MOUSE_BUTTON_DOWN}; inside.button.x=600;
  assert(button.handleStreakProgressEvent(inside));
  assert(button.streakPresentation_.active() && button.overlay.events == 1);
  assert(button.handleStreakProgressEvent(released));
  assert(!button.streakPresentation_.active() && !button.streakDismissGesture_);
  assert(!button.inputCallback()().justTouched && !button.browserCallback()("https://example.com"));
  ++button.context.currentFrame;
  assert(button.inputCallback()().justTouched && button.browserCallback()("https://example.com"));
  assert(button.streakSkinResumeMillis_ == static_cast<long long>(ticks));

  ResultScene escape;
  escape.streakPresentation_.arm({.saved=true, .manual=true, .currentDay=true, .activeRoutine=true}, ticks);
  SDL_Event key{.type=SDL_EVENT_KEY_DOWN}; key.key.key=SDLK_ESCAPE;
  assert(escape.handleStreakProgressEvent(key));
  key.type=SDL_EVENT_KEY_UP;
  assert(escape.handleStreakProgressEvent(key));
  assert(!escape.handleStreakProgressEvent(key));
}
