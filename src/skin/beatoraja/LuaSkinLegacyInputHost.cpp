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

// Public integer fields from the pinned LibGDX Input.Keys class, including aliases.
constexpr auto kKeyConstants = std::to_array<KeyName>({
    KeyName{-1, "ANY_KEY"},
    KeyName{7, "NUM_0"},
    KeyName{8, "NUM_1"},
    KeyName{9, "NUM_2"},
    KeyName{10, "NUM_3"},
    KeyName{11, "NUM_4"},
    KeyName{12, "NUM_5"},
    KeyName{13, "NUM_6"},
    KeyName{14, "NUM_7"},
    KeyName{15, "NUM_8"},
    KeyName{16, "NUM_9"},
    KeyName{29, "A"},
    KeyName{57, "ALT_LEFT"},
    KeyName{58, "ALT_RIGHT"},
    KeyName{75, "APOSTROPHE"},
    KeyName{77, "AT"},
    KeyName{30, "B"},
    KeyName{4, "BACK"},
    KeyName{73, "BACKSLASH"},
    KeyName{31, "C"},
    KeyName{5, "CALL"},
    KeyName{27, "CAMERA"},
    KeyName{28, "CLEAR"},
    KeyName{55, "COMMA"},
    KeyName{32, "D"},
    KeyName{67, "DEL"},
    KeyName{67, "BACKSPACE"},
    KeyName{112, "FORWARD_DEL"},
    KeyName{23, "DPAD_CENTER"},
    KeyName{20, "DPAD_DOWN"},
    KeyName{21, "DPAD_LEFT"},
    KeyName{22, "DPAD_RIGHT"},
    KeyName{19, "DPAD_UP"},
    KeyName{23, "CENTER"},
    KeyName{20, "DOWN"},
    KeyName{21, "LEFT"},
    KeyName{22, "RIGHT"},
    KeyName{19, "UP"},
    KeyName{33, "E"},
    KeyName{6, "ENDCALL"},
    KeyName{66, "ENTER"},
    KeyName{65, "ENVELOPE"},
    KeyName{70, "EQUALS"},
    KeyName{64, "EXPLORER"},
    KeyName{34, "F"},
    KeyName{80, "FOCUS"},
    KeyName{35, "G"},
    KeyName{68, "GRAVE"},
    KeyName{36, "H"},
    KeyName{79, "HEADSETHOOK"},
    KeyName{3, "HOME"},
    KeyName{37, "I"},
    KeyName{38, "J"},
    KeyName{39, "K"},
    KeyName{40, "L"},
    KeyName{71, "LEFT_BRACKET"},
    KeyName{41, "M"},
    KeyName{90, "MEDIA_FAST_FORWARD"},
    KeyName{87, "MEDIA_NEXT"},
    KeyName{85, "MEDIA_PLAY_PAUSE"},
    KeyName{88, "MEDIA_PREVIOUS"},
    KeyName{89, "MEDIA_REWIND"},
    KeyName{86, "MEDIA_STOP"},
    KeyName{82, "MENU"},
    KeyName{69, "MINUS"},
    KeyName{91, "MUTE"},
    KeyName{42, "N"},
    KeyName{83, "NOTIFICATION"},
    KeyName{78, "NUM"},
    KeyName{43, "O"},
    KeyName{44, "P"},
    KeyName{56, "PERIOD"},
    KeyName{81, "PLUS"},
    KeyName{18, "POUND"},
    KeyName{26, "POWER"},
    KeyName{45, "Q"},
    KeyName{46, "R"},
    KeyName{72, "RIGHT_BRACKET"},
    KeyName{47, "S"},
    KeyName{84, "SEARCH"},
    KeyName{74, "SEMICOLON"},
    KeyName{59, "SHIFT_LEFT"},
    KeyName{60, "SHIFT_RIGHT"},
    KeyName{76, "SLASH"},
    KeyName{1, "SOFT_LEFT"},
    KeyName{2, "SOFT_RIGHT"},
    KeyName{62, "SPACE"},
    KeyName{17, "STAR"},
    KeyName{63, "SYM"},
    KeyName{48, "T"},
    KeyName{61, "TAB"},
    KeyName{49, "U"},
    KeyName{0, "UNKNOWN"},
    KeyName{50, "V"},
    KeyName{25, "VOLUME_DOWN"},
    KeyName{24, "VOLUME_UP"},
    KeyName{51, "W"},
    KeyName{52, "X"},
    KeyName{53, "Y"},
    KeyName{54, "Z"},
    KeyName{16, "META_ALT_LEFT_ON"},
    KeyName{2, "META_ALT_ON"},
    KeyName{32, "META_ALT_RIGHT_ON"},
    KeyName{64, "META_SHIFT_LEFT_ON"},
    KeyName{1, "META_SHIFT_ON"},
    KeyName{128, "META_SHIFT_RIGHT_ON"},
    KeyName{4, "META_SYM_ON"},
    KeyName{129, "CONTROL_LEFT"},
    KeyName{130, "CONTROL_RIGHT"},
    KeyName{131, "ESCAPE"},
    KeyName{132, "END"},
    KeyName{133, "INSERT"},
    KeyName{92, "PAGE_UP"},
    KeyName{93, "PAGE_DOWN"},
    KeyName{94, "PICTSYMBOLS"},
    KeyName{95, "SWITCH_CHARSET"},
    KeyName{255, "BUTTON_CIRCLE"},
    KeyName{96, "BUTTON_A"},
    KeyName{97, "BUTTON_B"},
    KeyName{98, "BUTTON_C"},
    KeyName{99, "BUTTON_X"},
    KeyName{100, "BUTTON_Y"},
    KeyName{101, "BUTTON_Z"},
    KeyName{102, "BUTTON_L1"},
    KeyName{103, "BUTTON_R1"},
    KeyName{104, "BUTTON_L2"},
    KeyName{105, "BUTTON_R2"},
    KeyName{106, "BUTTON_THUMBL"},
    KeyName{107, "BUTTON_THUMBR"},
    KeyName{108, "BUTTON_START"},
    KeyName{109, "BUTTON_SELECT"},
    KeyName{110, "BUTTON_MODE"},
    KeyName{144, "NUMPAD_0"},
    KeyName{145, "NUMPAD_1"},
    KeyName{146, "NUMPAD_2"},
    KeyName{147, "NUMPAD_3"},
    KeyName{148, "NUMPAD_4"},
    KeyName{149, "NUMPAD_5"},
    KeyName{150, "NUMPAD_6"},
    KeyName{151, "NUMPAD_7"},
    KeyName{152, "NUMPAD_8"},
    KeyName{153, "NUMPAD_9"},
    KeyName{243, "COLON"},
    KeyName{244, "F1"},
    KeyName{245, "F2"},
    KeyName{246, "F3"},
    KeyName{247, "F4"},
    KeyName{248, "F5"},
    KeyName{249, "F6"},
    KeyName{250, "F7"},
    KeyName{251, "F8"},
    KeyName{252, "F9"},
    KeyName{253, "F10"},
    KeyName{254, "F11"},
    KeyName{255, "F12"},
});

