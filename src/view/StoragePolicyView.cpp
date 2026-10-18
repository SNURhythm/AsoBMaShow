#include "StoragePolicyView.h"
#include "DropdownView.h"
#include "TextView.h"
#include "AppTheme.h"
#include "../i18n/Localization.h"
using namespace storage_management;

StoragePolicyView::StoragePolicyView(Policy policy, std::function<void(Policy)> onChange,
                                     OverlayPortal *portal)
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
    auto *dropdown = new DropdownView({
        .onOpenChanged = [this, i](bool open) {
          openDropdown_ = open ? i : (openDropdown_ == i ? -1 : openDropdown_);
          Refresh();
        },
        .onOptionSelected = [this, i](const std::string &id) {
          if (!enabled_ || (i >= 2 && policy_.bga != BgaAction::CompressH264)) return;
          const auto previous = policy_;
          const int value = std::stoi(id);
          switch (i) {
          case 0: policy_.audio = static_cast<AudioAction>(value); break;
          case 1: policy_.bga = static_cast<BgaAction>(value); break;
          case 2: policy_.videoQuality = static_cast<VideoQuality>(value); break;
          case 3: policy_.resolution = static_cast<ResolutionCap>(value); break;
          case 4: policy_.maxFps = value; break;
          }
          openDropdown_ = -1;
          Refresh();
          if (policy_ != previous && onChange_) onChange_(policy_);
        }}, portal);
    dropdown->setTriggerWidth(230);
    dropdowns_.push_back(dropdown);
    row->addView(dropdown);
    addView(row);
  }
  Refresh();
}
void StoragePolicyView::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  if (!enabled_) openDropdown_ = -1;
  Refresh();
}
void StoragePolicyView::SetPolicy(Policy policy) { policy_ = policy; openDropdown_ = -1; Refresh(); }
void StoragePolicyView::onLanguageChanged() { View::onLanguageChanged(); Refresh(); }
void StoragePolicyView::Refresh() {
  const std::vector<DropdownView::Option> options[] = {
      {{"0", i18n::message("storage.ui.keep")}, {"1", i18n::message("storage.ui.flac")},
       {"2", i18n::message("storage.ui.ogg")}},
      {{"0", i18n::message("storage.ui.keep")}, {"1", i18n::message("storage.ui.h264")},
       {"2", i18n::message("storage.ui.delete_video")}},
      {{"0", i18n::message("storage.ui.high")}, {"1", i18n::message("storage.ui.balanced")},
       {"2", i18n::message("storage.ui.smaller")}},
      {{"0", i18n::message("storage.ui.original")}, {"1", "1920 × 1080"},
       {"2", "1280 × 720"}, {"3", "854 × 480"}, {"4", "640 × 360"}},
      {{"0", i18n::message("storage.ui.original")}, {"60", "60"}, {"30", "30"},
       {"24", "24"}, {"15", "15"}}};
  const int values[] = {static_cast<int>(policy_.audio), static_cast<int>(policy_.bga),
      static_cast<int>(policy_.videoQuality), static_cast<int>(policy_.resolution), policy_.maxFps};
  for (std::size_t i = 0; i < dropdowns_.size(); ++i) {
    const bool enabled = enabled_ && (i < 2 || policy_.bga == BgaAction::CompressH264);
    dropdowns_[i]->refresh({.selectedId = std::to_string(values[i]), .options = options[i],
        .open = enabled && openDropdown_ == static_cast<int>(i), .enabled = enabled});
  }
}
