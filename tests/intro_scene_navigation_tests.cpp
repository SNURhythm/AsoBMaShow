#include "scene/IntroSceneNavigation.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::size_t assignedLane(MusicSelectKeyLayout layout,
                         MusicSelectAssignedKey key) {
  const std::size_t count = layout == MusicSelectKeyLayout::Beat14K ? 18 : 9;
  for (std::size_t lane = 0; lane < count; ++lane) {
    if (musicSelectKeyAssigned(layout, lane, key)) return lane;
  }
  std::cerr << "required intro navigation lane is not assigned\n";
  std::exit(1);
}

MusicSelectLogicalInput logicalInput(MusicSelectKeyLayout layout) {
  const std::size_t count = layout == MusicSelectKeyLayout::Beat14K ? 18 : 9;
  MusicSelectLogicalInput input;
  input.keys.resize(count);
  input.changed.resize(count);
  input.analog.resize(count);
  input.analogDelta.resize(count);
  return input;
}

void testKeyboardNavigationAndEnterActivateSettings() {
  IntroSceneNavigation navigation(MusicSelectKeyLayout::Beat7K);
  auto down = logicalInput(MusicSelectKeyLayout::Beat7K);
  down.controlHeld.insert(MusicSelectControlKey::Down);
  const auto moved = navigation.process(down, 1'000);
  require(moved.selectionChanged &&
              navigation.choice() == IntroSceneChoice::Settings &&
              !moved.activated,
          "Down selects Settings without activating it");

  auto enter = logicalInput(MusicSelectKeyLayout::Beat7K);
  enter.controlPressed.insert(MusicSelectControlKey::Enter);
  const auto activated = navigation.process(enter, 1'001);
  require(activated.activated &&
              *activated.activated == IntroSceneChoice::Settings,
          "Enter activates the keyboard-selected Settings button");
}

void testConfiguredNavigationAndStartArePointerIndependent() {
  constexpr auto layout = MusicSelectKeyLayout::Beat7K;
  IntroSceneNavigation navigation(layout);
  auto configuredDown = logicalInput(layout);
  const auto lane = assignedLane(layout, MusicSelectAssignedKey::Up);
  configuredDown.keys[lane] = true;
  configuredDown.changed[lane] = true;
  const auto moved = navigation.process(configuredDown, 2'000);
  require(moved.selectionChanged &&
              navigation.choice() == IntroSceneChoice::Settings,
          "the configured selector navigation key reaches Settings");

  auto start = logicalInput(layout);
  start.start = true;
  const auto firstStart = navigation.process(start, 2'001);
  const auto heldStart = navigation.process(start, 2'002);
  require(firstStart.activated &&
              *firstStart.activated == IntroSceneChoice::Settings &&
              !heldStart.activated,
          "configured Start activates once on its pressed edge");
}

} // namespace

int main() {
  testKeyboardNavigationAndEnterActivateSettings();
  testConfiguredNavigationAndStartArePointerIndependent();
  return 0;
}
