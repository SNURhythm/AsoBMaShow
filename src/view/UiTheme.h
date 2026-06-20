#pragma once

#include "../rendering/Color.h"
#include <SDL2/SDL.h>

namespace ui_theme {

enum class ThemeMode { Dark, Light };

struct Palette {
  Color backdropTop;
  Color backdropBottom;
  Color glassTop;
  Color glassBottom;
  Color glassStrongTop;
  Color glassStrongBottom;
  Color surfaceTint;
  Color fieldInk;
  Color fieldTeal;
  Color cyan;
  Color coral;
  Color lime;
  Color amber;
  Color textPrimary;
  Color textSecondary;
  Color textMuted;
  Color darkText;
  Color hairline;
};

inline const Palette &darkPalette() {
  static const Palette palette{
      Color(7, 20, 34, 236),    Color(39, 19, 42, 236),
      Color(236, 253, 255, 58), Color(39, 186, 180, 76),
      Color(238, 255, 252, 82), Color(49, 205, 186, 112),
      Color(19, 72, 82, 154),   Color(5, 9, 16, 220),
      Color(22, 111, 114, 170), Color(70, 230, 224, 255),
      Color(255, 104, 88, 255), Color(183, 246, 92, 255),
      Color(255, 204, 81, 255), Color(248, 253, 255, 255),
      Color(198, 226, 230, 255), Color(141, 181, 188, 255),
      Color(8, 18, 24, 255),    Color(157, 252, 241, 178)};
  return palette;
}

inline const Palette &lightPalette() {
  static const Palette palette{
      Color(235, 250, 251, 255), Color(255, 239, 226, 255),
      Color(255, 255, 255, 212), Color(212, 250, 245, 206),
      Color(255, 255, 255, 238), Color(225, 255, 247, 228),
      Color(202, 245, 239, 210), Color(220, 245, 244, 230),
      Color(183, 238, 232, 210), Color(0, 166, 177, 255),
      Color(238, 76, 66, 255),   Color(97, 176, 34, 255),
      Color(222, 143, 34, 255),  Color(8, 25, 32, 255),
      Color(52, 88, 96, 255),    Color(104, 138, 145, 255),
      Color(8, 18, 24, 255),     Color(0, 174, 174, 160)};
  return palette;
}

inline ThemeMode &activeModeStorage() {
  static ThemeMode mode = ThemeMode::Dark;
  return mode;
}

inline void setActiveMode(ThemeMode mode) { activeModeStorage() = mode; }

inline ThemeMode activeMode() { return activeModeStorage(); }

inline const Palette &activePalette() {
  return activeMode() == ThemeMode::Light ? lightPalette() : darkPalette();
}

inline Color backdropTop() { return activePalette().backdropTop; }
inline Color backdropBottom() { return activePalette().backdropBottom; }
inline Color glassTop() { return activePalette().glassTop; }
inline Color glassBottom() { return activePalette().glassBottom; }
inline Color glassStrongTop() { return activePalette().glassStrongTop; }
inline Color glassStrongBottom() { return activePalette().glassStrongBottom; }
inline Color surfaceTint() { return activePalette().surfaceTint; }
inline Color fieldInk() { return activePalette().fieldInk; }
inline Color fieldTeal() { return activePalette().fieldTeal; }
inline Color cyan() { return activePalette().cyan; }
inline Color coral() { return activePalette().coral; }
inline Color lime() { return activePalette().lime; }
inline Color amber() { return activePalette().amber; }
inline Color textPrimary() { return activePalette().textPrimary; }
inline Color textSecondary() { return activePalette().textSecondary; }
inline Color textMuted() { return activePalette().textMuted; }
inline Color darkText() { return activePalette().darkText; }
inline Color hairline() { return activePalette().hairline; }

inline SDL_Color sdl(const Color &color) {
  return SDL_Color{color.r, color.g, color.b, color.a};
}

} // namespace ui_theme
