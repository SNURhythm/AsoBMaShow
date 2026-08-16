#include "practice/PracticeConfiguration.h"
#include "scene/SceneEventRouting.h"

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
}
