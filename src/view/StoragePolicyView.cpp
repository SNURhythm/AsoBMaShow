#include "StoragePolicyView.h"
#include "Button.h"
#include "TextView.h"
#include "AppTheme.h"
#include "../i18n/Localization.h"
#include <array>
#include <algorithm>
using namespace storage_management;

StoragePolicyView::StoragePolicyView(Policy policy, std::function<void(Policy)> onChange)
    : policy_(policy), onChange_(std::move(onChange)) {
  setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(6);
  const char *labels[] = {"storage.ui.audio", "storage.ui.bga", "storage.ui.quality",
                          "storage.ui.resolution", "storage.ui.fps"};
  for (int i = 0; i < 5; ++i) {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(8);
    auto *label = new TextView(app_theme::font, 22);
    label->setLocalizedText(i18n::message(labels[i]));
    label->setWrap(true);
    label->setThemedColor(app_theme::textSecondary);
    label->setFlex(1)->setMinWidth(0)->setHeight(66);
    row->addView(label);
    auto *button = new Button();
    button->setWidth(230)->setHeight(66)->setCornerRadius(app_theme::controlRadius());
    button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                     app_theme::controlPressed);
    auto *value = new TextView(app_theme::font, 20);
    value->setAlign(TextView::CENTER);
    value->setVAlign(TextView::MIDDLE);
    value->setThemedColor(app_theme::textPrimary);
    button->setContentView(value);
    values_.push_back(value);
    buttons_.push_back(button);
    button->setOnClickListener([this, i] {
      switch (i) {
      case 0: policy_.audio = static_cast<AudioAction>((static_cast<int>(policy_.audio) + 1) % 3); break;
      case 1: policy_.bga = static_cast<BgaAction>((static_cast<int>(policy_.bga) + 1) % 3); break;
      case 2: policy_.videoQuality = static_cast<VideoQuality>((static_cast<int>(policy_.videoQuality) + 1) % 3); break;
      case 3: policy_.resolution = static_cast<ResolutionCap>((static_cast<int>(policy_.resolution) + 1) % 5); break;
      case 4: {
        constexpr std::array<int, 6> fps{0, 60, 30, 24, 15, 0};
        const auto it = std::find(fps.begin(), fps.end() - 1, policy_.maxFps);
        policy_.maxFps = it == fps.end() - 1 ? 0 : *std::next(it);
        break;
      }
      }
      Refresh();
      if (onChange_) onChange_(policy_);
    });
    row->addView(button);
    addView(row);
  }
  Refresh();
}
void StoragePolicyView::SetEnabled(bool enabled) { enabled_ = enabled; Refresh(); }
void StoragePolicyView::SetPolicy(Policy policy) { policy_ = policy; Refresh(); }
void StoragePolicyView::onLanguageChanged() { View::onLanguageChanged(); Refresh(); }
void StoragePolicyView::Refresh() {
  for (std::size_t i = 0; i < buttons_.size(); ++i)
    buttons_[i]->setEnabled(enabled_ && (i < 2 || policy_.bga == BgaAction::CompressH264));
  const char *audio[] = {"storage.ui.keep", "storage.ui.flac", "storage.ui.ogg"};
  const char *bga[] = {"storage.ui.keep", "storage.ui.h264", "storage.ui.delete_video"};
  const char *quality[] = {"storage.ui.high", "storage.ui.balanced", "storage.ui.smaller"};
  const char *resolution[] = {"", "1920 × 1080", "1280 × 720", "854 × 480", "640 × 360"};
  values_[0]->setLocalizedText(i18n::message(audio[static_cast<int>(policy_.audio)]));
  values_[1]->setLocalizedText(i18n::message(bga[static_cast<int>(policy_.bga)]));
  values_[2]->setLocalizedText(i18n::message(quality[static_cast<int>(policy_.videoQuality)]));
  values_[3]->setText(policy_.resolution == ResolutionCap::Original
      ? i18n::tr("storage.ui.original") : resolution[static_cast<int>(policy_.resolution)]);
  values_[4]->setText(policy_.maxFps == 0 ? i18n::tr("storage.ui.original") : std::to_string(policy_.maxFps));
}
