#include "PlayOptionSectionView.h"

#include "../PlayOptionUtils.h"
#include "Button.h"
#include "TextInputBox.h"
#include "TextView.h"
#include "UiTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr const char *kFont = "assets/fonts/notosanscjkjp.ttf";

TextView *makeText(std::string text, int size) {
  auto *view = new TextView(kFont, size);
  view->setText(std::move(text));
  view->setThemedColor(ui_theme::textPrimary);
  view->setAlign(TextView::CENTER);
  view->setVAlign(TextView::MIDDLE);
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

Button *makeButton(const std::string &label, int fontSize, TextView **textOut) {
  auto *button = new Button(0, 0, 0, 48);
  button->setFlexGrow(1.0f);
  button->setFlexBasis(0.0f);
  button->setFlexShrink(1.0f);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                ui_theme::hairlineStrong,
                                ui_theme::accentBorder);
  auto *text = makeText(label, fontSize);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

void styleButton(Button *button, TextView *text, bool selected, bool enabled) {
  if (button == nullptr || text == nullptr) {
    return;
  }
  button->setEnabled(enabled);
  if (!enabled) {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
    return;
  }
  if (selected) {
    button->setThemedBackgroundColors(ui_theme::primaryAction,
                                      ui_theme::primaryActionHover,
                                      ui_theme::primaryActionPressed);
    button->setThemedBorderColors(ui_theme::accentBorderStrong,
                                  ui_theme::accentBorderStrong,
                                  ui_theme::accentBorderStrong);
    text->setThemedColor(
        [] { return ui_theme::textOn(ui_theme::primaryAction()); });
    return;
  }
  button->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                ui_theme::hairlineStrong,
                                ui_theme::accentBorder);
  text->setThemedColor(ui_theme::textPrimary);
}
} // namespace

