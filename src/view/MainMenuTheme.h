#pragma once

#include "UiTheme.h"

// Keep selector colors local so imported skins and gameplay feedback retain
// their colors. The backdrop lets preview BGA show through beneath crisp text.
namespace main_menu_theme {

inline constexpr const char *font = "assets/fonts/roboto-condensed-regular.ttf";
inline constexpr const char *boldFont = "assets/fonts/roboto-condensed-bold.ttf";

inline bool light() { return ui_theme::activeMode() == ui_theme::ThemeMode::Light; }
inline Color canvas() { return light() ? Color(249, 248, 243, 255) : Color(18, 20, 24, 255); }
inline Color backdrop() { auto color = canvas(); color.a = 200; return color; }
inline Color control() { auto color = canvas(); color.a = 64; return color; }
inline Color surface() { return light() ? Color(242, 241, 237, 64) : Color(27, 30, 36, 64); }
inline Color hover() { return light() ? Color(223, 225, 230, 96) : Color(40, 45, 55, 96); }
inline Color pressed() { return light() ? Color(209, 215, 228, 128) : Color(49, 57, 72, 128); }
inline Color ink() { return light() ? Color(25, 27, 32, 255) : Color(238, 237, 232, 255); }
inline Color secondary() { return light() ? Color(84, 87, 94, 255) : Color(173, 178, 188, 255); }
inline Color muted() { return light() ? Color(104, 107, 114, 255) : Color(148, 155, 167, 255); }
inline Color rule() { return light() ? Color(198, 198, 194, 255) : Color(54, 59, 69, 255); }
inline Color accent() { return light() ? Color(0, 80, 255, 255) : Color(143, 171, 255, 255); }
inline Color action() { return light() ? Color(0, 80, 255, 255) : Color(0, 80, 255, 255); }
inline Color actionHover() { return light() ? Color(16, 66, 194, 255) : Color(59, 105, 239, 255); }
inline Color actionPressed() { return Color(16, 55, 164, 255); }
inline Color onAction() { return Color(255, 255, 255, 255); }
inline Color selected() { return light() ? Color(25, 27, 32, 255) : Color(238, 237, 232, 255); }
inline Color onSelected() { return light() ? Color(248, 247, 242, 255) : Color(25, 27, 32, 255); }
inline Color selectedSecondary() { return light() ? Color(190, 195, 205, 255) : Color(76, 80, 89, 255); }
inline Color selectedAccent() { return light() ? Color(159, 186, 255, 255) : Color(0, 80, 255, 255); }
inline Color warning() { return light() ? Color(132, 84, 14, 255) : Color(241, 195, 108, 255); }
inline Color error() { return light() ? Color(171, 38, 55, 255) : Color(255, 148, 159, 255); }
inline Color dock() { return Color(18, 20, 24, 96); }
inline Color dockHover() { return Color(35, 39, 48, 120); }
inline Color dockPressed() { return Color(44, 50, 62, 144); }
inline Color dockText() { return Color(238, 237, 232, 255); }
inline Color dockSecondary() { return Color(173, 178, 188, 255); }

} // namespace main_menu_theme
