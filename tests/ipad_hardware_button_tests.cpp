#include "../src/platform/IPadHardwareButton.h"
#include "../src/scene/play/GuidedAccessButtonCue.h"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace ipad_hardware;

int main() {
  // Home-button iPads use the center of the chin, even if they have Touch ID.
  const auto home = modelForIdentifier("iPad12,1");
  assert(home.button == Button::Home);
  auto cue = locateButton(home, Orientation::Portrait, true);
  assert(cue.edge == Edge::Bottom && cue.x == 0.5F && cue.y == 1.0F);
  cue = locateButton(home, Orientation::PortraitUpsideDown, true);
  assert(cue.edge == Edge::Top && cue.x == 0.5F && cue.y == 0.0F);
  // These are UIInterfaceOrientation names, not UIDeviceOrientation names.
  cue = locateButton(home, Orientation::LandscapeLeft, true);
  assert(cue.edge == Edge::Left && cue.x == 0.0F && cue.y == 0.5F);
  cue = locateButton(home, Orientation::LandscapeRight, true);
  assert(cue.edge == Edge::Right && cue.x == 1.0F && cue.y == 0.5F);

  const auto pro = modelForIdentifier("iPad16,3");
  assert(pro.button == Button::Top);
  // Apple M4 drawing: 177.51 mm body, 160.13 mm display, button center
  // 13.98 + 12.06/2 mm from the right body edge => x ~= 0.929307.
  cue = locateButton(pro, Orientation::Portrait, true);
  assert(cue.edge == Edge::Top && std::abs(cue.x - 0.929307F) < 0.00001F);
  cue = locateButton(pro, Orientation::LandscapeRight, true);
  assert(cue.edge == Edge::Left && std::abs(cue.y - 0.070693F) < 0.00001F);
  cue = locateButton(pro, Orientation::LandscapeLeft, true);
  assert(cue.edge == Edge::Right && std::abs(cue.y - 0.929307F) < 0.00001F);
  cue = locateButton(pro, Orientation::PortraitUpsideDown, true);
  assert(cue.edge == Edge::Bottom && std::abs(cue.x - 0.070693F) < 0.00001F);

  // Mini 6/A17 Pro put the Top button on the other side; volume buttons
  // occupy the upper right. Never reuse the Air/Pro anchor for these models.
  for (const auto identifier : {"iPad14,1", "iPad14,2", "iPad16,1", "iPad16,2"}) {
    cue = locateButton(modelForIdentifier(identifier), Orientation::Portrait, true);
    assert(cue.button == Button::Top && cue.edge == Edge::Top);
    assert(std::abs(cue.x - 0.105971F) < 0.00001F);
  }
  assert(modelForIdentifier("iPad13,1").button == Button::Top); // Air 4 Touch ID
  assert(modelForIdentifier("iPad11,3").button == Button::Home); // Air 3 Touch ID
  assert(modelForIdentifier("iPad15,7").button == Button::Top); // iPad A16
  assert(modelForIdentifier("iPad17,4").button == Button::Top); // Pro M5 cellular

  // A known button type can still be explained when its location is uncertain.
  cue = locateButton(pro, Orientation::Portrait, false);
  assert(cue.button == Button::Top && cue.edge == Edge::Unknown);
  cue = locateButton(pro, Orientation::Unknown, true);
  assert(cue.button == Button::Top && cue.edge == Edge::Unknown);
  for (const auto identifier : {"", "iPad99,1", "iPad16,30", "iPhone16,3", "arm64"}) {
    cue = locateButton(modelForIdentifier(identifier), Orientation::Portrait, true);
    assert(cue.button == Button::Unknown && cue.edge == Edge::Unknown);
  }

  // Labels stay upright, inside safe bounds, and inward of their edge marker.
  auto layout = gameplay::layoutButtonCue(
      locateButton(pro, Orientation::Portrait, true), 1920, 2560, {40, 0, 30, 0});
  assert(layout.marker.width > layout.marker.height);
  assert(layout.marker.y < 20 && layout.label.y >= 40);
  assert(layout.label.x + layout.label.width <= 1920);
  layout = gameplay::layoutButtonCue(
      locateButton(pro, Orientation::LandscapeRight, true), 1920, 1440);
  assert(layout.marker.height > layout.marker.width);
  assert(layout.marker.x < 20 && layout.label.x > layout.marker.x);
  assert(layout.label.y >= 0);
  layout = gameplay::layoutButtonCue(
      locateButton(home, Orientation::LandscapeRight, true), 1920, 1440);
  assert(layout.marker.x > layout.label.x + layout.label.width);
  assert(layout.label.y < 720 && layout.label.y + layout.label.height > 720);
  layout = gameplay::layoutButtonCue(
      locateButton(home, Orientation::Portrait, true), 1920, 2560, {0, 0, 48, 0});
  assert(layout.label.y + layout.label.height <= 2560 - 48);
  assert(layout.marker.y > layout.label.y + layout.label.height);
  for (const auto orientation : {Orientation::Portrait, Orientation::PortraitUpsideDown,
                                 Orientation::LandscapeLeft, Orientation::LandscapeRight}) {
    for (const auto model : {home, pro, modelForIdentifier("iPad14,1")}) {
      layout = gameplay::layoutButtonCue(locateButton(model, orientation, true), 320, 480);
      assert(layout.label.width > 0 && layout.label.height > 0);
      assert(layout.label.x >= 0 && layout.label.y >= 0);
      assert(layout.label.x + layout.label.width <= 320);
      assert(layout.label.y + layout.label.height <= 480);
      assert(layout.marker.x >= 0 && layout.marker.y >= 0);
      assert(layout.marker.x + layout.marker.width <= 320);
      assert(layout.marker.y + layout.marker.height <= 480);
    }
  }
  layout = gameplay::layoutButtonCue(locateButton(pro, Orientation::Portrait, false),
                                   1920, 720);
  // The existing central help supplies text-only fallback. A second floating
  // label would cover the centered title in a short, wide partial window.
  assert(layout.marker.width == 0 && layout.label.width == 0);
  std::cout << "iPad hardware button models and orientation tests passed\n";
}
