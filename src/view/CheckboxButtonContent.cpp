#include "CheckboxButtonContent.h"

#include "IconText.h"
#include "TextView.h"

#include <utility>

namespace {
constexpr const char *kUiFont = "assets/fonts/notosanscjkjp.ttf";
}

CheckboxButtonContent::CheckboxButtonContent(std::string label, int labelSize,
                                             int iconSize) {
  setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(7);

  icon_ = new TextView(ui_icons::kFontAwesomeSolidPath, iconSize);
  icon_->setWidth(24)->setHeight(54);
  icon_->setAlign(TextView::CENTER);
  icon_->setVAlign(TextView::MIDDLE);
  icon_->setOverflow(TextView::TextOverflow::Hidden);
  addView(icon_);

  label_ = new TextView(kUiFont, labelSize);
  label_->setText(std::move(label));
  label_->setHeight(54);
  label_->setVAlign(TextView::MIDDLE);
  label_->setOverflow(TextView::TextOverflow::Hidden);
  if (label_->getText().empty()) {
    label_->setDisplay(YGDisplayNone);
  }
  addView(label_);

  setChecked(false);
}

void CheckboxButtonContent::setChecked(bool value) {
  checked_ = value;
  icon_->setText(ui_icons::textForCodepoint(
      checked_ ? ui_icons::kSquareCheck : ui_icons::kSquare));
}

void CheckboxButtonContent::setThemedColor(ThemeColorProvider provider) {
  icon_->setThemedColor(provider);
  label_->setThemedColor(std::move(provider));
}
