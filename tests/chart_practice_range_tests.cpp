#include "practice/PracticeConfiguration.h"
#include "scene/SceneEventRouting.h"
#include "scene/play/SkinTextInputLifecycle.h"

#include <cassert>
#include <vector>

int main() {
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
      SDL_QUIT,
      SDL_WINDOWEVENT,
      SDL_KEYDOWN,
      SDL_KEYUP,
      SDL_TEXTINPUT,
      SDL_TEXTEDITING,
      SDL_TEXTEDITING_EXT,
      SDL_MOUSEMOTION,
      SDL_MOUSEBUTTONDOWN,
      SDL_MOUSEBUTTONUP,
      SDL_MOUSEWHEEL,
      SDL_FINGERDOWN,
      SDL_FINGERMOTION,
      SDL_FINGERUP,
  };
  for (const Uint32 eventType : previouslyForwarded) {
    assert(scene_event_routing::shouldDispatchToScene(eventType));
  }
  assert(scene_event_routing::shouldDispatchToScene(SDL_CONTROLLERBUTTONDOWN));
  assert(scene_event_routing::shouldDispatchToScene(SDL_CONTROLLERBUTTONUP));
  assert(!scene_event_routing::shouldDispatchToScene(SDL_CONTROLLERAXISMOTION));
  assert(
      !scene_event_routing::shouldDispatchToScene(SDL_CONTROLLERSENSORUPDATE));
  assert(!scene_event_routing::shouldDispatchToScene(SDL_JOYBUTTONDOWN));

  SDL_Event mouseSynthesizedTouch{};
  mouseSynthesizedTouch.type = SDL_FINGERDOWN;
  mouseSynthesizedTouch.tfinger.type = SDL_FINGERDOWN;
  mouseSynthesizedTouch.tfinger.touchId = SDL_MOUSE_TOUCHID;
  assert(!scene_event_routing::shouldDispatchToScene(mouseSynthesizedTouch));

  SDL_Event directTouch{};
  directTouch.type = SDL_FINGERDOWN;
  directTouch.tfinger.type = SDL_FINGERDOWN;
  directTouch.tfinger.touchId = 42;
  assert(scene_event_routing::shouldDispatchToScene(directTouch));

  SDL_Event lifecycle{};
  lifecycle.type = SDL_WINDOWEVENT;
  for (const Uint8 windowEvent : {SDL_WINDOWEVENT_FOCUS_LOST,
                                  SDL_WINDOWEVENT_MINIMIZED,
                                  SDL_WINDOWEVENT_HIDDEN}) {
    lifecycle.window.event = windowEvent;
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
  for (const Uint32 appEvent : {SDL_APP_WILLENTERBACKGROUND,
                                SDL_APP_DIDENTERBACKGROUND}) {
    lifecycle.type = appEvent;
    assert(skin_text_input_lifecycle::shouldCommit(lifecycle, true));
  }
  lifecycle.type = SDL_WINDOWEVENT;
  lifecycle.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
  assert(!skin_text_input_lifecycle::shouldCommit(lifecycle, true));
  int commits = 0;
  assert(skin_text_input_lifecycle::route(
             lifecycle, true, [&] {
               ++commits;
               return true;
             }) == skin_text_input_lifecycle::CommitResult::NotRequested);
  assert(commits == 0);
  lifecycle.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
  assert(skin_text_input_lifecycle::route(
             lifecycle, true, [&] {
               ++commits;
               return false;
             }) == skin_text_input_lifecycle::CommitResult::Retained);
  assert(commits == 1);
}