bool equalConstantName(std::string_view value, std::string_view constant) noexcept {
  if (value.size() != constant.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const char c = value[i];
    if ((c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c) != constant[i]) return false;
  }
  return true;
}

} // namespace

LuaSkinLegacyInputHost::LuaSkinLegacyInputHost()
    : LuaSkinLegacyInputHost(LuaSkinLegacyInputSnapshot{}) {}

LuaSkinLegacyInputHost::LuaSkinLegacyInputHost(
    LuaSkinLegacyInputSnapshot snapshot) {
  (void)publish(std::move(snapshot));
}

bool LuaSkinLegacyInputHost::publish(
    LuaSkinLegacyInputSnapshot snapshot) noexcept {
  LuaSkinLegacyInputGeneration generation;
  generation.sequence = generation_.sequence + 1;
  generation.drawableWidth = std::max(0, snapshot.drawableWidth);
  generation.drawableHeight = std::max(0, snapshot.drawableHeight);
  generation.anyKeyPressed = snapshot.anyKeyPressed;
  generation.pressedMouseButtons = snapshot.pressedMouseButtons;
  generation.pointerX = snapshot.pointerX;
  generation.pointerY = snapshot.pointerY;
  generation.deltaX = snapshot.deltaX;
  generation.deltaY = snapshot.deltaY;
  generation.justTouched = snapshot.justTouched;
  for (const int key : snapshot.justPressedKeys) {
    if (key >= 0 && static_cast<std::size_t>(key) < generation.justPressedGdxKeys.size()) {
      generation.justPressedGdxKeys.set(static_cast<std::size_t>(key));
    }
  }
  for (const int key : snapshot.pressedKeys) {
    if (key >= 0 &&
        static_cast<std::size_t>(key) < generation.pressedGdxKeys.size()) {
      generation.pressedGdxKeys.set(static_cast<std::size_t>(key));
      generation.anyKeyPressed = true;
    }
  }
  generation.controllerCount = std::min(
      snapshot.controllers.size(), input::kLegacyInputMaximumControllers);
  for (std::size_t index = 0; index < generation.controllerCount; ++index) {
    generation.controllers[index].setName(snapshot.controllers[index].name);
    for (const int button : snapshot.controllers[index].pressedButtons) {
      if (button >= 0 &&
          static_cast<std::size_t>(button) <
              generation.controllers[index].pressedButtons.size()) {
        generation.controllers[index].pressedButtons.set(
            static_cast<std::size_t>(button));
      }
    }
  }
  publish(std::move(generation));
  return true;
}

