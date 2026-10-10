#include "platform/ApplicationEventQueue.h"
#include "input/SDLPointerEvent.h"
#include "view/ScrollMomentum.h"
#include <SDL3/SDL.h>
#include "music_select/MusicSelectInputBindingAdapter.h"
#include "scene/IntroSceneNavigation.h"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::cerr << "line " << __LINE__ << ": " << #value << '\n'; std::exit(1); } } while (false)
struct View {
  struct EventDispatchLifetime {
    explicit EventDispatchLifetime(View &) {}
    bool alive() const { return true; }
  };
  virtual ~View() = default;
  std::vector<View *> children;
  bool visible = true;
  virtual bool handleEventsImpl(SDL_Event &) { return true; }
  virtual void onPointerEventConsumed(const SDL_Event &) {}
  virtual void onPointerInputCancelled() {}
  bool handleEvents(SDL_Event &event) { return handleEventsImpl(event); }
  void notifyPointerEventConsumed(const SDL_Event &event) { onPointerEventConsumed(event); }
  VIEW_CANCEL
};
struct Button : View {
  bool enabled = true, mousePressedInside = false, isHovered = false;
  SDL_FingerID activeTouchId = -1;
  std::unique_ptr<View> contentView;
  std::function<void()> onClickListener;
  bool handleEventsImpl(SDL_Event &) override;
  void onPointerEventConsumed(const SDL_Event &) override;
  void onPointerInputCancelled() override;
};
bool isInsideButton(const Button &, float x, float y) { return x >= 0 && x < 100 && y >= 0 && y < 100; }
template<class T> void mouseEventToUi(const T &event, int &x, int &y) { x = event.x; y = event.y; }
void mouseCoordsToUi(float x, float y, int &outX, int &outY) { outX = x; outY = y; }
void fingerEventToUi(const SDL_TouchFingerEvent &event, float &x, float &y) { x = event.x * 100; y = event.y * 100; }
struct ScrollView : View {
  bool mousePressedInside = true, mouseDragging = true, mouseCapturedByContent = true, cancelMouseClick = true;
  bool touchPressedInside = true, touchDragging = true, touchCapturedByContent = true, cancelTouchClick = true;
  SDL_FingerID activeTouchId = 11;
  ScrollMomentum touchMomentum;
  std::unique_ptr<View> contentView;
  void onPointerInputCancelled() override;
};
SCROLL_CANCEL
struct RecyclerView : View {
  SDL_FingerID touchId = 11;
  int touchPressIndex = 2;
  bool touchDragging = true;
  ScrollMomentum touchMomentum;
  std::vector<std::pair<View *, int>> viewEntries;
  std::vector<View *> recycledViewEntries;
  RECYCLER_CANCEL
};
struct OverlayPortal : View {
  std::vector<View *> presented;
  PORTAL_CANCEL
};
struct Scene {
  std::vector<View *> views;
  SCENE_RECOVERY
};
struct IntroScene : Scene {
  std::unique_ptr<MusicSelectInputBindingAdapter> inputBindingAdapter_;
  IntroSceneNavigation navigation_{MusicSelectKeyLayout::Beat7K};
  struct { struct { int skinMusicSelectInput = 0; } settings; } context;
  void onInputQueueOverflow() override;
};
INTRO_CANCEL
struct InputCaptureController {
  enum class State { Listening, AwaitingConflictConfirmation };
  State state_ = State::Listening;
  std::map<input::PhysicalControl, bool> activationStates_;
  std::optional<input::PhysicalInputEvent> monitorSample_;
  InputProfile profile_;
  InputBindingResolver resolver_{profile_, {}, {}};
  static constexpr float kAxisCaptureReleaseThreshold = .1F;
  static constexpr float kAxisCaptureActivationThreshold = .2F;
  static constexpr float kNonAxisCaptureActivationThreshold = .5F;
  int candidates = 0;
  void stageCandidate(input::PhysicalControl) { ++candidates; }
  void considerControlActivation(const input::PhysicalInputEvent &, input::PhysicalControl, float);
  void resetInputState();
};
CAPTURE_ACTIVATE
CAPTURE_RESET
BUTTON_HANDLER
BUTTON_CONSUMED
BUTTON_CANCEL

