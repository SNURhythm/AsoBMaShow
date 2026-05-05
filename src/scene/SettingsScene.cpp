#include "SettingsScene.h"
#include "../context.h"
#include "../rendering/Color.h"
#include "../view/Button.h"
#include "../view/TextView.h"

#include <algorithm>
#include <string>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

TextView *makeText(const std::string &text, int size, const Color &color,
                   TextView::TextAlign align = TextView::LEFT,
                   TextView::TextVAlign valign = TextView::TOP) {
  auto *view = new TextView(kFontPath, size);
  view->setText(text);
  view->setColor({color.r, color.g, color.b, color.a});
  view->setAlign(align);
  view->setVAlign(valign);
  return view;
}

Button *makeButton(int width, int height, TextView *label,
                   const Color &normalBackground,
                   const Color &hoverBackground,
                   const Color &pressedBackground,
                   const Color &normalBorder, const Color &hoverBorder,
                   const Color &pressedBorder, int borderWidth = 2) {
  auto *button = new Button(0, 0, width, height);
  label->setAlign(TextView::CENTER);
  label->setVAlign(TextView::MIDDLE);
  button->setContentView(label);
  button->setBackgroundColors(normalBackground, hoverBackground,
                              pressedBackground);
  button->setBorderColors(normalBorder, hoverBorder, pressedBorder);
  button->setStyledBorderWidth(borderWidth);
  return button;
}

View *makeCard(const std::string &title, const std::string &description,
               View *body, int minHeight) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column);
  card->setGap(22);
  card->setPadding(Edge::All, 28);
  card->setBackgroundColor(Color(19, 30, 46, 245));
  card->setBorderColor(Color(76, 104, 136, 255));
  card->setBorderWidth(2);
  card->setHeight(static_cast<float>(minHeight));

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setGap(8);
  auto *titleText = makeText(title, 34, Color(244, 248, 255));
  auto *descText = makeText(description, 22, Color(168, 186, 209));
  header->addView(titleText);
  header->addView(descText);

  card->addView(header);
  card->addView(body);
  return card;
}

View *makeSummaryRow(const std::string &label, TextView **valueOut) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifySpaceBetween);
  row->setAlignItems(YGAlignCenter);

  auto *labelText = makeText(label, 24, Color(164, 186, 206));
  auto *valueText =
      makeText("", 24, Color(244, 248, 255), TextView::RIGHT);
  row->addView(labelText);
  row->addView(valueText);
  if (valueOut != nullptr) {
    *valueOut = valueText;
  }
  return row;
}

int clampOffset(int value) {
  return std::clamp(value, AppSettings::kMinInputOffsetMs,
                    AppSettings::kMaxInputOffsetMs);
}
} // namespace

void SettingsScene::init() { initView(); }

