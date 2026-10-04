#include "../src/settings/PresentationOrientationState.h"
#include "../src/AppSettings.h"
#include <cassert>
#include <iostream>

int main() {
  using namespace player_settings;
  PresentationOrientationState state;
  AppSettings settings;
  settings.presentation(PresentationOrientation::Landscape).laneLength = 11;
  settings.presentation(PresentationOrientation::Portrait).laneLength = 24;
  assert(!state.updateViewport(1920, 1080));
  assert(state.updateViewport(1080, 1920));
  settings.setActivePresentationOrientation(state.orientation());
  assert(settings.presentation().laneLength == 24);
  state.setGameplayLocked(true);
  assert(!state.updateViewport(1920, 1080));
  assert(state.orientation() == PresentationOrientation::Portrait);
  state.setGameplayLocked(true); // pause/retry retains ownership
  assert(!state.updateViewport(0, 0));
  assert(!state.updateViewport(1000, 1000));
  assert(state.orientation() == PresentationOrientation::Portrait);
  state.setGameplayLocked(false);
  assert(state.orientation() == PresentationOrientation::Landscape);
  settings.setActivePresentationOrientation(state.orientation());
  assert(settings.presentation().laneLength == 11);
  assert(state.updateViewport(768, 1024));
  assert(!state.updateViewport(1024, 1024));
  assert(state.orientation() == PresentationOrientation::Portrait);
  std::cout << "presentation orientation state tests passed\n";
}
