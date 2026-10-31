#pragma once

#include "AppTheme.h"
#include "Button.h"
#include "IconText.h"
#include "TextView.h"

class PlayStartButton final : public Button {
public:
  PlayStartButton(int width, int height) : Button(0, 0, width, height) {
    setFlexShrink(0);
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
    content->setJustifyContent(YGJustifyCenter)->setPadding(Edge::All, 4);
    auto *title = new View();
    title->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
    title->setJustifyContent(YGJustifyCenter)->setGap(16);
    title->setFlex(1)->setMinHeight(0);
    YGNodeStyleSetMaxHeight(title->getNode(), 64);
    icon_ = new TextView(ui_icons::kFontAwesomeSolidPath, 42);
    icon_->setName("selectorButtonIcon");
    icon_->setWidth(48)->setAutoFitText(true);
    YGNodeStyleSetHeightPercent(icon_->getNode(), 100);
    icon_->setVAlign(TextView::MIDDLE);
    label_ = new TextView(main_menu_theme::font, 44, TextView::FontWeight::Bold);
    label_->setName("selectorButtonLabel");
    label_->setLocalizedText(i18n::message("menu.selector.play.label"));
    label_->setMinWidth(0)->setAutoFitText(true);
    YGNodeStyleSetHeightPercent(label_->getNode(), 100);
    label_->setAlign(TextView::CENTER);
    label_->setVAlign(TextView::MIDDLE);
    title->addView(icon_);
    title->addView(label_);
    summary_ = new TextView(main_menu_theme::font, 16);
    summary_->setName("quickRetryStartSummary");
    summary_->setHeight(32)->setFlexShrink(0)->setMinWidth(0)->setAutoFitText(true);
    summary_->setAlign(TextView::CENTER);
    summary_->setVAlign(TextView::MIDDLE);
    summary_->setWrap(true);
    content->addView(title);
    content->addView(summary_);
    setContentView(content);
    setQuickRetry(false, {});
  }

  TextView *label() const { return label_; }

  void setQuickRetry(bool enabled, const std::string &summary) {
    summary_->setText(summary);
    summary_->setVisible(enabled);
    summary_->setDisplay(enabled ? YGDisplayFlex : YGDisplayNone);
    icon_->setText(ui_icons::textForCodepoint(enabled ? 0xf071 : ui_icons::kPlay));
    setThemedBackgroundColors(enabled ? app_theme::warningAction : main_menu_theme::action,
        enabled ? app_theme::warningActionHover : main_menu_theme::actionHover,
        enabled ? app_theme::warningActionPressed : main_menu_theme::actionPressed);
    setStyledBorderWidth(enabled ? 2 : 0);
    setThemedBorderColors(main_menu_theme::warning, main_menu_theme::warning, main_menu_theme::warning);
    for (auto *text : {icon_, label_, summary_}) text->setThemedColor(main_menu_theme::onAction);
  }

private:
  TextView *icon_ = nullptr;
  TextView *label_ = nullptr;
  TextView *summary_ = nullptr;
};
