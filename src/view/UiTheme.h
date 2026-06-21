#pragma once

#include "../rendering/Color.h"
#include <SDL2/SDL.h>
#include <algorithm>

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

struct ShadowSpec {
  int offsetX;
  int offsetY;
  int spread;
  float radiusInset;
};

inline constexpr ShadowSpec kCardShadow{0, 6, 14, 3.0f};
inline constexpr ShadowSpec kHeaderShadow{0, 5, 12, 3.0f};
inline constexpr ShadowSpec kPanelShadow{0, 8, 16, 4.0f};
inline constexpr ShadowSpec kModalShadow{0, 12, 22, 5.0f};
inline constexpr ShadowSpec kSidePanelShadow{-6, 0, 20, 5.0f};

inline const Palette &darkPalette() {
  static const Palette palette{
      Color(7, 20, 34, 236),     Color(39, 19, 42, 236),
      Color(236, 253, 255, 58),  Color(39, 186, 180, 76),
      Color(238, 255, 252, 82),  Color(49, 205, 186, 112),
      Color(19, 72, 82, 154),    Color(5, 9, 16, 220),
      Color(18, 92, 100, 156),   Color(86, 210, 202, 255),
      Color(255, 104, 88, 255),  Color(183, 246, 92, 255),
      Color(255, 204, 81, 255),  Color(248, 253, 255, 255),
      Color(198, 226, 230, 255), Color(141, 181, 188, 255),
      Color(8, 18, 24, 255),     Color(102, 151, 156, 112)};
  return palette;
}

inline const Palette &lightPalette() {
  static const Palette palette{
      Color(235, 250, 251, 255), Color(255, 239, 226, 255),
      Color(255, 255, 255, 212), Color(212, 250, 245, 206),
      Color(255, 255, 255, 238), Color(225, 255, 247, 228),
      Color(202, 245, 239, 210), Color(220, 245, 244, 230),
      Color(183, 238, 232, 210), Color(0, 145, 156, 255),
      Color(238, 76, 66, 255),   Color(97, 176, 34, 255),
      Color(222, 143, 34, 255),  Color(8, 25, 32, 255),
      Color(52, 88, 96, 255),    Color(104, 138, 145, 255),
      Color(8, 18, 24, 255),     Color(56, 118, 126, 132)};
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

inline Color withAlpha(Color color, uint8_t alpha) {
  color.a = alpha;
  return color;
}

inline int perceivedBrightness(const Color &color) {
  return (static_cast<int>(color.r) * 299 + static_cast<int>(color.g) * 587 +
          static_cast<int>(color.b) * 114) /
         1000;
}

inline Color textOn(const Color &background) {
  return perceivedBrightness(background) > 150 ? darkText()
                                               : Color(248, 253, 255, 255);
}

inline Color hairlineSubtle() {
  return activeMode() == ThemeMode::Light ? withAlpha(hairline(), 84)
                                          : withAlpha(hairline(), 56);
}

inline Color hairlineStrong() {
  return activeMode() == ThemeMode::Light ? withAlpha(hairline(), 148)
                                          : withAlpha(hairline(), 118);
}

inline Color accentBorder() {
  return activeMode() == ThemeMode::Light ? Color(0, 145, 156, 138)
                                          : Color(86, 210, 202, 126);
}

inline Color accentBorderStrong() {
  return activeMode() == ThemeMode::Light ? Color(0, 145, 156, 192)
                                          : Color(105, 224, 216, 174);
}

inline Color backdrop() {
  return activeMode() == ThemeMode::Light ? Color(235, 239, 244, 255)
                                          : Color(7, 10, 16, 255);
}

inline Color resultBackdrop() {
  return activeMode() == ThemeMode::Light ? Color(235, 239, 244, 112)
                                          : Color(7, 10, 16, 116);
}

inline Color panel() {
  return activeMode() == ThemeMode::Light ? Color(249, 252, 253, 238)
                                          : Color(15, 22, 32, 238);
}

inline Color panelStrong() {
  return activeMode() == ThemeMode::Light ? Color(255, 255, 255, 250)
                                          : Color(19, 28, 40, 250);
}

inline Color panelSubtle() {
  return activeMode() == ThemeMode::Light ? Color(241, 246, 248, 224)
                                          : Color(11, 17, 26, 224);
}

inline Color resultPanel() {
  return activeMode() == ThemeMode::Light ? Color(249, 252, 253, 168)
                                          : Color(15, 22, 32, 152);
}

inline Color resultPanelStrong() {
  return activeMode() == ThemeMode::Light ? Color(255, 255, 255, 188)
                                          : Color(19, 28, 40, 172);
}

inline Color resultPanelSubtle() {
  return activeMode() == ThemeMode::Light ? Color(241, 246, 248, 142)
                                          : Color(11, 17, 26, 126);
}

inline Color mainMenuBackdrop() {
  return activeMode() == ThemeMode::Light ? Color(247, 252, 253, 72)
                                          : Color(5, 8, 13, 52);
}

inline Color mainMenuPanel() {
  return activeMode() == ThemeMode::Light ? Color(255, 255, 255, 150)
                                          : Color(15, 22, 32, 124);
}

inline Color mainMenuSurface() {
  return activeMode() == ThemeMode::Light ? Color(232, 241, 243, 172)
                                          : Color(24, 35, 48, 150);
}

inline Color mainMenuItem() {
  return activeMode() == ThemeMode::Light ? Color(249, 252, 253, 174)
                                          : Color(15, 22, 32, 142);
}

inline Color mainMenuItemSelected() {
  return activeMode() == ThemeMode::Light ? Color(255, 255, 255, 214)
                                          : Color(23, 34, 48, 186);
}

inline Color control() {
  return activeMode() == ThemeMode::Light ? Color(232, 241, 243, 238)
                                          : Color(24, 35, 48, 238);
}

inline Color controlHover() {
  return activeMode() == ThemeMode::Light ? Color(220, 235, 237, 246)
                                          : Color(31, 46, 62, 246);
}

inline Color controlPressed() {
  return activeMode() == ThemeMode::Light ? Color(207, 226, 229, 255)
                                          : Color(39, 58, 77, 255);
}

inline Color primaryAction() {
  return activeMode() == ThemeMode::Light ? Color(0, 105, 112, 244)
                                          : Color(18, 118, 114, 238);
}

inline Color primaryActionHover() {
  return activeMode() == ThemeMode::Light ? Color(0, 122, 130, 250)
                                          : Color(23, 139, 133, 246);
}

inline Color primaryActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(0, 91, 98, 255)
                                          : Color(17, 101, 98, 255);
}

