#include "scene/play/RealtimeTouchInputRouter.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using SDL_FingerID = long long;
using Uint32 = unsigned;
Uint32 SDL_GetTicks() { return 0; }
template <class... Args> void SDL_Log(Args...) {}
struct Vector3 { float x, y, z; };
namespace bx { using Vec3 = Vector3; }
namespace bms_parser {
struct Note { bool IsLongNote() const { return false; } };
struct LongNote : Note { bool IsHolding = false; };
}
enum class ReplayTouchAction { Down, Move, Up, Cancel };
struct FlickState {
  float startX, startY;
  Uint32 startTime;
  bool active;
  int lastFlickDirection;
  bms_parser::LongNote *activeLongNote;
};
bool hasActiveLongNote(FlickState &state) {
  return state.activeLongNote && state.activeLongNote->IsHolding;
}
namespace rendering {
int render_width = 1000, render_height = 500;
int window_width = 800, window_height = 400;
float ui_scale_x = 1.0F, ui_scale_y = 1.0F;
int ui_offset_x = 100, ui_offset_y = 50;
struct Camera {
  float getNearClip() const { return 0; }
  float getFarClip() const { return 1; }
  bx::Vec3 deproject(float x, float y, float z) const {
    return {x / render_width * 8, y / render_height, z};
  }
} game_camera;
}

// Only the platform/camera and logical-input sink are substitutes. Finger
// ownership, drag, scratch, hit testing and layout invalidation are extracted
// unchanged from production below.
class RhythmInputHandler {
public:
  struct TouchSource { void discardPendingEvents() {} };
  TouchSource *touchInputSource = nullptr;
  bool applicationBackground = false;
  std::vector<int> presses, releases;
  std::vector<std::uint64_t> laneTimestamps;
  struct Pipeline {
    RhythmInputHandler *owner;
    int resets = 0;
    void reset() { ++resets; }
    struct Scope { int player, keyMode; };
    bms_parser::Note *consumePhysicalTouchLane(Scope, int lane, bool pressed,
                                               std::optional<int>, std::uint64_t timestamp) {
      (pressed ? owner->presses : owner->releases).push_back(lane);
      owner->laneTimestamps.push_back(timestamp);
      return nullptr;
    }
  } pipeline{this};
  int keyMode = 7;
  Pipeline *logicalInputPipeline = &pipeline;
  int totalLaneCount = 8, scratchLaneCount = 1;
  float playAreaWidth = 8, playAreaLeftX = 0;
  bool dragModeEnabled = false;
  input::PlayfieldTouchConfig touchConfig;
  std::vector<int> laneOrder{0, 1, 2, 3, 4, 5, 6, 7};
  std::optional<gameplay::RealtimeTouchLayout> touchLaneLayout;
  std::map<SDL_FingerID, int> fingerToLane;
  std::map<SDL_FingerID, bool> fingerLanePressed;
  std::map<SDL_FingerID, FlickState> flickStates;
  std::map<SDL_FingerID, Uint32> cancelGraceExpiry;
  std::map<SDL_FingerID, Vector3> activeTouchPoints;
  std::function<bool(SDL_FingerID, ReplayTouchAction, Vector3, std::uint64_t)> touchEventCallback;
  std::function<std::optional<bool>(int)> longNoteHeldCallback;
  bms_parser::Note *applyTouchLane(int lane, bool pressed, std::optional<int>);
  std::uint64_t ingressTimestamp = 0;
  std::uint64_t touchEventTimestampMicros() const { return ingressTimestamp; }
  bool notifyTouchEvent(SDL_FingerID, ReplayTouchAction, Vector3);
  void discardPendingTouchEvents();
  void cancelInputState();
  void setApplicationBackground(bool background);
  Vector3 normalizedTouchToRenderLocation(Vector3) const;
  bool isLaneOccupied(int, SDL_FingerID) const;
  void beginFingerLane(SDL_FingerID, int, Vector3);
  void releaseFingerLane(SDL_FingerID);
  void handleScratchMove(SDL_FingerID, Vector3);
  void onFingerDown(SDL_FingerID, Vector3);
  void onFingerUp(SDL_FingerID, Vector3);
  void onFingerMove(SDL_FingerID, Vector3);
  int clampLane(int) const;
  bool isScratchLane(int) const;
  int touchToLaneIndex(Vector3) const;
  std::optional<int> playfieldTouchLane(Vector3, bool) const;
  std::optional<int> touchToLaneIfInside(Vector3) const;
  int touchToLane(Vector3);
  void setTouchLaneLayout(std::optional<gameplay::RealtimeTouchLayout>);
  std::optional<int> authoredTouchLane(Vector3, bool) const;
};

