// Real event handlers; geometry, drawing and external effects are controlled.
#include <SDL3/SDL.h>
#include "input/SDLPointerEvent.h"
#include "platform/SDLMainThread.h"
#include "math/Vector3.h"
#include "music_select/MusicSelectExternalActions.h"
#include "scene/PracticeAnalyticsPresentation.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// This extracted-handler fixture has no SDL event loop. Native dispatch is
// synchronous here; cross-thread behavior is covered by the runtime tests.
bool SDLCALL SDL_IsMainThread() { return true; }
bool SDLCALL SDL_RunOnMainThread(SDL_MainThreadCallback callback, void *context, bool) {
  callback(context);
  return true;
}
const char *SDLCALL SDL_GetError() { return "fixture SDL error"; }
bool SDLCALL SDL_SetError(const char *, ...) { return false; }

#define ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS 1
namespace rendering {
constexpr int window_width = 100, window_height = 100;
constexpr int render_width = 100, render_height = 100;
constexpr float widthScale = 1, heightScale = 1;
void normalizedToUi(float x, float y, float &outX, float &outY) {
  outX = x * 100; outY = y * 100;
}
template<class X, class Y>
void screenToUi(float x, float y, X &outX, Y &outY) { outX = x; outY = y; }
void screenToUiNormalized(float x, float y, float &outX, float &outY) {
  outX = x / 100; outY = y / 100;
}
void normalizedToUiNormalized(float x, float y, float &outX, float &outY) {
  outX = x; outY = y;
}
}
SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) { *x = *y = 0; return 0; }
SDL_Keymod SDL_GetModState() { return SDL_KMOD_NONE; }
struct RenderContext {};
constexpr int YGPositionTypeAbsolute = 0;
struct View {
  virtual ~View() = default;
  int x = 0, y = 0, width = 100, height = 100;
  bool visible = true;
  virtual bool handleEvents(SDL_Event &) { return true; }
  virtual void notifyPointerEventConsumed(const SDL_Event &event) { onPointerEventConsumed(event); }
  virtual void onPointerEventConsumed(const SDL_Event &) {}
  virtual bool handleEventsImpl(SDL_Event &) { return true; }
  virtual void onLayout() {}
  virtual void onMove(int, int) {}
  virtual void onResize(int, int) {}
  virtual void renderImpl(RenderContext &) {}
  virtual void propagateThemeChange() {}
  virtual void propagateLanguageChange() {}
  void render(RenderContext &) {}
  void setBackgroundColor(int) {}
  void setCornerRadius(int) {}
  void setSize(int w, int h) { width = w; height = h; }
  void setPositionNoLayout(int px, int py, int) { x = px; y = py; }
  int getX() const { return x; }
  int getY() const { return y; }
  int getWidth() const { return width; }
  int getHeight() const { return height; }
  bool getVisible() const { return visible; }
  void setVisible(bool value) { visible = value; }
};
struct ScissorScope { ScissorScope(RenderContext &, int, int, int, int) {} };
namespace ui_theme { int textMuted() { return 0; } }
bool pointInside(const View &view, float x, float y) {
  return x >= view.x && y >= view.y && x <= view.x + view.width && y <= view.y + view.height;
}
bool isInsideView(View *view, float x, float y) { return pointInside(*view, x, y); }
bool isInsideButton(View &view, float x, float y) { return pointInside(view, x, y); }
void mouseToUi(float x, float y, float &outX, float &outY) { outX = x; outY = y; }
template<class Event, class X, class Y>
void mouseEventToUi(const Event &event, X &x, Y &y) { x = event.x; y = event.y; }
template<class Event, class X, class Y>
void mouseMotionToUi(const Event &event, X &x, Y &y) { x = event.x; y = event.y; }
template<class Event, class X, class Y>
void mouseButtonEventToUi(const Event &event, X &x, Y &y) { x = event.x; y = event.y; }
void mouseMotionEventToUi(const SDL_MouseMotionEvent &event, int &x, int &y) {
  x = event.x; y = event.y;
}
void fingerEventToUi(const SDL_TouchFingerEvent &event, float &x, float &y) {
  rendering::normalizedToUi(event.x, event.y, x, y);
}
struct UiLogicalPoint { float x = 0, y = 0; };
long long fixtureMicros = 0;
long long nowMicros() { return fixtureMicros; }
long long steadyMicros() { return fixtureMicros; }
long long unixMillis() { return fixtureMicros / 1000; }
namespace skin {
enum class MusicSelectSkinPointerTargetKind { None, Bar, Slider, Image, Text };
struct MusicSelectSkinPointerResult {
  bool consumed = true;
  std::optional<int> focusedStringWriter;
};
}
struct SelectorSession {
  skin::MusicSelectSkinPointerTargetKind kind = skin::MusicSelectSkinPointerTargetKind::Bar;
  struct Target { skin::MusicSelectSkinPointerTargetKind kind; };
  int downs = 0, drags = 0;
  Target pointerTargetAt(UiLogicalPoint) { return {kind}; }
  skin::MusicSelectSkinPointerResult queuePointerDown(UiLogicalPoint, int, long long) {
    ++downs; return {.consumed = kind != skin::MusicSelectSkinPointerTargetKind::None};
  }
  bool queuePointerDrag(UiLogicalPoint, long long) { ++drags; return true; }
};
struct MusicSelectScene {
  std::unique_ptr<SelectorSession> skinSession_ = std::make_unique<SelectorSession>();
  MusicSelectTouchGesture skinTouchGesture_;
  View *skinTextInput_ = nullptr;
  struct { struct { int skinMusicSelectScrollDurationLow = 1; } settings; } context;
  struct { void move(bool, int, long long) {} } bars_;
  struct Sound { void playScratch() {} };
  Sound *systemSound_ = nullptr;
  int actions = 0, backs = 0;
  void applySkinPointerResult(const skin::MusicSelectSkinPointerResult &, MusicSelectPointerOrigin) { ++actions; }
  void closeDirectory() { ++backs; }
  void selectedBarMoved() {}
  bool queueSkinPointerEvent(SDL_Event &);
};
struct MusicPlayerScene {
  std::vector<float> seeks;
  void seekToFraction(float fraction) { seeks.push_back(fraction); }
  bool handleProgressSeekEvents(SDL_Event &, View *, bool &, SDL_FingerID &);
};
struct GamePlayScene {
  View button;
  View *pauseButton = &button;
  bool coursePauseHoldActive = false, coursePauseHoldTouch = false;
  bool coursePauseHoldRewinding = false;
  SDL_FingerID coursePauseHoldFinger = -1;
  long long coursePauseHoldStartMicros = 0, coursePauseHoldRewindStartMicros = 0;
  float coursePauseHoldProgress = 0, coursePauseHoldRewindStartProgress = 0;
  static constexpr long long kCoursePauseHoldMicros = 1'000'000, kCoursePauseRewindMicros = 100'000;
  int completions = 0;
  bool isCoursePlayback() const { return true; }
  void showPauseMenu(bool) { ++completions; }
  bool handleCoursePauseButtonEvent(SDL_Event &);
  void beginCoursePauseHold(bool, SDL_FingerID);
  void cancelCoursePauseHold();
  void resetCoursePauseHold();
  void updateCoursePauseHoldProgress(long long);
};
struct Viewer : View {
  struct TouchPoint { float x, y; };
  std::map<SDL_FingerID, TouchPoint> activeTouches;
  bool pinchActive = false, touchGestureWasPinch = false, mouseDragging = false;
  SDL_FingerID dragTouchId = -1;
  int lastMouseX = 0, lastMouseY = 0, mouseStartX = 0, mouseStartY = 0;
  float mouseDragDistance = 0, touchStartX = 0, touchStartY = 0, touchDragDistance = 0;
  float scrollX = 0, scrollY = 0, screenZoom = 1;
  static constexpr float kCursorTapSlop = 6;
  int selections = 0, pinches = 0;
  bool containsPoint(float x, float y) { return pointInside(*this, x, y); }
  void clampScroll() {}
  void beginPinch() { pinchActive = true; ++pinches; }
  void applyPinch() {}
  void selectAtUiPoint(float, float) { ++selections; }
  bool handleEventsImpl(SDL_Event &) override;
};
struct Analytics : View {
  struct Model {
    struct Analysis { std::vector<int> sections{1, 2, 3, 4}; } analysis;
    const Analysis &displayedAnalysis() const { return analysis; }
  } model;
  PracticeAnalyticsMode mode = PracticeAnalyticsMode::Sections;
  practice_analytics_presentation::PointerCaptureState pointerCapture;
  std::size_t touchDragFirst = 0, mouseDragFirst = 0;
  std::vector<std::pair<std::size_t, std::size_t>> selections;
  std::size_t sectionForX(float x) {
    return practice_analytics_presentation::exactSectionForX(4, x, 100);
  }
  void publish(std::size_t first, std::size_t last) { selections.emplace_back(first, last); }
  bool handleEventsImpl(SDL_Event &) override;
};
enum KeySource { ScanCode };
struct IInputHandler {
  virtual ~IInputHandler() = default;
  virtual void onFingerCancel(SDL_FingerID, Vector3) = 0;
};
struct PreviewInput : IInputHandler {
  std::map<SDL_FingerID, Vector3> held;
  int cancels = 0, ups = 0;
  void onKeyDown(int, KeySource) {}
  void onKeyUp(int, KeySource) {}
  void onFingerDown(SDL_FingerID finger, Vector3 point) { held[finger] = point; }
  void onFingerMove(SDL_FingerID finger, Vector3 point) { if (held.contains(finger)) held[finger] = point; }
  void onFingerUp(SDL_FingerID finger, Vector3) { held.erase(finger); ++ups; }
private:
  void onFingerCancel(SDL_FingerID finger, Vector3) override { held.erase(finger); ++cancels; }
};
struct SettingsScene {
  std::unique_ptr<PreviewInput> previewInputHandler = std::make_unique<PreviewInput>();
  void forwardPreviewInputEvent(SDL_Event &);
};
enum class PresentationUiControlKind { Slider, LaneCover };
struct PresentationUiHit { PresentationUiControlKind kind = PresentationUiControlKind::Slider; };
struct ResultSession {
  int downs = 0, moves = 0;
  bool queuePointerDown(UiLogicalPoint, long long, PresentationUiHit *) { ++downs; return true; }
  bool queuePointerMove(PresentationUiHit, UiLogicalPoint, long long) { ++moves; return true; }
  void setPointerPosition(UiLogicalPoint) {}
};
struct EventHandleResult {};
struct ResultScene {
  std::unique_ptr<ResultSession> resultSkinSession = std::make_unique<ResultSession>();
  std::optional<PresentationUiHit> resultSkinMouseCapture;
  std::map<SDL_FingerID, PresentationUiHit> resultSkinTouchCaptures;
  long long resultSkinStartedMicros = 0;
  std::optional<long long> resultSkinFadeoutStartedMillis;
  std::optional<UiLogicalPoint> resultSkinPointerUiPosition;
  View *courseDetailsModalRoot = nullptr, *viewportLayout = nullptr;
  int actions = 0;
  void consumeResultSkinBuiltinEvents() { ++actions; }
  bool queueResultSkinPointerEvent(SDL_Event &);
  EventHandleResult handleEvents(SDL_Event &);
};

