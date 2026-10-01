#include "NewcomerTutorialView.h"

#include "../i18n/Localization.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <utility>

namespace {
constexpr std::array<const char *, 3> kLanguages{"en", "ko", "ja"};
constexpr std::array<const char *, 3> kLanguageNames{"English", "한국어", "日本語"};

TextView *label(int size) {
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", size);
  text->setThemedColor(ui_theme::textPrimary);
  text->setVAlign(TextView::MIDDLE);
  return text;
}

Button *button(const i18n::Text &caption, TextView **textOut = nullptr) {
  auto *result = new Button();
  result->setHeight(52)->setFlex(1)->setCornerRadius(ui_theme::controlRadius());
  result->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                    ui_theme::controlPressed);
  result->setThemedBorderColors(ui_theme::hairlineStrong, ui_theme::accentBorder,
                                ui_theme::accentBorderStrong);
  result->setStyledBorderWidth(1);
  auto *text = label(22);
  text->setAlign(TextView::CENTER);
  text->setLocalizedText(caption);
  result->setContentView(text);
  if (textOut) *textOut = text;
  return result;
}

void place(View *view, float x, float y, float width, float height) {
  view->setPositionType(YGPositionTypeAbsolute);
  view->setPosition(Edge::Left, x);
  view->setPosition(Edge::Top, y);
  view->setSize(std::max(0.0F, width), std::max(0.0F, height));
}
} // namespace

NewcomerTutorialView::NewcomerTutorialView(NewcomerTutorialCallbacks callbacks,
                                          bool folderImportCopies)
    : callbacks_(std::move(callbacks)), folderImportCopies_(folderImportCopies) {
  View::LayoutBatchScope batch;
  language_ = i18n::language() == i18n::Language::Korean ? "ko"
              : i18n::language() == i18n::Language::Japanese ? "ja" : "en";
  setPositionType(YGPositionTypeAbsolute);
  setPosition(Edge::Left, 0);
  setPosition(Edge::Top, 0);
  setZIndex(2000);
  for (auto *&scrim : scrims_) {
    scrim = new View();
    scrim->setBackgroundColor(Color(0, 0, 0, 170));
    addView(scrim);
  }
  highlight_ = new View();
  highlight_->setThemedBorderColor(ui_theme::cyan);
  highlight_->setBorderWidth(3)->setCornerRadius(ui_theme::controlRadius());
  addView(highlight_);

  panel_ = new View();
  panel_->setFlexDirection(FlexDirection::Column)
      ->setPadding(Edge::All, 24)->setGap(12)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setThemedBorderColor(ui_theme::accentBorderStrong)
      ->setBorderWidth(1)->setCornerRadius(ui_theme::panelRadius());
  progress_ = label(18);
  progress_->setHeight(32)->setFlexShrink(0);
  progress_->setThemedColor(ui_theme::textSecondary);
  panel_->addView(progress_);
  title_ = label(30);
  title_->setHeight(46)->setFlexShrink(0);
  panel_->addView(title_);
  body_ = label(22);
  body_->setWrap(true);
  body_->setVAlign(TextView::TOP);
  body_->setFlex(1);
  panel_->addView(body_);

  languages_ = new View();
  languages_->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
  for (std::size_t i = 0; i < kLanguages.size(); ++i) {
    auto *choice = button(kLanguageNames[i]);
    choice->setOnClickListener([this, i] { chooseLanguage(kLanguages[i]); });
    languages_->addView(choice);
    languageButtons_[i] = choice;
  }
  panel_->addView(languages_);
  status_ = label(18);
  status_->setThemedColor(ui_theme::coral);
  status_->setWrap(true);
  panel_->addView(status_);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row)->setHeight(52)
      ->setFlexShrink(0)->setGap(10);
  auto *skipButton = button(i18n::message("tutorial.controls.skip"));
  skipButton->setOnClickListener([this] { skip(); });
  footer->addView(skipButton);
  back_ = button(i18n::message("tutorial.controls.back"));
  back_->setOnClickListener([this] { back(); });
  footer->addView(back_);
  auto *next = button(i18n::message("tutorial.controls.next"), &nextText_);
  next->setThemedBackgroundColors(ui_theme::primaryAction,
                                  ui_theme::primaryActionHover,
                                  ui_theme::primaryActionPressed);
  nextText_->setThemedColor([] { return ui_theme::textOn(ui_theme::primaryAction()); });
  next->setOnClickListener([this] { advance(); });
  footer->addView(next);
  panel_->addView(footer);
  addView(panel_);
  refresh();
}

bool NewcomerTutorialView::saveLanguage() {
  saveFailed_ = callbacks_.saveLanguage && !callbacks_.saveLanguage(language_);
  refresh();
  return !saveFailed_;
}

void NewcomerTutorialView::chooseLanguage(const std::string &language) {
  if (std::find(kLanguages.begin(), kLanguages.end(), language) == kLanguages.end()) return;
  language_ = language;
  saveLanguage();
}

void NewcomerTutorialView::changeStep(NewcomerTutorialStep step) {
  step_ = step;
  saveFailed_ = false;
  if (callbacks_.stepChanged) callbacks_.stepChanged(step_);
  refresh();
}

void NewcomerTutorialView::advance() {
  if (!getVisible()) return;
  if (step_ == NewcomerTutorialStep::Language && !saveLanguage()) return;
  if (step_ == NewcomerTutorialStep::PlayOptions) {
    finish();
    return;
  }
  changeStep(static_cast<NewcomerTutorialStep>(static_cast<int>(step_) + 1));
}