PlayOptionSectionView::PlayOptionSectionView(
    PlayOptionSectionCallbacks callbacks, PlayOptionSectionLayout layout)
    : callbacks(std::move(callbacks)) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(10);

  auto *heading = makeText("Play Option", 20);
  heading->setThemedColor(ui_theme::textSecondary);
  heading->setAlign(TextView::LEFT);
  heading->setHeight(28);
  addView(heading);

  const int columns = std::max(1, layout.columns);
  View *row = nullptr;
  for (size_t i = 0; i < play_options::kPlayOptions.size(); ++i) {
    if (i % static_cast<size_t>(columns) == 0) {
      row = new View();
      row->setFlexDirection(FlexDirection::Row);
      row->setAlignItems(YGAlignStretch);
      row->setGap(10);
      row->setHeight(layout.rowHeight);
      addView(row);
    }
    TextView *text = nullptr;
    const std::string option = play_options::kPlayOptions[i];
    auto *button = makeButton(option, layout.buttonFontSize, &text);
    button->setOnClickListener([this, option]() {
      if (this->callbacks.onOptionSelected) {
        this->callbacks.onOptionSelected(option);
      }
    });
    optionButtons.push_back({button, text, option});
    row->addView(button);
  }

  if (!layout.showLaneOrder) {
    return;
  }

  auto *laneRow = new View();
  laneRow->setFlexDirection(FlexDirection::Row);
  laneRow->setAlignItems(YGAlignCenter);
  laneRow->setGap(10);
  laneRow->setHeight(52);

  auto *laneLabel = makeText("Lane Order", 17);
  laneLabel->setThemedColor(ui_theme::textSecondary);
  laneLabel->setAlign(TextView::LEFT);
  laneLabel->setWidth(90);
  laneLabel->setFlexShrink(0.0f);
  laneLabel->setHeight(46);
  laneRow->addView(laneLabel);

  laneOrderInput = new TextInputBox(kFont, 18);
  laneOrderInput->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  laneOrderInput->setBackgroundColor(ui_theme::control());
  laneOrderInput->setCornerRadius(ui_theme::controlRadius());
  laneOrderInput->setBorderColor(ui_theme::hairline());
  laneOrderInput->setBorderWidth(1);
  laneOrderInput->setPadding(Edge::Left, 12);
  laneOrderInput->setPadding(Edge::Right, 12);
  laneOrderInput->setVAlign(TextView::MIDDLE);
  laneOrderInput->setOverflow(TextView::TextOverflow::Hidden);
  laneOrderInput->setFlexGrow(1.0f);
  laneOrderInput->setFlexBasis(0.0f);
  laneOrderInput->setFlexShrink(1.0f);
  laneOrderInput->setHeight(46);
  laneOrderInput->onSubmit([this](const std::string &text) {
    if (this->callbacks.onLaneOrderSubmitted) {
      this->callbacks.onLaneOrderSubmitted(text);
    }
  });
  laneRow->addView(laneOrderInput);
  addView(laneRow);

  auto *laneActionRow = new View();
  laneActionRow->setFlexDirection(FlexDirection::Row);
  laneActionRow->setJustifyContent(YGJustifyFlexEnd);
  laneActionRow->setAlignItems(YGAlignStretch);
  laneActionRow->setGap(10);
  laneActionRow->setHeight(46);

  TextView *applyText = nullptr;
  applyLaneOrderButton = makeButton("Apply", 17, &applyText);
  applyLaneOrderButton->setFlexGrow(0.0f);
  applyLaneOrderButton->setFlexBasis(96.0f);
  applyLaneOrderButton->setFlexShrink(0.0f);
  applyLaneOrderButton->setOnClickListener([this]() {
    if (this->callbacks.onLaneOrderSubmitted && laneOrderInput != nullptr) {
      this->callbacks.onLaneOrderSubmitted(laneOrderInput->getText());
    }
  });
  laneActionRow->addView(applyLaneOrderButton);

  TextView *resetText = nullptr;
  resetLaneOrderButton = makeButton("Reset", 17, &resetText);
  resetLaneOrderButton->setFlexGrow(0.0f);
  resetLaneOrderButton->setFlexBasis(96.0f);
  resetLaneOrderButton->setFlexShrink(0.0f);
  resetLaneOrderButton->setOnClickListener([this]() {
    if (this->callbacks.onOptionSelected) {
      this->callbacks.onOptionSelected("NORMAL");
    }
  });
  laneActionRow->addView(resetLaneOrderButton);
  addView(laneActionRow);

  laneOrderMessage = makeText("", 15);
  laneOrderMessage->setThemedColor(ui_theme::textMuted);
  laneOrderMessage->setAlign(TextView::LEFT);
  laneOrderMessage->setHeight(24);
  addView(laneOrderMessage);
}

void PlayOptionSectionView::refresh(const std::string &selectedOption,
                                    const std::string &defaultLaneOrder,
                                    bool laneOrderEnabled) {
  const std::string normalized =
      play_options::normalizePlayOption(selectedOption);
  for (auto &item : optionButtons) {
    const bool allowed =
        !callbacks.isOptionAllowed || callbacks.isOptionAllowed(item.option);
    styleButton(item.button, item.text, normalized == item.option, allowed);
  }

  if (laneOrderInput != nullptr) {
    const auto assigned =
        play_options::laneAssignNotationFromOption(selectedOption);
    laneOrderInput->setEditingText(assigned.value_or(defaultLaneOrder));
  }
  if (applyLaneOrderButton != nullptr) {
    applyLaneOrderButton->setEnabled(laneOrderEnabled);
  }
  if (resetLaneOrderButton != nullptr) {
    resetLaneOrderButton->setEnabled(true);
  }
  if (!laneOrderEnabled) {
    setLaneOrderMessage("Lane order is unavailable for this selection.");
  } else if (laneOrderMessage != nullptr) {
    laneOrderMessage->setText("");
  }
}

void PlayOptionSectionView::setLaneOrderMessage(std::string message,
                                                bool error) {
  if (laneOrderMessage == nullptr) {
    return;
  }
  laneOrderMessage->setText(std::move(message));
  laneOrderMessage->setColor(
      ui_theme::sdl(error ? ui_theme::coral() : ui_theme::textMuted()));
}