void SettingsScene::initView() {
  View::LayoutBatchScope layoutBatch;

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setPadding(Edge::All, 68);
  rootLayout->setGap(28);
  rootLayout->setBackgroundColor(Color(10, 18, 30));

  auto *accentA = new View(110, 86, 480, 180);
  accentA->setPositionType(YGPositionTypeAbsolute);
  accentA->setBackgroundColor(Color(39, 101, 160, 96));
  rootLayout->addView(accentA);

  auto *accentB = new View(rendering::window_width - 520,
                           rendering::window_height - 250, 420, 160);
  accentB->setPositionType(YGPositionTypeAbsolute);
  accentB->setBackgroundColor(Color(207, 110, 62, 72));
  rootLayout->addView(accentB);

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(10);
  headerText->addView(
      makeText("Settings", 72, Color(244, 248, 255)));
  headerText->addView(makeText(
      "Persistent player preferences for timing, keysounds, and visual load.",
      26, Color(162, 183, 205)));
  header->addView(headerText);

  auto *backLabel = makeText("Back", 28, Color(237, 243, 252),
                             TextView::CENTER, TextView::MIDDLE);
  auto *backButton =
      makeButton(180, 64, backLabel, Color(22, 33, 49, 255),
                 Color(31, 46, 67, 255), Color(53, 78, 110, 255),
                 Color(96, 121, 156, 255), Color(120, 151, 190, 255),
                 Color(148, 186, 231, 255));
  backButton->setOnClickListener(
      [this]() { context.sceneManager->changeScene("MainMenu"); });
  header->addView(backButton);
  rootLayout->addView(header);

  auto *body = new View();
  body->setFlex(1);
  body->setFlexDirection(FlexDirection::Row);
  body->setGap(28);
  body->setAlignItems(YGAlignStretch);

  auto *summaryCard = new View();
  summaryCard->setWidth(420);
  summaryCard->setPadding(Edge::All, 30);
  summaryCard->setGap(18);
  summaryCard->setFlexDirection(FlexDirection::Column);
  summaryCard->setBackgroundColor(Color(17, 27, 42, 245));
  summaryCard->setBorderColor(Color(68, 94, 123, 255));
  summaryCard->setBorderWidth(2);

  summaryCard->addView(makeText("Profile Snapshot", 34, Color(244, 248, 255)));
  summaryCard->addView(makeText(
      "Saved immediately for new charts.",
      22, Color(160, 181, 204)));
  summaryCard->addView(makeSummaryRow("Judgement Offset",
                                      &summaryOffsetValueText));
  summaryCard->addView(
      makeSummaryRow("Input Keysounds", &summaryKeysoundValueText));
  summaryCard->addView(makeSummaryRow("BGA Playback", &summaryBgaValueText));
  summaryCard->addView(makeText(
      "Positive offset judges later. Auto timed audio ignores hit timing.",
      20, Color(131, 151, 176)));

  auto *cardsColumn = new View();
  cardsColumn->setFlex(1);
  cardsColumn->setFlexDirection(FlexDirection::Column);
  cardsColumn->setGap(24);

  auto *offsetControls = new View();
  offsetControls->setFlexDirection(FlexDirection::Row);
  offsetControls->setGap(12);
  offsetControls->setAlignItems(YGAlignCenter);

  auto updateOffset = [this](int delta) {
    context.settings.inputOffsetMs =
        clampOffset(context.settings.inputOffsetMs + delta);
    persistSettings();
  };

  auto *minusTen = makeButton(
      110, 72, makeText("-10", 28, Color(239, 244, 251), TextView::CENTER,
                         TextView::MIDDLE),
      Color(28, 40, 58, 255), Color(36, 52, 75, 255),
      Color(61, 87, 118, 255), Color(84, 107, 139, 255),
      Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusTen->setOnClickListener([updateOffset]() { updateOffset(-10); });
  offsetControls->addView(minusTen);

  auto *minusOne = makeButton(
      96, 72, makeText("-1", 28, Color(239, 244, 251), TextView::CENTER,
                        TextView::MIDDLE),
      Color(28, 40, 58, 255), Color(36, 52, 75, 255),
      Color(61, 87, 118, 255), Color(84, 107, 139, 255),
      Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  minusOne->setOnClickListener([updateOffset]() { updateOffset(-1); });
  offsetControls->addView(minusOne);

  auto *offsetValue = new View();
  offsetValue->setWidth(270);
  offsetValue->setHeight(72);
  offsetValue->setBackgroundColor(Color(10, 17, 28, 255));
  offsetValue->setBorderColor(Color(78, 105, 140, 255));
  offsetValue->setBorderWidth(2);
  offsetValueText = makeText("", 30, Color(244, 248, 255), TextView::CENTER,
                             TextView::MIDDLE);
  offsetValue->addView(offsetValueText);
  offsetControls->addView(offsetValue);

  auto *plusOne = makeButton(
      96, 72, makeText("+1", 28, Color(239, 244, 251), TextView::CENTER,
                        TextView::MIDDLE),
      Color(28, 40, 58, 255), Color(36, 52, 75, 255),
      Color(61, 87, 118, 255), Color(84, 107, 139, 255),
      Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusOne->setOnClickListener([updateOffset]() { updateOffset(1); });
  offsetControls->addView(plusOne);

  auto *plusTen = makeButton(
      110, 72, makeText("+10", 28, Color(239, 244, 251), TextView::CENTER,
                         TextView::MIDDLE),
      Color(28, 40, 58, 255), Color(36, 52, 75, 255),
      Color(61, 87, 118, 255), Color(84, 107, 139, 255),
      Color(108, 136, 174, 255), Color(139, 172, 217, 255));
  plusTen->setOnClickListener([updateOffset]() { updateOffset(10); });
  offsetControls->addView(plusTen);

  auto *resetOffset = makeButton(
      140, 72, makeText("Reset", 28, Color(248, 241, 236), TextView::CENTER,
                         TextView::MIDDLE),
      Color(96, 57, 44, 255), Color(117, 72, 55, 255),
      Color(153, 96, 74, 255), Color(165, 105, 79, 255),
      Color(193, 124, 93, 255), Color(219, 145, 108, 255));
  resetOffset->setOnClickListener([this]() {
    context.settings.inputOffsetMs = 0;
    persistSettings();
  });
  offsetControls->addView(resetOffset);

  cardsColumn->addView(makeCard(
      "Judgement Offset",
      "Positive values judge later. Use this when your hits consistently feel "
      "early relative to the music.",
      offsetControls, 190));

  auto *keysoundControls = new View();
  keysoundControls->setFlexDirection(FlexDirection::Column);
  keysoundControls->setGap(16);
  keysoundControls->setAlignItems(YGAlignFlexStart);
  keysoundControls->addView(makeText(
      "Tap to switch modes. The current selection is shown on the right.",
      22, Color(150, 171, 193)));

  keysoundModeText = makeText("", 26, Color(245, 248, 252), TextView::CENTER,
                              TextView::MIDDLE);
  keysoundModeButton = makeButton(
      290, 72, keysoundModeText, Color(33, 56, 87, 255),
      Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  keysoundModeButton->setOnClickListener([this]() {
    context.settings.inputKeysoundEnabled =
        !context.settings.inputKeysoundEnabled;
    persistSettings();
  });
  keysoundControls->addView(keysoundModeButton);

  cardsColumn->addView(makeCard(
      "Input Keysounds",
      "Keep manual key clicks for classic BMS feedback, or switch to "
      "auto-timed playback for cleaner timing practice.",
      keysoundControls, 180));

  auto *bgaControls = new View();
  bgaControls->setFlexDirection(FlexDirection::Column);
  bgaControls->setGap(16);
  bgaControls->setAlignItems(YGAlignFlexStart);
  bgaControls->addView(makeText(
      "Tap to switch BGA rendering on or off for future previews and charts.",
      22, Color(150, 171, 193)));

  bgaModeText = makeText("", 26, Color(245, 248, 252), TextView::CENTER,
                         TextView::MIDDLE);
  bgaModeButton = makeButton(
      290, 72, bgaModeText, Color(33, 56, 87, 255),
      Color(43, 72, 110, 255), Color(59, 98, 147, 255),
      Color(92, 131, 177, 255), Color(118, 163, 217, 255),
      Color(139, 189, 244, 255));
  bgaModeButton->setOnClickListener([this]() {
    context.settings.bgaEnabled = !context.settings.bgaEnabled;
    persistSettings();
  });
  bgaControls->addView(bgaModeButton);

  cardsColumn->addView(makeCard(
      "BGA Playback",
      "Disable background animation if you want lower distraction or a lighter "
      "render path on slower hardware.",
      bgaControls, 180));

  auto *footer = new View();
  footer->setPadding(Edge::All, 24);
  footer->setBackgroundColor(Color(14, 22, 34, 220));
  footer->setBorderColor(Color(59, 80, 108, 255));
  footer->setBorderWidth(2);
  footer->addView(makeText(
      "Settings are saved automatically in the app documents directory.",
      22, Color(165, 185, 205)));
  cardsColumn->addView(footer);

  body->addView(summaryCard);
  body->addView(cardsColumn);
  rootLayout->addView(body);

  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshSettingsText();
}

void SettingsScene::refreshSettingsText() {
  const int offsetMs = context.settings.inputOffsetMs;
  const std::string offsetLabel =
      (offsetMs > 0 ? "+" : "") + std::to_string(offsetMs) + " ms";
  const std::string keysoundLabel =
      context.settings.inputKeysoundEnabled ? "Input Trigger" : "Auto Timed";
  const std::string bgaLabel =
      context.settings.bgaEnabled ? "Enabled" : "Disabled";

  if (offsetValueText != nullptr) {
    offsetValueText->setText(offsetLabel);
  }
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setText(offsetLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setText(bgaLabel);
  }
  if (keysoundModeText != nullptr) {
    keysoundModeText->setText(keysoundLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setText(bgaLabel);
  }

  if (keysoundModeButton != nullptr) {
    if (context.settings.inputKeysoundEnabled) {
      keysoundModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                              Color(43, 72, 110, 255),
                                              Color(59, 98, 147, 255));
      keysoundModeButton->setBorderColors(Color(92, 131, 177, 255),
                                          Color(118, 163, 217, 255),
                                          Color(139, 189, 244, 255));
    } else {
      keysoundModeButton->setBackgroundColors(Color(73, 56, 35, 255),
                                              Color(96, 72, 45, 255),
                                              Color(127, 95, 59, 255));
      keysoundModeButton->setBorderColors(Color(165, 120, 74, 255),
                                          Color(194, 141, 88, 255),
                                          Color(224, 163, 103, 255));
    }
  }

  if (bgaModeButton != nullptr) {
    if (context.settings.bgaEnabled) {
      bgaModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                         Color(45, 88, 80, 255),
                                         Color(63, 118, 107, 255));
      bgaModeButton->setBorderColors(Color(97, 157, 142, 255),
                                     Color(120, 187, 169, 255),
                                     Color(145, 214, 195, 255));
    } else {
      bgaModeButton->setBackgroundColors(Color(56, 42, 40, 255),
                                         Color(75, 55, 52, 255),
                                         Color(104, 75, 71, 255));
      bgaModeButton->setBorderColors(Color(141, 103, 98, 255),
                                     Color(176, 127, 121, 255),
                                     Color(209, 150, 143, 255));
    }
  }

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  refreshSettingsText();
}

void SettingsScene::update(float dt) { (void)dt; }

void SettingsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
}

void SettingsScene::cleanupScene() {}
