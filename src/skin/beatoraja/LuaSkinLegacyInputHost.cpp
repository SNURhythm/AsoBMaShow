#include "LuaSkinLegacyInputHost.h"

#include <algorithm>
#include <array>
#include <utility>

namespace skin {
namespace {

struct KeyName {
  int code;
  std::string_view name;
};

constexpr auto kKeyNames = std::to_array<KeyName>({
    KeyName{0, "Unknown"},
    KeyName{1, "Soft Left"}, KeyName{2, "Soft Right"}, KeyName{3, "Home"},
    KeyName{4, "Back"}, KeyName{5, "Call"}, KeyName{6, "End Call"},
    KeyName{7, "0"}, KeyName{8, "1"}, KeyName{9, "2"}, KeyName{10, "3"},
    KeyName{11, "4"}, KeyName{12, "5"}, KeyName{13, "6"}, KeyName{14, "7"},
    KeyName{15, "8"}, KeyName{16, "9"}, KeyName{17, "*"}, KeyName{18, "#"},
    KeyName{19, "Up"}, KeyName{20, "Down"}, KeyName{21, "Left"},
    KeyName{22, "Right"}, KeyName{23, "Center"}, KeyName{24, "Volume Up"},
    KeyName{25, "Volume Down"}, KeyName{26, "Power"}, KeyName{27, "Camera"},
    KeyName{28, "Clear"}, KeyName{29, "A"}, KeyName{30, "B"},
    KeyName{31, "C"}, KeyName{32, "D"}, KeyName{33, "E"},
    KeyName{34, "F"}, KeyName{35, "G"}, KeyName{36, "H"},
    KeyName{37, "I"}, KeyName{38, "J"}, KeyName{39, "K"},
    KeyName{40, "L"}, KeyName{41, "M"}, KeyName{42, "N"},
    KeyName{43, "O"}, KeyName{44, "P"}, KeyName{45, "Q"},
    KeyName{46, "R"}, KeyName{47, "S"}, KeyName{48, "T"},
    KeyName{49, "U"}, KeyName{50, "V"}, KeyName{51, "W"},
    KeyName{52, "X"}, KeyName{53, "Y"}, KeyName{54, "Z"},
    KeyName{55, ","}, KeyName{56, "."}, KeyName{57, "L-Alt"},
    KeyName{58, "R-Alt"}, KeyName{59, "L-Shift"}, KeyName{60, "R-Shift"},
    KeyName{61, "Tab"}, KeyName{62, "Space"}, KeyName{63, "SYM"},
    KeyName{64, "Explorer"}, KeyName{65, "Envelope"}, KeyName{66, "Enter"},
    KeyName{67, "Delete"}, KeyName{68, "`"}, KeyName{69, "-"},
    KeyName{70, "="}, KeyName{71, "["}, KeyName{72, "]"},
    KeyName{73, "\\"}, KeyName{74, ";"}, KeyName{75, "'"},
    KeyName{76, "/"}, KeyName{77, "@"}, KeyName{78, "Num"},
    KeyName{79, "Headset Hook"}, KeyName{80, "Focus"}, KeyName{81, "Plus"},
    KeyName{82, "Menu"}, KeyName{83, "Notification"}, KeyName{84, "Search"},
    KeyName{85, "Play/Pause"}, KeyName{86, "Stop Media"},
    KeyName{87, "Next Media"}, KeyName{88, "Prev Media"},
    KeyName{89, "Rewind"}, KeyName{90, "Fast Forward"}, KeyName{91, "Mute"},
    KeyName{92, "Page Up"}, KeyName{93, "Page Down"},
    KeyName{94, "PICTSYMBOLS"}, KeyName{95, "SWITCH_CHARSET"},
    KeyName{96, "A Button"}, KeyName{97, "B Button"},
    KeyName{98, "C Button"}, KeyName{99, "X Button"},
    KeyName{100, "Y Button"}, KeyName{101, "Z Button"},
    KeyName{102, "L1 Button"}, KeyName{103, "R1 Button"},
    KeyName{104, "L2 Button"}, KeyName{105, "R2 Button"},
    KeyName{106, "Left Thumb"}, KeyName{107, "Right Thumb"},
    KeyName{108, "Start"}, KeyName{109, "Select"},
    KeyName{110, "Button Mode"}, KeyName{112, "Forward Delete"},
    KeyName{129, "L-Ctrl"}, KeyName{130, "R-Ctrl"},
    KeyName{131, "Escape"}, KeyName{132, "End"}, KeyName{133, "Insert"},
    KeyName{144, "Numpad 0"}, KeyName{145, "Numpad 1"},
    KeyName{146, "Numpad 2"}, KeyName{147, "Numpad 3"},
    KeyName{148, "Numpad 4"}, KeyName{149, "Numpad 5"},
    KeyName{150, "Numpad 6"}, KeyName{151, "Numpad 7"},
    KeyName{152, "Numpad 8"}, KeyName{153, "Numpad 9"},
    KeyName{243, ":"}, KeyName{244, "F1"}, KeyName{245, "F2"},
    KeyName{246, "F3"}, KeyName{247, "F4"}, KeyName{248, "F5"},
    KeyName{249, "F6"}, KeyName{250, "F7"}, KeyName{251, "F8"},
    KeyName{252, "F9"}, KeyName{253, "F10"}, KeyName{254, "F11"},
    KeyName{255, "F12"},
});

void normalize(LuaSkinLegacyInputSnapshot &snapshot) {
  snapshot.drawableWidth = std::max(0, snapshot.drawableWidth);
  snapshot.drawableHeight = std::max(0, snapshot.drawableHeight);
  std::ranges::sort(snapshot.pressedKeys);
  auto uniqueKeys = std::ranges::unique(snapshot.pressedKeys);
  snapshot.pressedKeys.erase(uniqueKeys.begin(), uniqueKeys.end());
  snapshot.anyKeyPressed =
      snapshot.anyKeyPressed || !snapshot.pressedKeys.empty();
  for (auto &controller : snapshot.controllers) {
    std::ranges::sort(controller.pressedButtons);
    auto uniqueButtons = std::ranges::unique(controller.pressedButtons);
    controller.pressedButtons.erase(uniqueButtons.begin(), uniqueButtons.end());
  }
}

} // namespace

LuaSkinLegacyInputHost::LuaSkinLegacyInputHost()
    : LuaSkinLegacyInputHost(LuaSkinLegacyInputSnapshot{}) {}

LuaSkinLegacyInputHost::LuaSkinLegacyInputHost(
    LuaSkinLegacyInputSnapshot snapshot) {
  if (!publish(std::move(snapshot))) {
    snapshot_ = std::make_shared<const LuaSkinLegacyInputSnapshot>();
  }
}

bool LuaSkinLegacyInputHost::publish(
    LuaSkinLegacyInputSnapshot snapshot) noexcept {
  try {
    normalize(snapshot);
    snapshot_ = std::make_shared<const LuaSkinLegacyInputSnapshot>(
        std::move(snapshot));
    return true;
  } catch (...) {
    return false;
  }
}

int LuaSkinLegacyInputHost::drawableWidth() const noexcept {
  return snapshot_ ? snapshot_->drawableWidth : 0;
}

int LuaSkinLegacyInputHost::drawableHeight() const noexcept {
  return snapshot_ ? snapshot_->drawableHeight : 0;
}

bool LuaSkinLegacyInputHost::isKeyPressed(int gdxKeyCode) const noexcept {
  if (!snapshot_) {
    return false;
  }
  if (gdxKeyCode == -1) {
    return snapshot_->anyKeyPressed;
  }
  return std::ranges::binary_search(snapshot_->pressedKeys, gdxKeyCode);
}

std::size_t LuaSkinLegacyInputHost::controllerCount() const noexcept {
  return snapshot_ ? snapshot_->controllers.size() : 0;
}

const LuaSkinLegacyControllerSnapshot *
LuaSkinLegacyInputHost::controller(std::size_t index) const noexcept {
  return snapshot_ && index < snapshot_->controllers.size()
             ? &snapshot_->controllers[index]
             : nullptr;
}

int LuaSkinLegacyInputHost::keyCode(std::string_view name) noexcept {
  const auto found = std::ranges::find(kKeyNames, name, &KeyName::name);
  return found != kKeyNames.end() ? found->code : -1;
}

} // namespace skin