struct TouchVisualState {
  std::map<long long, Vector3> active;
  void setLiveTouchPoint(long long finger, ReplayTouchAction action,
                         float x, float y, long long) {
    if (action == ReplayTouchAction::Cancel || action == ReplayTouchAction::Up) {
      active.erase(finger);
    } else {
      active[finger] = {x, y, 0};
    }
  }
};

bool fixtureAndroid = true;
enum class PresentationMode { BuiltIn, Skin };
struct FixturePresentation {
  PresentationMode mode = PresentationMode::Skin;
  gameplay::RealtimeTouchLayout layout;
  PresentationMode activeMode() const { return mode; }
  const gameplay::RealtimeTouchLayout &touchLayout() const { return layout; }
};

class GamePlayScene {
public:
  RhythmInputHandler *inputHandler = nullptr;
  FixturePresentation *presentation = nullptr;
  bool realtimeAuthority = false;
  bool realtimeGameplayAuthorityActive() const { return realtimeAuthority; }
  void refreshLegacyTouchLayout();
  struct State { bool isPlaying = true, isEnding = false; } ownedState;
  State *state = &ownedState;
  struct Context {
    struct Jukebox {
      long long getTimeMicros() const { return 900000; }
      Jukebox &audioRuntime() { return *this; }
      std::optional<long long> songTimeMicrosAtSteadyMicros(std::uint64_t time) {
        return static_cast<long long>(time) - 100000;
      }
      bool paused = false;
      bool isPaused() const { return paused; }
    } jukebox;
  } context;
  TouchVisualState visualState;
  TouchVisualState *playfieldVisualStateStore = &visualState;
  bool practiceAllowed = true;
  bool floatingLaneCoverDragActive = false;
  bool floatingLaneCoverDragChanged = false;
  SDL_FingerID floatingLaneCoverFinger = -1;
  float floatingLaneCoverDragOffsetY = 0;
  int persisted = 0;
  bool practiceInputAllowed(long long) { return practiceAllowed; }
  void persistFloatingLaneCoverSettings() { ++persisted; }
  std::vector<long long> replayTimes;
  long long getGameplayTimeMicros(long long time) { return time - 10000; }
  void appendReplayTouchSample(SDL_FingerID, ReplayTouchAction, Vector3, long long time) {
    replayTimes.push_back(time);
  }
  bool handleTouchInput(SDL_FingerID, ReplayTouchAction, Vector3, std::uint64_t);
  bool handleFloatingLaneCoverInput(SDL_FingerID, ReplayTouchAction, Vector3, long long) {
    return false;
  }
  bool handleTouchInputAtGameplayTime(SDL_FingerID, ReplayTouchAction,
                                      Vector3, long long, bool = true);
  void cancelLegacyFloatingLaneCoverTouch();
};

// PRODUCTION_METHODS