void LuaSkinLegacyInputHost::publish(
    LuaSkinLegacyInputGeneration generation) noexcept {
  generation.drawableWidth = std::max(0, generation.drawableWidth);
  generation.drawableHeight = std::max(0, generation.drawableHeight);
  generation.controllerCount = std::min(
      generation.controllerCount, input::kLegacyInputMaximumControllers);
  generation.anyKeyPressed =
      generation.anyKeyPressed || generation.pressedGdxKeys.any();
  generation_ = std::move(generation);
}

int LuaSkinLegacyInputHost::drawableWidth() const noexcept {
  return generation_.drawableWidth;
}

int LuaSkinLegacyInputHost::drawableHeight() const noexcept {
  return generation_.drawableHeight;
}

bool LuaSkinLegacyInputHost::isKeyPressed(int gdxKeyCode) const noexcept {
  if (gdxKeyCode == -1) {
    return generation_.anyKeyPressed;
  }
  return gdxKeyCode >= 0 &&
         static_cast<std::size_t>(gdxKeyCode) <
             generation_.pressedGdxKeys.size() &&
         generation_.pressedGdxKeys.test(static_cast<std::size_t>(gdxKeyCode));
}

std::size_t LuaSkinLegacyInputHost::controllerCount() const noexcept {
  return generation_.controllerCount;
}

std::string_view
LuaSkinLegacyInputHost::controllerName(std::size_t index) const noexcept {
  return index < generation_.controllerCount
             ? generation_.controllers[index].name()
             : std::string_view{};
}

bool LuaSkinLegacyInputHost::controllerButtonPressed(
    std::size_t index, int button) const noexcept {
  return index < generation_.controllerCount && button >= 0 &&
         static_cast<std::size_t>(button) <
             generation_.controllers[index].pressedButtons.size() &&
         generation_.controllers[index].pressedButtons.test(
             static_cast<std::size_t>(button));
}

int LuaSkinLegacyInputHost::keyCode(std::string_view name) noexcept {
  const auto found = std::ranges::find(kKeyNames, name, &KeyName::name);
  return found != kKeyNames.end() ? found->code : -1;
}

bool LuaSkinLegacyInputHost::isKeyJustPressed(int key) const noexcept {
  return key >= 0 && static_cast<std::size_t>(key) < generation_.justPressedGdxKeys.size() &&
         generation_.justPressedGdxKeys.test(static_cast<std::size_t>(key));
}
bool LuaSkinLegacyInputHost::isButtonPressed(int button) const noexcept {
  return button >= 0 && static_cast<std::size_t>(button) < generation_.pressedMouseButtons.size() &&
         generation_.pressedMouseButtons.test(static_cast<std::size_t>(button));
}
int LuaSkinLegacyInputHost::getX() const noexcept { return generation_.pointerX; }
int LuaSkinLegacyInputHost::getY() const noexcept { return generation_.pointerY; }
int LuaSkinLegacyInputHost::getDeltaX() const noexcept { return generation_.deltaX; }
int LuaSkinLegacyInputHost::getDeltaY() const noexcept { return generation_.deltaY; }
bool LuaSkinLegacyInputHost::justTouched() const noexcept { return generation_.justTouched; }
bool LuaSkinLegacyInputHost::hasCurrentPressEdge() const noexcept {
  return justTouched() || isKeyJustPressed(66) || isKeyJustPressed(62);
}
std::optional<int> LuaSkinLegacyInputHost::keyConstant(std::string_view name) noexcept {
  const auto field = std::ranges::find_if(kKeyConstants, [name](const auto &entry) {
    return equalConstantName(name, entry.name);
  });
  if (field != kKeyConstants.end()) return field->code;
  const int code = keyCode(name);
  return code >= 0 ? std::optional<int>(code) : std::nullopt;
}

} // namespace skin