inline Color dangerAction() {
  return activeMode() == ThemeMode::Light ? Color(164, 52, 45, 244)
                                          : Color(142, 52, 48, 238);
}

inline Color dangerActionHover() {
  return activeMode() == ThemeMode::Light ? Color(186, 61, 52, 250)
                                          : Color(164, 61, 55, 246);
}

inline Color dangerActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(139, 42, 37, 255)
                                          : Color(123, 43, 40, 255);
}

inline Color successAction() {
  return activeMode() == ThemeMode::Light ? Color(0, 112, 86, 244)
                                          : Color(32, 120, 106, 238);
}

inline Color successActionHover() {
  return activeMode() == ThemeMode::Light ? Color(0, 132, 101, 250)
                                          : Color(42, 143, 126, 246);
}

inline Color successActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(0, 96, 74, 255)
                                          : Color(30, 101, 90, 255);
}

inline Color infoAction() {
  return activeMode() == ThemeMode::Light ? Color(36, 90, 143, 244)
                                          : Color(36, 82, 126, 238);
}

inline Color infoActionHover() {
  return activeMode() == ThemeMode::Light ? Color(43, 106, 166, 250)
                                          : Color(47, 99, 149, 246);
}

inline Color infoActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(30, 76, 123, 255)
                                          : Color(33, 71, 111, 255);
}

inline Color violetAction() {
  return activeMode() == ThemeMode::Light ? Color(92, 76, 151, 244)
                                          : Color(70, 69, 124, 238);
}

inline Color violetActionHover() {
  return activeMode() == ThemeMode::Light ? Color(108, 90, 174, 250)
                                          : Color(86, 84, 148, 246);
}

inline Color violetActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(76, 62, 128, 255)
                                          : Color(61, 60, 106, 255);
}

inline Color warningAction() {
  return activeMode() == ThemeMode::Light ? Color(128, 82, 22, 244)
                                          : Color(116, 84, 34, 238);
}

inline Color warningActionHover() {
  return activeMode() == ThemeMode::Light ? Color(150, 96, 28, 250)
                                          : Color(136, 99, 42, 246);
}

inline Color warningActionPressed() {
  return activeMode() == ThemeMode::Light ? Color(108, 68, 18, 255)
                                          : Color(98, 71, 30, 255);
}

inline Color scrim() {
  return activeMode() == ThemeMode::Light ? Color(20, 31, 39, 92)
                                          : Color(0, 0, 0, 164);
}

inline Color insetSurface() {
  return activeMode() == ThemeMode::Light ? Color(244, 248, 250, 244)
                                          : Color(8, 14, 23, 230);
}

inline Color progressTrack() {
  return activeMode() == ThemeMode::Light ? Color(225, 235, 238, 246)
                                          : Color(8, 14, 23, 230);
}

inline Color progressFill() {
  return activeMode() == ThemeMode::Light ? Color(0, 121, 98, 244)
                                          : Color(62, 168, 145, 240);
}

inline Color shadow() {
  return activeMode() == ThemeMode::Light ? Color(22, 38, 50, 64)
                                          : Color(0, 0, 0, 150);
}

inline Color cardShadow() {
  return activeMode() == ThemeMode::Light ? Color(22, 38, 50, 42)
                                          : Color(0, 0, 0, 96);
}

inline float panelRadius() { return 8.0f; }
inline float controlRadius() { return 8.0f; }

inline float insetRadius(float outerRadius, float inset) {
  return std::max(0.0f, outerRadius - std::max(0.0f, inset));
}

inline float childRadiusForInset(float parentRadius, float borderWidth,
                                 float padding) {
  return insetRadius(parentRadius, borderWidth + padding);
}

inline SDL_Color sdl(const Color &color) {
  return SDL_Color{color.r, color.g, color.b, color.a};
}

} // namespace ui_theme