int failures = 0;
void expect(bool pass, const char *message) {
  if (!pass) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

gameplay::RealtimeTouchLayout skinLayout() {
  gameplay::RealtimeTouchLayout layout;
  layout.revision = 17;
  layout.laneRegions = {
      {.bottomLeft = {.20F, .8F}, .bottomRight = {.30F, .8F},
       .topLeft = {.20F, .2F}, .topRight = {.30F, .2F}, .lane = 3},
      {.bottomLeft = {.35F, .8F}, .bottomRight = {.60F, .8F},
       .topLeft = {.35F, .2F}, .topRight = {.60F, .2F}, .lane = 1},
      {.bottomLeft = {.70F, .8F}, .bottomRight = {.80F, .8F},
       .topLeft = {.70F, .2F}, .topRight = {.80F, .2F}, .lane = 7,
       .scratch = true}};
  return layout;
}

int main() {
  // SDL already removed the UI offset: (.25 * 1000 - 100) / 800.
  const Vector3 firstLane{.1875F, .5F, 0};
  const Vector3 secondLane{.4375F, .5F, 0};
  const Vector3 gap{.28125F, .5F, 0};
  for (const bool authored : {false, true}) {
    for (const auto side : {input::SideTapMode::EdgeLane, input::SideTapMode::Scratch,
                            input::SideTapMode::Ignore}) {
      RhythmInputHandler taps;
      taps.touchConfig = {.tapToScratch = true, .sideTapMode = side};
      if (authored) taps.setTouchLaneLayout(skinLayout());
      taps.onFingerDown(90, {-.3F, .5F, 0});
      expect(side == input::SideTapMode::Scratch ? taps.presses == std::vector<int>{7}
                 : side == input::SideTapMode::Ignore || authored ? taps.presses.empty()
                 : taps.presses == std::vector<int>{0},
             "legacy side taps follow each policy for built-in and authored layouts");
      taps.discardPendingTouchEvents();
      expect(taps.releases == taps.presses, "legacy side scratch is released on cancellation");
    }
    RhythmInputHandler taps;
    taps.touchConfig.tapToScratch = true;
    if (authored) taps.setTouchLaneLayout(skinLayout());
    const Vector3 scratch{authored ? .8125F : 1.05F, .5F, 0};
    taps.onFingerDown(91, scratch);
    taps.onFingerMove(91, {scratch.x, .2F, 0});
    expect(taps.presses == std::vector<int>{7} && taps.releases.empty() && taps.flickStates.empty(),
           "legacy tap scratch presses immediately and remains held during movement");
    taps.onFingerUp(91, scratch);
    expect(taps.releases == std::vector<int>{7}, "legacy tap scratch releases on lift");
  }
  RhythmInputHandler delayed;
  GamePlayScene delayedScene;
  delayed.setTouchLaneLayout(skinLayout());
  delayed.touchEventCallback = [&](SDL_FingerID finger, ReplayTouchAction action,
                                   Vector3 point, std::uint64_t time) {
    return delayedScene.handleTouchInput(finger, action, point, time);
  };
  delayed.ingressTimestamp = 123000;
  delayed.onFingerDown(77, firstLane);
  delayed.ingressTimestamp = 143000;
  delayed.onFingerUp(77, firstLane);
  expect(delayed.laneTimestamps == std::vector<std::uint64_t>{123000, 143000},
         "production touch handler forwards original timestamps to logical lane pipeline");
  expect(delayedScene.replayTimes == std::vector<long long>{13000, 33000},
         "production touch sample capture maps ingress through the audio and gameplay clocks");
  RhythmInputHandler handler;
  handler.setTouchLaneLayout(skinLayout());
  handler.onFingerDown(1, firstLane);
  handler.onFingerUp(1, firstLane);
  expect(handler.presses == std::vector<int>{3} &&
             handler.releases == std::vector<int>{3},
         "SDL tap follows authored lane with safe-area offset");
  handler.presses.clear(); handler.releases.clear();
  handler.onFingerDown(2, gap);
  handler.onFingerUp(2, gap);
  expect(handler.presses.empty(), "skin lane gap never clamps to a built-in lane");
  handler.onFingerDown(3, {.1875F, 1.2F, 0});
  handler.onFingerUp(3, firstLane);
  expect(handler.presses == std::vector<int>{3},
         "non-drag tap below authored lane retains vertical clamping");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = true;
  handler.onFingerDown(4, firstLane);
  handler.onFingerMove(4, secondLane);
  handler.onFingerMove(4, gap);
  handler.onFingerUp(4, gap);
  expect(handler.presses == std::vector<int>({3, 1}) &&
             handler.releases == std::vector<int>({3, 1}),
         "drag crosses unequal authored lanes and releases in their gap");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = false;
  const Vector3 scratch{.8125F, .5F, 0};
  handler.onFingerDown(5, scratch);
  expect(handler.presses.empty(), "authored scratch waits for flick");
  handler.onFingerMove(5, {.8125F, .4F, 0});
  handler.onFingerUp(5, scratch);
  expect(handler.presses == std::vector<int>{7} &&
             handler.releases == std::vector<int>{7},
         "authored scratch flick and lift retain scratch ownership");
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(gameplay::RealtimeTouchLayout{});
  handler.onFingerDown(6, firstLane);
  expect(handler.presses.empty(), "unpublished skin geometry fails closed");
  handler.onFingerUp(6, firstLane);
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(std::nullopt);
  handler.onFingerDown(7, {.1875F, .5F, 0});
  handler.onFingerUp(7, firstLane);
  expect(handler.presses == std::vector<int>{1} &&
             handler.releases == std::vector<int>{1},
         "built-in camera routing remains unchanged without a skin layout");
  handler.presses.clear(); handler.releases.clear();
  handler.setTouchLaneLayout(skinLayout());
  handler.onFingerDown(8, firstLane);
  handler.setTouchLaneLayout(skinLayout());
  expect(handler.releases.empty(), "ordinary frame publication preserves held notes");
  auto replacement = skinLayout();
  ++replacement.revision;
  handler.setTouchLaneLayout(replacement);
  expect(handler.releases == std::vector<int>{3},
         "layout revision change releases the original held lane");
  handler.onFingerUp(8, firstLane);
  expect(handler.releases == std::vector<int>{3},
         "lift after layout invalidation cannot release twice");
  handler.presses.clear(); handler.releases.clear();
  rendering::ui_scale_x = 2.0F;
  rendering::ui_scale_y = .5F;
  handler.onFingerDown(9, {.09375F, 1.0F, 0});
  handler.onFingerUp(9, {.09375F, 1.0F, 0});
  expect(handler.presses == std::vector<int>{3},
         "independent UI scales map to the same authored drawable lane");
  handler.presses.clear(); handler.releases.clear();
  handler.dragModeEnabled = true;
  handler.onFingerDown(10, {.09375F, 3.0F, 0});
  handler.onFingerUp(10, {.09375F, 3.0F, 0});
  expect(handler.presses.empty(), "drag mode does not vertically clamp outside skin lanes");

  RhythmInputHandler interrupted;
  interrupted.setTouchLaneLayout(skinLayout());
  bool callbackCapture = false;
  int callbackCancels = 0;
  Vector3 cancelledPoint{};
  interrupted.touchEventCallback = [&](SDL_FingerID finger, ReplayTouchAction action,
                                       Vector3 point, std::uint64_t) {
    if (finger != 20) return false;
    if (action == ReplayTouchAction::Down) callbackCapture = true;
    if (action == ReplayTouchAction::Cancel) {
      callbackCapture = false;
      ++callbackCancels;
      cancelledPoint = point;
    }
    if (action == ReplayTouchAction::Up) callbackCapture = false;
    return true;
  };
  interrupted.onFingerDown(20, firstLane);
  interrupted.onFingerMove(20, secondLane);
  expect(interrupted.fingerToLane.empty() && callbackCapture,
         "callback-consumed Down owns no gameplay lane");
  interrupted.discardPendingTouchEvents();
  expect(!callbackCapture && callbackCancels == 1 &&
             cancelledPoint.x == secondLane.x && cancelledPoint.y == secondLane.y,
         "background discard cancels callback-owned touch at its latest location");
  interrupted.discardPendingTouchEvents();
  expect(callbackCancels == 1, "repeated background/foreground discard cancels once");
  interrupted.setApplicationBackground(true);
  expect(interrupted.applicationBackground && interrupted.pipeline.resets == 1,
         "background clears physical binding ownership as well as touch ownership");
  interrupted.onFingerDown(20, firstLane);
  interrupted.onFingerMove(20, secondLane);
  expect(!callbackCapture && interrupted.fingerToLane.empty(),
         "inactive legacy input cannot acquire a new pointer or lane");
  interrupted.setApplicationBackground(false);
  expect(!interrupted.applicationBackground && interrupted.pipeline.resets == 1,
         "foreground restores input without another synthetic binding release");
  interrupted.onFingerDown(20, firstLane);
  interrupted.onFingerUp(20, firstLane);
  interrupted.discardPendingTouchEvents();
  expect(callbackCancels == 1,
         "a reused pointer ID that already lifted receives no stale cancellation");
  interrupted.onFingerDown(20, firstLane);
  interrupted.cancelInputState();
  expect(!interrupted.applicationBackground && !callbackCapture &&
             callbackCancels == 2 && interrupted.pipeline.resets == 2,
         "input recovery cancels held controls without changing application activity");

  rendering::ui_scale_x = rendering::ui_scale_y = 1.0F;
  std::vector<ReplayTouchAction> laneCallbacks;
  interrupted.touchEventCallback = [&](SDL_FingerID, ReplayTouchAction action,
                                       Vector3, std::uint64_t) {
    laneCallbacks.push_back(action);
    return action == ReplayTouchAction::Cancel;
  };
  interrupted.onFingerDown(21, firstLane);
  interrupted.discardPendingTouchEvents();
  interrupted.discardPendingTouchEvents();
  expect(laneCallbacks == std::vector<ReplayTouchAction>({ReplayTouchAction::Down,
                                                        ReplayTouchAction::Cancel}) &&
             interrupted.presses == std::vector<int>{3} &&
             interrupted.releases == std::vector<int>{3},
         "discard closes callback lifecycle and releases a held lane once even if Cancel is consumed");

  for (int blocked = 0; blocked < 3; ++blocked) {
    GamePlayScene scene;
    scene.context.jukebox.paused = blocked == 0;
    scene.practiceAllowed = blocked != 1;
    scene.ownedState.isEnding = blocked == 2;
    scene.floatingLaneCoverDragActive = true;
    scene.floatingLaneCoverDragChanged = true;
    scene.floatingLaneCoverFinger = 20;
    scene.floatingLaneCoverDragOffsetY = 15;
    scene.visualState.active[20] = firstLane;
    (void)scene.handleTouchInputAtGameplayTime(20, ReplayTouchAction::Cancel,
                                              firstLane, 1'000);
    expect(!scene.floatingLaneCoverDragActive &&
               scene.floatingLaneCoverFinger == -1 &&
               scene.floatingLaneCoverDragOffsetY == 0 && scene.persisted == 1,
           "Cancel retires lane-cover capture even when gameplay input is gated");
    expect(scene.visualState.active.empty(),
           "Cancel closes published live-touch visualization while paused or gated");
  }
  for (const bool android : {false, true}) {
    fixtureAndroid = android;
    RhythmInputHandler input;
    FixturePresentation presentation;
    GamePlayScene scene;
    scene.inputHandler = &input;
    scene.presentation = &presentation;
    scene.realtimeAuthority = true;
    scene.refreshLegacyTouchLayout();
    presentation.layout = skinLayout();
    scene.refreshLegacyTouchLayout();
    input.onFingerDown(99, firstLane);
    input.onFingerUp(99, firstLane);
    expect(android ? input.presses == std::vector<int>{3}
                   : !input.touchLaneLayout.has_value(),
           "late skin geometry updates Android legacy touch routing under realtime authority only");
    if (android) {
      presentation.mode = PresentationMode::BuiltIn;
      scene.refreshLegacyTouchLayout();
      expect(!input.touchLaneLayout.has_value(), "built-in fallback clears authored touch regions");
    }
  }
  return failures ? 1 : 0;
}
