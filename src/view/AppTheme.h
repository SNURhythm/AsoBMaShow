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
// Preserve gameplay feedback and rank colors.
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

// Scene and modal surfaces are opaque; only BGA-facing chrome opts into alpha.
inline Color backdrop() { return main_menu_theme::canvas(); }
inline Color backdropTop() { return backdrop(); }
inline Color backdropBottom() { return backdrop(); }
inline Color panel() { return main_menu_theme::light() ? Color(255, 255, 255, 255) : Color(24, 29, 39, 255); }
inline Color panelStrong() { return main_menu_theme::light() ? Color(255, 255, 255, 255) : Color(28, 34, 46, 255); }
inline Color panelSubtle() { return main_menu_theme::light() ? Color(238, 242, 248, 255) : Color(22, 27, 36, 255); }
// Gameplay HUD overlays remain transparent to the BGA behind them.
inline Color hudPanel() { return withAlpha(main_menu_theme::canvas(), 112); }
inline Color hudPanelStrong() { return withAlpha(main_menu_theme::canvas(), 232); }
inline Color hudPanelSubtle() { return main_menu_theme::surface(); }
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
inline Color resultPanelStrong() { return panelStrong(); }
inline Color resultPanelSubtle() { return panelSubtle(); }
inline Color mainMenuBackdrop() { return backdrop(); }
inline Color mainMenuPanel() { return panel(); }
inline Color mainMenuSurface() { return panelSubtle(); }
inline Color mainMenuItem() { return Color(0, 0, 0, 0); }
inline Color mainMenuItemSelected() { return withAlpha(action(), 64); }
inline Color control() { return main_menu_theme::control(); }
inline Color controlHover() { return main_menu_theme::hover(); }
inline Color controlPressed() { return main_menu_theme::pressed(); }
enum class ActionCategory {
  Neutral, Primary, Confirmation, Information, DataTransfer, Caution, Destructive
};

struct ActionPaletteEntry {
  Color normal;
  Color hover;
  Color pressed;
  Color label;
};

inline ActionPaletteEntry actionPalette(ActionCategory category) {
  switch (category) {
  case ActionCategory::Primary:
    return {action(), actionHover(), actionPressed(), onAction()};
  case ActionCategory::Confirmation:
    return {Color(24, 119, 83, 255), Color(18, 102, 70, 255),
            Color(14, 84, 58, 255), onAction()};
  case ActionCategory::Information:
    return {Color(20, 113, 139, 255), Color(16, 96, 121, 255),
            Color(13, 80, 102, 255), onAction()};
  case ActionCategory::DataTransfer:
    return {Color(113, 77, 190, 255), Color(96, 62, 167, 255),
            Color(80, 50, 144, 255), onAction()};
  case ActionCategory::Caution:
    return {Color(153, 91, 18, 255), Color(131, 75, 13, 255),
            Color(110, 61, 10, 255), onAction()};
  case ActionCategory::Destructive:
    return {Color(187, 57, 74, 255), Color(165, 43, 60, 255),
            Color(140, 33, 48, 255), onAction()};
  case ActionCategory::Neutral:
    return {control(), controlHover(), controlPressed(), textPrimary()};
  }
  return {control(), controlHover(), controlPressed(), textPrimary()};
}

// Compatibility accessors keep existing semantic call sites on the same palette.
inline Color primaryAction() { return actionPalette(ActionCategory::Primary).normal; }
inline Color primaryActionHover() { return actionPalette(ActionCategory::Primary).hover; }
inline Color primaryActionPressed() { return actionPalette(ActionCategory::Primary).pressed; }
inline Color successAction() { return actionPalette(ActionCategory::Confirmation).normal; }
inline Color successActionHover() { return actionPalette(ActionCategory::Confirmation).hover; }
inline Color successActionPressed() { return actionPalette(ActionCategory::Confirmation).pressed; }
inline Color infoAction() { return actionPalette(ActionCategory::Information).normal; }
inline Color infoActionHover() { return actionPalette(ActionCategory::Information).hover; }
inline Color infoActionPressed() { return actionPalette(ActionCategory::Information).pressed; }
inline Color violetAction() { return actionPalette(ActionCategory::DataTransfer).normal; }
inline Color violetActionHover() { return actionPalette(ActionCategory::DataTransfer).hover; }
inline Color violetActionPressed() { return actionPalette(ActionCategory::DataTransfer).pressed; }
inline Color warningAction() { return actionPalette(ActionCategory::Caution).normal; }
inline Color warningActionHover() { return actionPalette(ActionCategory::Caution).hover; }
inline Color warningActionPressed() { return actionPalette(ActionCategory::Caution).pressed; }
inline Color dangerAction() { return actionPalette(ActionCategory::Destructive).normal; }
inline Color dangerActionHover() { return actionPalette(ActionCategory::Destructive).hover; }
inline Color dangerActionPressed() { return actionPalette(ActionCategory::Destructive).pressed; }
inline Color progressTrack() { return control(); }
inline Color progressFill() { return action(); }
inline Color scrim() { return Color(0, 0, 0, 144); }
inline Color shadow() { return Color(0, 0, 0, 0); }
inline Color cardShadow() { return Color(0, 0, 0, 0); }
inline float panelRadius() { return 0.0f; }
inline float controlRadius() { return 0.0f; }
} // namespace app_theme
