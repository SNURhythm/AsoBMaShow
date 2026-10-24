#include "../i18n/Localization.h"
#include "PlayOptionSectionView.h"

#include "../PlayOptionUtils.h"
#include "Button.h"
#include "ChoiceControlView.h"
#include "TextInputBox.h"
#include "TextView.h"
#include "AppTheme.h"

#include <utility>

namespace {
constexpr const char *kFont = app_theme::font;

TextView *makeText(i18n::Text text, int size) {
  auto *view = new TextView(kFont, size);
  view->setLocalizedText(std::move(text));
  view->setThemedColor(app_theme::textPrimary);
  view->setAlign(TextView::CENTER);
  view->setVAlign(TextView::MIDDLE);
  view->setOverflow(TextView::TextOverflow::Hidden);
  return view;
}

Button *makeButton(const i18n::Text &label, int fontSize, TextView **textOut) {
  auto *button = new Button(0, 0, 0, 48);
  button->setFlexGrow(1.0f);
  button->setFlexBasis(0.0f);
  button->setFlexShrink(1.0f);
  button->setCornerRadius(app_theme::controlRadius());
  button->setStyledBorderWidth(1);
  button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                    app_theme::controlPressed);
  button->setThemedBorderColors(app_theme::hairlineSubtle,
                                app_theme::hairlineStrong,
                                app_theme::accentBorder);
  auto *text = makeText(label, fontSize);
  button->setContentView(text);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

} // namespace

PlayOptionSectionView::PlayOptionSectionView(
    PlayOptionSectionCallbacks callbacks, PlayOptionSectionLayout layout,
    OverlayPortal *overlayPortal)
    : callbacks(std::move(callbacks)) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(10);

  if (layout.showHeading) {
    auto *heading = makeText(i18n::message("play_options.section.play_option.label"), 20);
    heading->setThemedColor(app_theme::textSecondary);
    heading->setAlign(TextView::LEFT);
    heading->setHeight(28);
    addView(heading);
  }

  optionChoice = new ChoiceControlView([this](const std::string &option) {
    if (this->callbacks.isOptionAllowed && !this->callbacks.isOptionAllowed(option)) return false;
    if (this->callbacks.onOptionSelected) this->callbacks.onOptionSelected(option);
    return true;
  }, overlayPortal, layout.buttonFontSize, layout.rowHeight);
  optionChoice->setName("play-option-choice");
  addView(optionChoice);

  if (!layout.showLaneOrder) {
    return;
  }

  auto *laneRow = new View();
  laneRow->setFlexDirection(FlexDirection::Row);
  laneRow->setAlignItems(YGAlignCenter);
  laneRow->setGap(10);
  laneRow->setHeight(52);

  auto *laneLabel = makeText(i18n::message("play_options.section.lane_order.label"), 17);
  laneLabel->setThemedColor(app_theme::textSecondary);
  laneLabel->setAlign(TextView::LEFT);
  laneLabel->setWidth(90);
  laneLabel->setFlexShrink(0.0f);
  laneLabel->setHeight(46);
  laneRow->addView(laneLabel);

  laneOrderInput = new TextInputBox(kFont, 18);
  laneOrderInput->setColor(app_theme::sdl(app_theme::textPrimary()));
  laneOrderInput->setBackgroundColor(app_theme::control());
  laneOrderInput->setCornerRadius(app_theme::controlRadius());
  laneOrderInput->setBorderColor(app_theme::hairline());
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
  applyLaneOrderButton = makeButton(i18n::message("play_options.section.apply.label"), 17, &applyText);
  applyLaneOrderButton->setFlexGrow(0.0f);
  applyLaneOrderButton->setWidth(96.0f);
  applyLaneOrderButton->setFlexShrink(0.0f);
  applyLaneOrderButton->setOnClickListener([this]() {
    if (this->callbacks.onLaneOrderSubmitted && laneOrderInput != nullptr) {
      this->callbacks.onLaneOrderSubmitted(laneOrderInput->getText());
    }
  });
  laneActionRow->addView(applyLaneOrderButton);

  TextView *resetText = nullptr;
  resetLaneOrderButton = makeButton(i18n::message("play_options.section.reset.label"), 17, &resetText);
  resetLaneOrderButton->setFlexGrow(0.0f);
  resetLaneOrderButton->setWidth(96.0f);
  resetLaneOrderButton->setFlexShrink(0.0f);
  resetLaneOrderButton->setOnClickListener([this]() {
    resetLaneOrderDraft();
    if (this->callbacks.onOptionSelected) {
      this->callbacks.onOptionSelected("NORMAL");
    }
  });
  laneActionRow->addView(resetLaneOrderButton);
  addView(laneActionRow);

  laneOrderMessage = makeText("", 15);
  laneOrderMessage->setThemedColor(app_theme::textMuted);
  laneOrderMessage->setAlign(TextView::LEFT);
  laneOrderMessage->setHeight(24);
  addView(laneOrderMessage);
}

void PlayOptionSectionView::refresh(const std::string &selectedOption,
                                    const std::string &defaultLaneOrder,
                                    bool laneOrderEnabled) {
  const std::string normalized =
      play_options::normalizePlayOption(selectedOption);
  std::vector<DropdownView::Option> options;
  for (const auto option : play_options::kPlayOptions) {
    const std::string id(option);
    options.push_back({.id = id, .label = id,
        .available = !callbacks.isOptionAllowed || callbacks.isOptionAllowed(id)});
  }
  if (play_options::laneAssignNotationFromOption(normalized).has_value()) {
    options.push_back({.id = normalized,
        .label = play_options::formatPlayOptionLabel(normalized),
        .available = false});
  }
  optionChoice->refresh({.label = i18n::message("play_options.section.play_option.label"),
                         .selectedId = normalized, .options = std::move(options)});

  if (laneOrderInput != nullptr &&
      (!laneOrderSnapshotInitialized || lastLaneOrderOption != normalized ||
       lastDefaultLaneOrder != defaultLaneOrder ||
       lastLaneOrderEnabled != laneOrderEnabled)) {
    const auto assigned =
        play_options::laneAssignNotationFromOption(selectedOption);
    laneOrderInput->setEditingText(assigned.value_or(defaultLaneOrder));
    laneOrderSnapshotInitialized = true;
    lastLaneOrderOption = normalized;
    lastDefaultLaneOrder = defaultLaneOrder;
    lastLaneOrderEnabled = laneOrderEnabled;
  }
  if (applyLaneOrderButton != nullptr) {
    applyLaneOrderButton->setEnabled(laneOrderEnabled);
  }
  if (resetLaneOrderButton != nullptr) {
    resetLaneOrderButton->setEnabled(true);
  }
  if (!laneOrderEnabled) {
    setLaneOrderMessage(i18n::message("play_options.section.lane_order_unavailable_selection.message"));
  } else if (laneOrderMessage != nullptr) {
    laneOrderMessage->setText("");
  }
}

void PlayOptionSectionView::setLaneOrderMessage(i18n::Text message,
                                                bool error) {
  if (laneOrderMessage == nullptr) {
    return;
  }
  laneOrderMessage->setLocalizedText(std::move(message));
  laneOrderMessage->setColor(
      app_theme::sdl(error ? app_theme::coral() : app_theme::textMuted()));
}

void PlayOptionSectionView::closeDropdowns() {
  optionChoice->closeMenu();
}
