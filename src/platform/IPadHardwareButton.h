#pragma once
#include <string_view>

namespace ipad_hardware {
enum class Button { Unknown, Home, Top };
enum class Edge { Unknown, Top, Right, Bottom, Left };
// Matches UIKit's interface orientation semantics (Home on the named side).
enum class Orientation { Unknown, Portrait, PortraitUpsideDown, LandscapeLeft, LandscapeRight };
struct Model {
  Button button = Button::Unknown;
  float portraitX = -1.0F;
};
struct ButtonLocation {
  Button button = Button::Unknown;
  Edge edge = Edge::Unknown;
  float x = 0.5F;
  float y = 0.5F;
};
inline Model modelForIdentifier(std::string_view identifier) {
  static constexpr struct Entry {
    std::string_view identifier;
    Model model;
  } models[] = {
#include "IPadHardwareButtonModels.inc"
  };
  for (const auto &entry : models) {
    if (entry.identifier == identifier) return entry.model;
  }
  return {};
}

inline ButtonLocation locateButton(Model model, Orientation orientation,
                                   bool fillsBuiltInScreen) {
  ButtonLocation location;
  location.button = model.button;
  if (!fillsBuiltInScreen || model.portraitX < 0.0F ||
      orientation == Orientation::Unknown) return location;
  const bool home = model.button == Button::Home;
  const float x = model.portraitX;
  const float y = home ? 1.0F : 0.0F;
  switch (orientation) {
    case Orientation::Portrait:
      return {model.button, home ? Edge::Bottom : Edge::Top, x, y};
    case Orientation::PortraitUpsideDown:
      return {model.button, home ? Edge::Top : Edge::Bottom, 1.0F - x, 1.0F - y};
    case Orientation::LandscapeLeft:
      return {model.button, home ? Edge::Left : Edge::Right, 1.0F - y, x};
    case Orientation::LandscapeRight:
      return {model.button, home ? Edge::Right : Edge::Left, y, 1.0F - x};
    default: return location;
  }
}
} // namespace ipad_hardware