RANKING_CLASS
HANDLER_METHODS

int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
SDL_Event finger(Uint32 type, SDL_FingerID id = 42, float x = .25F, float y = .5F) {
  SDL_Event event{};
  event.type = type;
  event.tfinger.touchID = 1;
  event.tfinger.fingerID = id;
  event.tfinger.x = x; event.tfinger.y = y;
  return event;
}
void testSelector() {
  for (auto target : {skin::MusicSelectSkinPointerTargetKind::Bar,
                     skin::MusicSelectSkinPointerTargetKind::None,
                     skin::MusicSelectSkinPointerTargetKind::Slider}) {
    MusicSelectScene scene;
    scene.skinSession_->kind = target;
    auto down = finger(SDL_EVENT_FINGER_DOWN);
    expect(scene.queueSkinPointerEvent(down), "selector begins capture");
    auto unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
    (void)scene.queueSkinPointerEvent(unrelated);
    auto otherDown = finger(SDL_EVENT_FINGER_DOWN, 99);
    if (target != skin::MusicSelectSkinPointerTargetKind::Slider)
      expect(!scene.queueSkinPointerEvent(otherDown), "selector unrelated cancel keeps capture");
    // A right swipe would navigate back on an ordinary release.
    auto motion = finger(SDL_EVENT_FINGER_MOTION, 42, .6F);
    (void)scene.queueSkinPointerEvent(motion);
    const int beforeActions = scene.actions, beforeDowns = scene.skinSession_->downs;
    auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
    expect(scene.queueSkinPointerEvent(cancel), "selector consumes matching cancellation");
    expect(scene.actions == beforeActions && scene.skinSession_->downs == beforeDowns && scene.backs == 0,
           "selector cancel cannot select a chart or navigate");
    expect(scene.queueSkinPointerEvent(otherDown), "selector accepts new gesture after cancel");
  }
  MusicSelectScene tap;
  auto down = finger(SDL_EVENT_FINGER_DOWN), cancel = finger(SDL_EVENT_FINGER_CANCELED);
  (void)tap.queueSkinPointerEvent(down); (void)tap.queueSkinPointerEvent(cancel);
  expect(tap.actions == 0 && tap.skinSession_->downs == 0, "canceled bar tap cannot select");
}
void testSeek() {
  MusicPlayerScene scene;
  View track;
  bool mouseDown = false;
  SDL_FingerID active = -1;
  auto down = finger(SDL_EVENT_FINGER_DOWN);
  expect(scene.handleProgressSeekEvents(down, &track, mouseDown, active), "seek begins");
  auto unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99, .9F);
  expect(!scene.handleProgressSeekEvents(unrelated, &track, mouseDown, active) && active == 42,
         "unrelated cancellation preserves seek owner");
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
  expect(scene.handleProgressSeekEvents(cancel, &track, mouseDown, active) && active == -1,
         "seek cancellation retires capture");
  expect(scene.seeks == std::vector<float>{.25F}, "seek cancel ignores final coordinate");
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99, .75F);
  expect(scene.handleProgressSeekEvents(fresh, &track, mouseDown, active) && active == 99,
         "seek accepts fresh gesture");
}
void testPauseHold() {
  fixtureMicros = 0;
  GamePlayScene scene;
  auto down = finger(SDL_EVENT_FINGER_DOWN);
  (void)scene.handleCoursePauseButtonEvent(down);
  auto unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
  expect(!scene.handleCoursePauseButtonEvent(unrelated) && scene.coursePauseHoldActive,
         "unrelated cancellation keeps course hold");
  fixtureMicros = 2'000'000; // Already beyond the hold threshold at event dispatch.
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED);
  expect(scene.handleCoursePauseButtonEvent(cancel) && !scene.coursePauseHoldActive,
         "course cancellation retires hold");
  scene.updateCoursePauseHoldProgress(3'000'000);
  expect(scene.completions == 0, "course cancel cannot complete hold even at threshold");
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99);
  (void)scene.handleCoursePauseButtonEvent(fresh);
  expect(scene.coursePauseHoldActive && scene.coursePauseHoldFinger == 99,
         "course hold accepts new finger");
}
void testViewer() {
  Viewer view;
  auto down = finger(SDL_EVENT_FINGER_DOWN), unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
  (void)view.handleEventsImpl(down); (void)view.handleEventsImpl(unrelated);
  expect(view.activeTouches.size() == 1 && view.activeTouches.contains(42), "viewer ignores unrelated cancel");
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
  expect(!view.handleEventsImpl(cancel) && view.activeTouches.empty() && !view.pinchActive,
         "viewer removes matching canceled finger");
  expect(view.selections == 0, "viewer cancel cannot move practice marker");
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99);
  (void)view.handleEventsImpl(fresh);
  expect(view.dragTouchId == 99 && !view.pinchActive && view.pinches == 0, "fresh viewer touch has no phantom pinch");
  auto second = finger(SDL_EVENT_FINGER_DOWN, 100);
  (void)view.handleEventsImpl(second);
  auto pinchCancel = finger(SDL_EVENT_FINGER_CANCELED, 100);
  (void)view.handleEventsImpl(pinchCancel);
  expect(view.activeTouches.size() == 1 && view.activeTouches.contains(99) && !view.pinchActive,
         "pinch cancellation preserves remaining finger");
  auto up = finger(SDL_EVENT_FINGER_UP, 99);
  (void)view.handleEventsImpl(up);
  expect(view.selections == 0, "remaining pinch finger cannot become an actionable tap");
}
void testAnalytics() {
  Analytics view;
  auto down = finger(SDL_EVENT_FINGER_DOWN), unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
  (void)view.handleEventsImpl(down); (void)view.handleEventsImpl(unrelated);
  expect(view.pointerCapture.touchActive(), "analytics unrelated cancel keeps capture");
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
  expect(!view.handleEventsImpl(cancel) && !view.pointerCapture.touchActive(), "analytics matching cancel ends capture");
  expect(view.selections == std::vector<std::pair<std::size_t, std::size_t>>{{1, 1}},
         "analytics cancellation cannot change selected range");
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99, .75F);
  (void)view.handleEventsImpl(fresh);
  expect(view.selections.size() == 2 && view.selections.back() == std::pair<std::size_t, std::size_t>{3, 3},
         "analytics accepts fresh gesture");
}
void testPreview() {
  SettingsScene scene;
  auto down = finger(SDL_EVENT_FINGER_DOWN), unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
  scene.forwardPreviewInputEvent(down); scene.forwardPreviewInputEvent(unrelated);
  expect(scene.previewInputHandler->held.contains(42), "preview unrelated cancellation keeps pressed lane");
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED);
  scene.forwardPreviewInputEvent(cancel);
  expect(scene.previewInputHandler->held.empty() && scene.previewInputHandler->cancels == 2 &&
             scene.previewInputHandler->ups == 0, "preview forwards cancellation phase without actionable release");
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99);
  scene.forwardPreviewInputEvent(fresh);
  expect(scene.previewInputHandler->held.contains(99), "preview accepts fresh gesture");
}
void testResult() {
  struct Overlay : View { bool handleEvents(SDL_Event &) override { return false; } } overlay;
  ResultScene scene;
  auto down = finger(SDL_EVENT_FINGER_DOWN), unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
  scene.handleEvents(down); scene.handleEvents(unrelated);
  expect(scene.resultSkinTouchCaptures.contains(42), "result ignores unrelated cancellation");
  // A covering overlay must not intercept continuation cleanup for an owned touch.
  scene.viewportLayout = &overlay;
  auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
  scene.handleEvents(cancel);
  expect(scene.resultSkinTouchCaptures.empty(), "result cancellation is routed before overlay");
  expect(scene.actions == 1 && scene.resultSkinSession->moves == 0, "result cancel has no skin action");
  scene.viewportLayout = nullptr;
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 99);
  scene.handleEvents(fresh);
  expect(scene.resultSkinTouchCaptures.size() == 1 && scene.resultSkinTouchCaptures.contains(99),
         "result fresh gesture has no stale capture");
}
void testRanking() {
  struct Table : View {
    std::optional<SDL_FingerID> active;
    int selections = 0;
    bool handleEvents(SDL_Event &event) override {
      if (event.type == SDL_EVENT_FINGER_DOWN) active = event.tfinger.fingerID;
      if (event.type == SDL_EVENT_FINGER_UP && active == event.tfinger.fingerID) { ++selections; active.reset(); }
      if (event.type == SDL_EVENT_FINGER_CANCELED && active == event.tfinger.fingerID) active.reset();
      return false;
    }
    void onPointerEventConsumed(const SDL_Event &event) override {
      if ((event.type == SDL_EVENT_FINGER_CANCELED || event.type == SDL_EVENT_FINGER_UP) &&
          active == event.tfinger.fingerID) active.reset();
    }
  };
  // Expose the production overrides through View's public virtual interface.
  for (bool consumed : {false, true}) {
    ir::RankingTableViewport viewport;
    View &route = viewport;
    auto *table = new Table;
    viewport.setContentView(table);
    auto down = finger(SDL_EVENT_FINGER_DOWN), unrelated = finger(SDL_EVENT_FINGER_CANCELED, 99);
    (void)route.handleEventsImpl(down);
    if (consumed) route.onPointerEventConsumed(unrelated); else (void)route.handleEventsImpl(unrelated);
    auto otherDown = finger(SDL_EVENT_FINGER_DOWN, 99);
    expect(route.handleEventsImpl(otherDown), "ranking unrelated cancellation preserves owner");
    auto cancel = finger(SDL_EVENT_FINGER_CANCELED, 42, .9F);
    if (consumed) route.onPointerEventConsumed(cancel); else (void)route.handleEventsImpl(cancel);
    expect(!table->active && table->selections == 0, "ranking cancel cleans child without selecting");
    expect(!route.handleEventsImpl(otherDown) && table->active == 99, "ranking accepts new gesture after cancel");
    auto up = finger(SDL_EVENT_FINGER_UP, 99);
    (void)route.handleEventsImpl(up);
    expect(table->selections == 1, "ranking fresh tap remains actionable");
  }
}
int main() {
  testSelector(); testSeek(); testPauseHold(); testViewer(); testAnalytics();
  testPreview(); testResult(); testRanking();
  return failures ? 1 : 0;
}