SDL_Event finger(Uint32 type, SDL_FingerID id) {
  SDL_Event result{};
  result.type = type;
  result.tfinger.touchID = 1;
  result.tfinger.fingerID = id;
  result.tfinger.x = result.tfinger.y = .5F;
  return result;
}
void testDroppedReleaseRecoversWithoutClick() {
  platform::ApplicationEventQueue queue(1);
  Button owner;
  auto content = std::make_unique<Button>();
  auto *button = content.get();
  owner.contentView = std::move(content);
  View hiddenRoot;
  hiddenRoot.children.push_back(&owner);
  Scene scene;
  scene.views.push_back(&hiddenRoot);
  int clicks = 0;
  button->onClickListener = [&] { ++clicks; };
  auto down = finger(SDL_EVENT_FINGER_DOWN, 11);
  owner.handleEvents(down);
  CHECK(button->activeTouchId == 11);
  // Visibility must not prevent retiring an earlier capture.
  hiddenRoot.visible = false;
  CHECK(queue.push(finger(SDL_EVENT_FINGER_UP, 11)));
  SDL_Event key{};
  key.type = SDL_EVENT_KEY_DOWN;
  queue.push(key);
  CHECK(queue.takeOverflow());
  scene.onInputQueueOverflow();
  CHECK(clicks == 0);
  CHECK(button->activeTouchId == -1);
  auto staleRelease = finger(SDL_EVENT_FINGER_UP, 11);
  owner.handleEvents(staleRelease);
  CHECK(clicks == 0);
  auto nextDown = finger(SDL_EVENT_FINGER_DOWN, 12);
  CHECK(!owner.handleEvents(nextDown));
  auto nextUp = finger(SDL_EVENT_FINGER_UP, 12);
  CHECK(!owner.handleEvents(nextUp));
  CHECK(clicks == 1);
  button->mousePressedInside = true;
  scene.onInputQueueOverflow();
  CHECK(!button->mousePressedInside);
  CHECK(clicks == 1);
}
void testIntroOverflowStopsRepeatAndAcceptsNextStart() {
  IntroScene scene;
  InputProfile profile;
  scene.inputBindingAdapter_ = std::make_unique<MusicSelectInputBindingAdapter>(profile, MusicSelectKeyLayout::Beat7K);
  auto &state = scene.inputBindingAdapter_->state();
  state.controlHeld.insert(MusicSelectControlKey::Down);
  (void)scene.navigation_.process(state, 1000);
  CHECK(scene.navigation_.choice() == IntroSceneChoice::Settings);
  state.start = true;
  CHECK(scene.navigation_.process(state, 1001).activated.has_value());
  scene.onInputQueueOverflow();
  CHECK(state.controlHeld.empty());
  CHECK(!state.start);
  CHECK(!scene.navigation_.process(state, 3000).selectionChanged);
  CHECK(scene.navigation_.choice() == IntroSceneChoice::Settings);
  // A fresh Start is accepted even if no intermediate frame saw a release.
  state.start = true;
  CHECK(scene.navigation_.process(state, 3001).activated == IntroSceneChoice::Settings);
}
void testBindingCaptureRearmsWithoutEndingEditing() {
  InputCaptureController capture;
  input::PhysicalControl control{};
  capture.considerControlActivation({}, control, 1.F);
  CHECK(capture.candidates == 1);
  capture.considerControlActivation({}, control, 1.F);
  CHECK(capture.candidates == 1);
  capture.resetInputState();
  CHECK(capture.candidates == 1);
  CHECK(capture.state_ == InputCaptureController::State::Listening);
  capture.considerControlActivation({}, control, 1.F);
  CHECK(capture.candidates == 2);
  capture.state_ = InputCaptureController::State::AwaitingConflictConfirmation;
  capture.resetInputState();
  CHECK(capture.state_ == InputCaptureController::State::AwaitingConflictConfirmation);
}
void testDetachedContentAndScrollingRecoverTogether() {
  ScrollView scroll;
  auto button = std::make_unique<Button>();
  auto *nested = button.get();
  auto down = finger(SDL_EVENT_FINGER_DOWN, 11);
  nested->handleEvents(down);
  scroll.contentView = std::move(button);
  scroll.touchMomentum.beginDrag(0);
  scroll.touchMomentum.recordDragDelta(50, 10);
  scroll.touchMomentum.release(10);
  Button recycled;
  recycled.handleEvents(down);
  RecyclerView recycler;
  recycler.viewEntries.emplace_back(&scroll, 0);
  recycler.recycledViewEntries.push_back(&recycled);
  OverlayPortal portal;
  portal.presented.push_back(&recycler);
  Scene scene;
  scene.views.push_back(&portal);
  scene.onInputQueueOverflow();
  CHECK(!scroll.mousePressedInside && !scroll.mouseDragging && !scroll.mouseCapturedByContent && !scroll.cancelMouseClick);
  CHECK(scroll.activeTouchId == -1 && !scroll.touchPressedInside && !scroll.touchDragging && !scroll.touchCapturedByContent && !scroll.cancelTouchClick);
  float delta = 0;
  CHECK(!scroll.touchMomentum.step(20, delta));
  CHECK(recycler.touchId == -1 && recycler.touchPressIndex == -1 && !recycler.touchDragging);
  CHECK(nested->activeTouchId == -1 && recycled.activeTouchId == -1);
  auto fresh = finger(SDL_EVENT_FINGER_DOWN, 12);
  CHECK(!nested->handleEvents(fresh) && !recycled.handleEvents(fresh));
}
int main() {
  testDetachedContentAndScrollingRecoverTogether();
  testDroppedReleaseRecoversWithoutClick();
  testIntroOverflowStopsRepeatAndAcceptsNextStart();
  testBindingCaptureRearmsWithoutEndingEditing();
}
