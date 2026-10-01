#pragma once

#include "../../view/TextView.h"
#include "../../view/UiTheme.h"

// The reminder's localized instructions use **...** for setting targets.
// Separate TextViews keep font weight and theme color scoped to those targets.
class GuidedAccessInstructionView : public View {
public:
  GuidedAccessInstructionView() {
    setFlexDirection(FlexDirection::Column);
    setJustifyContent(YGJustifyCenter);
  }

  void setLocalizedText(const i18n::Text &message) {
    message_ = message;
    rebuild();
  }

protected:
  void onLanguageChanged() override { rebuild(); }

private:
  i18n::Text message_;
  std::string resolved_;

  void rebuild() {
    const auto resolved = message_.resolve();
    if (resolved == resolved_) return;
    resolved_ = resolved;
    clearChildren();
    size_t lineStart = 0;
    while (lineStart < resolved.size()) {
      const auto newline = resolved.find('\n', lineStart);
      const auto line = resolved.substr(lineStart, newline - lineStart);
      auto *row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setFlexWrap(YGWrapWrap);
      row->setJustifyContent(YGJustifyCenter);
      row->setAlignItems(YGAlignCenter);
      row->setMinHeight(44);
      addView(row);
      size_t start = 0;
      bool emphasized = false;
      while (start < line.size()) {
        const auto marker = line.find("**", start);
        const auto text = line.substr(start, marker - start);
        if (!text.empty()) {
          auto *run = new TextView("assets/fonts/notosanscjkjp.ttf", 22,
              emphasized ? TextView::FontWeight::Bold : TextView::FontWeight::Regular);
          run->setText(text);
          run->setVAlign(TextView::MIDDLE);
          run->setThemedColor(emphasized ? ui_theme::cyan : ui_theme::textSecondary);
          row->addView(run);
        }
        if (marker == std::string::npos) break;
        emphasized = !emphasized;
        start = marker + 2;
      }
      if (newline == std::string::npos) break;
      lineStart = newline + 1;
    }
  }
};