void NewcomerTutorialView::back() {
  if (!getVisible() || step_ == NewcomerTutorialStep::Language) return;
  changeStep(static_cast<NewcomerTutorialStep>(static_cast<int>(step_) - 1));
}

void NewcomerTutorialView::finish() {
  saveFailed_ = callbacks_.complete && !callbacks_.complete();
  if (!saveFailed_) {
    setVisible(false);
    if (callbacks_.stepChanged) callbacks_.stepChanged(step_);
  } else {
    refresh();
  }
}

void NewcomerTutorialView::skip() {
  if (!getVisible()) return;
  if (step_ == NewcomerTutorialStep::Language && !saveLanguage()) return;
  finish();
}

void NewcomerTutorialView::refresh() {
  const bool language = step_ == NewcomerTutorialStep::Language;
  progress_->setLocalizedText(language ? i18n::message("tutorial.welcome.title")
      : i18n::message("tutorial.progress.label", {{"step", std::to_string(static_cast<int>(step_))}}));
  if (language) {
    title_->setText("Language · 언어 · 言語");
    body_->setLocalizedText(i18n::message("tutorial.language.body"));
  } else {
    constexpr std::array<const char *, 4> titles{
        "tutorial.tables.title", "tutorial.download.title", "tutorial.folder.title", "tutorial.options.title"};
    constexpr std::array<const char *, 4> bodies{
        "tutorial.tables.body", "tutorial.download.body", "tutorial.folder.body", "tutorial.options.body"};
    const auto index = static_cast<std::size_t>(step_) - 1;
    title_->setLocalizedText(i18n::message(titles[index]));
    body_->setLocalizedText(i18n::message(step_ == NewcomerTutorialStep::Folder && folderImportCopies_
        ? "tutorial.folder.copy_body" : bodies[index]));
  }
  languages_->setVisible(language);
  languages_->setHeight(language ? 52 : 0);
  for (std::size_t i = 0; i < kLanguages.size(); ++i) {
    const bool selected = language_ == kLanguages[i];
    languageButtons_[i]->setSelected(selected);
    languageButtons_[i]->setThemedBorderColors(
        selected ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong,
        ui_theme::accentBorder, ui_theme::accentBorderStrong);
  }
  back_->setEnabled(!language);
  nextText_->setLocalizedText(i18n::message(language ? "tutorial.controls.begin"
      : step_ == NewcomerTutorialStep::PlayOptions ? "tutorial.controls.finish" : "tutorial.controls.next"));
  status_->setVisible(saveFailed_);
  status_->setHeight(saveFailed_ ? 46 : 0);
  status_->setLocalizedText(i18n::message("tutorial.save.error"));
  propagateLanguageChange();
  updateLayout(rendering::window_width, rendering::window_height);
}

void NewcomerTutorialView::updateLayout(int width, int height) {
  if (!getVisible() || width <= 0 || height <= 0) return;
  View::LayoutBatchScope batch;
  setSize(width, height);
  View *target = step_ == NewcomerTutorialStep::Language || !callbacks_.target
                     ? nullptr : callbacks_.target(step_);
  const bool spotlight = target && target->getVisible() && target->getWidth() > 0 && target->getHeight() > 0;
  float left = 0, top = 0, right = 0, bottom = 0;
  if (spotlight) {
    left = std::clamp(static_cast<float>(target->getX() - 8), 0.0F, static_cast<float>(width));
    top = std::clamp(static_cast<float>(target->getY() - 8), 0.0F, static_cast<float>(height));
    right = std::clamp(static_cast<float>(target->getX() + target->getWidth() + 8), left, static_cast<float>(width));
    bottom = std::clamp(static_cast<float>(target->getY() + target->getHeight() + 8), top, static_cast<float>(height));
  }
  highlight_->setVisible(spotlight);
  place(highlight_, left, top, right - left, bottom - top);
  place(scrims_[0], 0, 0, width, top);
  place(scrims_[1], 0, top, left, bottom - top);
  place(scrims_[2], right, top, width - right, bottom - top);
  place(scrims_[3], 0, bottom, width, height - bottom);

  float panelWidth = std::min(580.0F, std::max(0.0F, width - 40.0F));
  if (spotlight) {
    const float sideWidth = std::max(left - 40, width - right - 40);
    if (sideWidth >= 340) panelWidth = std::min(panelWidth, sideWidth);
  }
  const float desiredHeight = panelWidth < 440 ? 500.0F : 400.0F;
  const float panelHeight = std::min(desiredHeight + (saveFailed_ ? 46 : 0), std::max(0.0F, height - 40.0F));
  float x = (width - panelWidth) / 2;
  float y = (height - panelHeight) / 2;
  if (spotlight) {
    if (right + 20 + panelWidth <= width - 20) x = right + 20;
    else if (left - 20 - panelWidth >= 20) x = left - 20 - panelWidth;
    else if (bottom + 20 + panelHeight <= height - 20) y = bottom + 20;
    else if (top - 20 - panelHeight >= 20) y = top - 20 - panelHeight;
  }
  place(panel_, x, y, panelWidth, panelHeight);
}

bool NewcomerTutorialView::handleEventsImpl(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && !event.key.repeat) {
    switch (event.key.keysym.sym) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_RIGHT: advance(); break;
    case SDLK_LEFT: back(); break;
    case SDLK_ESCAPE: skip(); break;
    case SDLK_1: if (step_ == NewcomerTutorialStep::Language) chooseLanguage("en"); break;
    case SDLK_2: if (step_ == NewcomerTutorialStep::Language) chooseLanguage("ko"); break;
    case SDLK_3: if (step_ == NewcomerTutorialStep::Language) chooseLanguage("ja"); break;
    default: break;
    }
  }
  return false;
}
