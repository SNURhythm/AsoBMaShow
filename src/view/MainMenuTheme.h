#pragma once

#include "UiTheme.h"

// Keep selector colors local so imported skins and gameplay feedback retain
// their colors. The backdrop lets preview BGA show through beneath crisp text.
namespace main_menu_theme {

inline constexpr const char *font = "assets/fonts/roboto-condensed-regular.ttf";
inline constexpr const char *boldFont = "assets/fonts/roboto-condensed-bold.ttf";

inline bool light() { return ui_theme::activeMode() == ui_theme::ThemeMode::Light; }
inline Color canvas() { return light() ? Color(247, 249, 252, 255) : Color(16, 19, 26, 255); }
inline Color backdrop() { auto color = canvas(); color.a = light() ? 232 : 200; return color; }
inline Color control() { auto color = canvas(); color.a = 64; return color; }
inline Color surface() { return light() ? Color(255, 255, 255, 96) : Color(37, 45, 61, 80); }
inline Color hover() { return light() ? Color(226, 232, 246, 160) : Color(57, 70, 96, 128); }
inline Color pressed() { return light() ? Color(211, 221, 244, 192) : Color(69, 86, 122, 160); }
inline Color ink() { return light() ? Color(27, 36, 54, 255) : Color(232, 237, 247, 255); }
inline Color secondary() { return light() ? Color(78, 93, 117, 255) : Color(167, 181, 204, 255); }
inline Color muted() { return light() ? Color(104, 119, 143, 255) : Color(132, 150, 177, 255); }
// Separate quiet row rhythm from section rules and workspace boundaries.
// Alpha lets dividers retain their hierarchy over preview artwork as well.
inline Color ruleSubtle() { return light() ? Color(102, 122, 153, 64) : Color(116, 139, 176, 48); }
inline Color rule() { return light() ? Color(142, 158, 183, 160) : Color(92, 113, 145, 112); }
inline Color ruleStrong() { return light() ? Color(109, 131, 163, 192) : Color(108, 130, 163, 160); }
inline Color accent() { return light() ? Color(66, 87, 214, 255) : Color(157, 176, 255, 255); }
inline Color action() { return light() ? Color(67, 88, 218, 255) : Color(79, 103, 230, 255); }
inline Color actionHover() { return light() ? Color(56, 74, 193, 255) : Color(67, 88, 218, 255); }
inline Color actionPressed() { return Color(48, 63, 164, 255); }
inline Color onAction() { return Color(255, 255, 255, 255); }
inline Color selected() { return light() ? Color(27, 36, 54, 255) : Color(232, 237, 247, 255); }
inline Color onSelected() { return light() ? Color(245, 248, 255, 255) : Color(27, 36, 54, 255); }
inline Color selectedSecondary() { return light() ? Color(184, 199, 223, 255) : Color(67, 83, 109, 255); }
inline Color selectedAccent() { return light() ? Color(167, 185, 255, 255) : Color(66, 87, 214, 255); }
inline Color warning() { return light() ? Color(132, 84, 14, 255) : Color(241, 195, 108, 255); }
inline Color error() { return light() ? Color(171, 38, 55, 255) : Color(255, 148, 159, 255); }
inline Color dock() { return Color(16, 22, 34, 112); }
inline Color dockHover() { return Color(38, 49, 69, 144); }
inline Color dockPressed() { return Color(49, 64, 91, 160); }
inline Color dockText() { return Color(232, 237, 247, 255); }
inline Color dockSecondary() { return Color(167, 181, 204, 255); }

} // namespace main_menu_theme
