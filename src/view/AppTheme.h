#pragma once

#include "MainMenuTheme.h"

// Application chrome and outside-lane HUD share the selector's visual language.
// Lane feedback, gauges and user skins retain their own styles.
namespace app_theme {
using ui_theme::ThemeMode;
using ui_theme::activeMode;
using ui_theme::setActiveMode;
using ui_theme::withAlpha;
using ui_theme::perceivedBrightness;
using ui_theme::sdl;
using ui_theme::childRadiusForInset;
using ui_theme::insetRadius;
// Preserve semantic warning/danger and gameplay feedback/rank colors.
using ui_theme::dangerAction;
using ui_theme::dangerActionHover;
using ui_theme::dangerActionPressed;
using ui_theme::warningAction;
using ui_theme::warningActionHover;
using ui_theme::warningActionPressed;
using ui_theme::fastFeedback;
using ui_theme::slowFeedback;
using ui_theme::scoreRankColor;
using ui_theme::kCardShadow;
using ui_theme::kHeaderShadow;
using ui_theme::kPanelShadow;
using ui_theme::kModalShadow;
using ui_theme::kSidePanelShadow;

inline constexpr const char *font = main_menu_theme::font;
inline constexpr const char *boldFont = main_menu_theme::boldFont;
using main_menu_theme::action;
using main_menu_theme::actionHover;
using main_menu_theme::actionPressed;
using main_menu_theme::onAction;

inline Color textPrimary() { return main_menu_theme::ink(); }
inline Color textSecondary() { return main_menu_theme::secondary(); }
inline Color textMuted() { return main_menu_theme::muted(); }
inline Color darkText() { return Color(27, 36, 54, 255); }
inline Color textOn(const Color &background) {
  return perceivedBrightness(background) > 150 ? darkText() : onAction();
}
inline Color cyan() { return main_menu_theme::accent(); }
inline Color coral() { return main_menu_theme::error(); }
inline Color amber() { return main_menu_theme::warning(); }
inline Color lime() {
  return activeMode() == ThemeMode::Light ? Color(35, 125, 58, 255)
                                         : Color(123, 218, 146, 255);
}
inline Color hairline() { return main_menu_theme::rule(); }
inline Color hairlineSubtle() { return main_menu_theme::ruleSubtle(); }
inline Color hairlineStrong() { return main_menu_theme::ruleStrong(); }
inline Color accentBorder() { return withAlpha(cyan(), 160); }
inline Color accentBorderStrong() { return cyan(); }

inline Color backdrop() { return main_menu_theme::backdrop(); }
inline Color backdropTop() { return backdrop(); }
inline Color backdropBottom() { return backdrop(); }
inline Color panel() { return withAlpha(main_menu_theme::canvas(), 112); }
inline Color panelStrong() { return withAlpha(main_menu_theme::canvas(), 232); }
inline Color panelSubtle() { return main_menu_theme::surface(); }
inline Color glassTop() { return panel(); }
inline Color glassBottom() { return panel(); }
inline Color glassStrongTop() { return panelStrong(); }
inline Color glassStrongBottom() { return panelStrong(); }
inline Color surfaceTint() { return panelSubtle(); }
inline Color fieldInk() { return panelSubtle(); }
inline Color fieldTeal() { return panelSubtle(); }
inline Color insetSurface() { return panelSubtle(); }
inline Color resultBackdrop() { return backdrop(); }
inline Color resultPanel() { return panel(); }
inline Color resultPanelStrong() { return withAlpha(main_menu_theme::canvas(), 160); }
inline Color resultPanelSubtle() { return panelSubtle(); }
inline Color mainMenuBackdrop() { return backdrop(); }
inline Color mainMenuPanel() { return panel(); }
inline Color mainMenuSurface() { return panelSubtle(); }
inline Color mainMenuItem() { return Color(0, 0, 0, 0); }
inline Color mainMenuItemSelected() { return withAlpha(action(), 64); }
inline Color control() { return main_menu_theme::control(); }
inline Color controlHover() { return main_menu_theme::hover(); }
inline Color controlPressed() { return main_menu_theme::pressed(); }
inline Color primaryAction() { return action(); }
inline Color primaryActionHover() { return actionHover(); }
inline Color primaryActionPressed() { return actionPressed(); }
inline Color successAction() { return action(); }
inline Color successActionHover() { return actionHover(); }
inline Color successActionPressed() { return actionPressed(); }
inline Color infoAction() { return action(); }
inline Color infoActionHover() { return actionHover(); }
inline Color infoActionPressed() { return actionPressed(); }
inline Color violetAction() { return action(); }
inline Color violetActionHover() { return actionHover(); }
inline Color violetActionPressed() { return actionPressed(); }
inline Color progressTrack() { return control(); }
inline Color progressFill() { return action(); }
inline Color scrim() { return Color(0, 0, 0, 144); }
inline Color shadow() { return Color(0, 0, 0, 0); }
inline Color cardShadow() { return Color(0, 0, 0, 0); }
inline float panelRadius() { return 0.0f; }
inline float controlRadius() { return 0.0f; }
} // namespace app_theme
