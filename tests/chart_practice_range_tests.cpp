#include "practice/PracticeConfiguration.h"
#include "scene/SceneEventRouting.h"
#include "scene/play/SkinTextInputLifecycle.h"

#include <cassert>
#include <vector>

int main() {
  struct LifecycleScene {
    std::vector<bool> changes;
    void onApplicationBackgroundChanged(bool background) {
      changes.push_back(background);
    }
  };
  LifecycleScene selectScene;
  LifecycleScene replacementScene;
  scene_event_routing::dispatchApplicationBackgroundChange(&selectScene, true);
  scene_event_routing::dispatchApplicationBackgroundChange(&replacementScene,
                                                           false);
  scene_event_routing::dispatchApplicationBackgroundChange<LifecycleScene>(
      nullptr, false);
  assert(selectScene.changes == std::vector<bool>{true});
  assert(replacementScene.changes == std::vector<bool>{false});

  practice::RangeSelection selection{.startMicros = 1'000'000,
                                     .endMicros = 5'000'000,
                                     .active = practice::Marker::End};
  selection.placeActiveMarker(500'000, 8'000'000);
  assert(selection.startMicros == 500'000);
  assert(selection.endMicros == 1'000'000);
  assert(selection.active == practice::Marker::Start);

  selection.placeActiveMarker(9'000'000, 8'000'000);
  assert(selection.startMicros == 1'000'000);
  assert(selection.endMicros == 8'000'000);
  assert(selection.active == practice::Marker::End);

  const std::vector<long long> timelines = {0, 1'000'000, 1'000'000, 2'500'000,
                                            5'000'000};
  assert(practice::adjacentTimelineMicros(timelines, 1'000'000,
                                          practice::TimelineDirection::Next) ==
         2'500'000);
  assert(practice::adjacentTimelineMicros(
             timelines, 2'500'000, practice::TimelineDirection::Previous) ==
         1'000'000);
  assert(!practice::adjacentTimelineMicros(
      timelines, 0, practice::TimelineDirection::Previous));
  assert(!practice::adjacentTimelineMicros(timelines, 5'000'000,
                                           practice::TimelineDirection::Next));

  constexpr Uint32 previouslyForwarded[] = {
      SDL_EVENT_QUIT,
      SDL_EVENT_WINDOW_FOCUS_LOST,
      SDL_EVENT_KEY_DOWN,
      SDL_EVENT_KEY_UP,
      SDL_EVENT_TEXT_INPUT,
      SDL_EVENT_TEXT_EDITING,
      SDL_EVENT_MOUSE_MOTION,
      SDL_EVENT_MOUSE_BUTTON_DOWN,
      SDL_EVENT_MOUSE_BUTTON_UP,
      SDL_EVENT_MOUSE_WHEEL,
      SDL_EVENT_FINGER_DOWN,
      SDL_EVENT_FINGER_MOTION,
      SDL_EVENT_FINGER_UP,
  };
  for (const Uint32 eventType : previouslyForwarded) {
    assert(scene_event_routing::shouldDispatchToScene(eventType));
  }
  assert(scene_event_routing::shouldDispatchToScene(SDL_EVENT_GAMEPAD_BUTTON_DOWN));
  assert(scene_event_routing::shouldDispatchToScene(SDL_EVENT_GAMEPAD_BUTTON_UP));
  assert(!scene_event_routing::shouldDispatchToScene(SDL_EVENT_GAMEPAD_AXIS_MOTION));
  assert(
      !scene_event_routing::shouldDispatchToScene(SDL_EVENT_GAMEPAD_SENSOR_UPDATE));
  assert(!scene_event_routing::shouldDispatchToScene(SDL_EVENT_JOYSTICK_BUTTON_DOWN));

  SDL_Event mouseSynthesizedTouch{};
  mouseSynthesizedTouch.type = SDL_EVENT_FINGER_DOWN;
  mouseSynthesizedTouch.tfinger.type = SDL_EVENT_FINGER_DOWN;
  mouseSynthesizedTouch.tfinger.touchID = SDL_MOUSE_TOUCHID;
  assert(!scene_event_routing::shouldDispatchToScene(mouseSynthesizedTouch));

  SDL_Event directTouch{};
  directTouch.type = SDL_EVENT_FINGER_DOWN;
  directTouch.tfinger.type = SDL_EVENT_FINGER_DOWN;
  directTouch.tfinger.touchID = 42;
  assert(scene_event_routing::shouldDispatchToScene(directTouch));

  SDL_Event lifecycle{};
  lifecycle.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  for (const Uint32 windowEvent : {SDL_EVENT_WINDOW_FOCUS_LOST,
                                  SDL_EVENT_WINDOW_MINIMIZED,
                                  SDL_EVENT_WINDOW_HIDDEN}) {
    lifecycle.type = windowEvent;
    assert(skin_text_input_lifecycle::shouldCommit(lifecycle, true));
    assert(!skin_text_input_lifecycle::shouldCommit(lifecycle, false));
    int commits = 0;
    assert(skin_text_input_lifecycle::route(
               lifecycle, true, [&] {
                 ++commits;
                 return true;
               }) ==
           skin_text_input_lifecycle::CommitResult::Committed);
    assert(commits == 1);
  }
  for (const Uint32 appEvent : {SDL_EVENT_WILL_ENTER_BACKGROUND,
                                SDL_EVENT_DID_ENTER_BACKGROUND}) {
    lifecycle.type = appEvent;
    assert(skin_text_input_lifecycle::shouldCommit(lifecycle, true));
  }
  lifecycle.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  assert(!skin_text_input_lifecycle::shouldCommit(lifecycle, true));
  int commits = 0;
  assert(skin_text_input_lifecycle::route(
             lifecycle, true, [&] {
               ++commits;
               return true;
             }) == skin_text_input_lifecycle::CommitResult::NotRequested);
  assert(commits == 0);
  lifecycle.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  assert(skin_text_input_lifecycle::route(
             lifecycle, true, [&] {
               ++commits;
               return false;
             }) == skin_text_input_lifecycle::CommitResult::Retained);
  assert(commits == 1);
}
