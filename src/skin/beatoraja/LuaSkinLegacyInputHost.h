#pragma once

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
};

// Owns one value-only input publication. Lua receives no SDL window,
// keyboard-state, joystick, or controller pointer through this facade.
class LuaSkinLegacyInputHost final {
public:
  LuaSkinLegacyInputHost();
  explicit LuaSkinLegacyInputHost(LuaSkinLegacyInputSnapshot);

  [[nodiscard]] bool publish(LuaSkinLegacyInputSnapshot) noexcept;
  [[nodiscard]] int drawableWidth() const noexcept;
  [[nodiscard]] int drawableHeight() const noexcept;
  [[nodiscard]] bool isKeyPressed(int gdxKeyCode) const noexcept;
  [[nodiscard]] std::size_t controllerCount() const noexcept;
  [[nodiscard]] const LuaSkinLegacyControllerSnapshot *
  controller(std::size_t index) const noexcept;

  // Mirrors the pinned LibGDX Input.Keys.valueOf display-name lookup. The
  // source method returns -1 (ANY_KEY) for an unknown, case-mismatched name.
  [[nodiscard]] static int keyCode(std::string_view name) noexcept;

private:
  std::shared_ptr<const LuaSkinLegacyInputSnapshot> snapshot_;
};

} // namespace skin
