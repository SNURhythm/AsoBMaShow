#pragma once

#include "../../input/InputTypes.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace skin {

struct LuaSkinLegacyControllerSnapshot {
  std::string name;
  std::vector<int> pressedButtons;
};

struct LuaSkinLegacyInputSnapshot {
  int drawableWidth = 0;
  int drawableHeight = 0;
  bool anyKeyPressed = false;
  std::vector<int> pressedKeys;
  std::vector<LuaSkinLegacyControllerSnapshot> controllers;
  std::vector<int> justPressedKeys;
  std::bitset<5> pressedMouseButtons;
  int pointerX = 0;
  int pointerY = 0;
  int deltaX = 0;
  int deltaY = 0;
  bool justTouched = false;
};

using LuaSkinLegacyInputGeneration = input::LegacyInputGeneration;

// Owns one value-only input publication. Lua receives no SDL window,
// keyboard-state, joystick, or controller pointer through this facade.
class LuaSkinLegacyInputHost final {
public:
  LuaSkinLegacyInputHost();
  explicit LuaSkinLegacyInputHost(LuaSkinLegacyInputSnapshot);

  [[nodiscard]] bool publish(LuaSkinLegacyInputSnapshot) noexcept;
  void publish(LuaSkinLegacyInputGeneration) noexcept;
  [[nodiscard]] int drawableWidth() const noexcept;
  [[nodiscard]] int drawableHeight() const noexcept;
  [[nodiscard]] bool isKeyPressed(int gdxKeyCode) const noexcept;
  [[nodiscard]] bool isKeyJustPressed(int gdxKeyCode) const noexcept;
  [[nodiscard]] bool isButtonPressed(int gdxButton) const noexcept;
  [[nodiscard]] int getX() const noexcept;
  [[nodiscard]] int getY() const noexcept;
  [[nodiscard]] int getDeltaX() const noexcept;
  [[nodiscard]] int getDeltaY() const noexcept;
  [[nodiscard]] bool justTouched() const noexcept;
  // Upstream browser gesture: pointer press or ENTER/SPACE down in this frame.
  [[nodiscard]] bool hasCurrentPressEdge() const noexcept;
  // Java field names (case insensitive), then display labels; unknown => nullopt.
  [[nodiscard]] static std::optional<int> keyConstant(std::string_view) noexcept;
  [[nodiscard]] std::size_t controllerCount() const noexcept;
  [[nodiscard]] std::string_view
  controllerName(std::size_t index) const noexcept;
  [[nodiscard]] bool controllerButtonPressed(std::size_t index,
                                             int button) const noexcept;

  // Mirrors the pinned LibGDX Input.Keys.valueOf display-name lookup. The
  // source method returns -1 (ANY_KEY) for an unknown, case-mismatched name.
  [[nodiscard]] static int keyCode(std::string_view name) noexcept;

private:
  LuaSkinLegacyInputGeneration generation_;
};

} // namespace skin
