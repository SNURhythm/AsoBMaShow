#include "StoragePolicyView.h"
#include "DropdownView.h"
#include "Button.h"
#include "ToggleButtonContent.h"
#include "BlockingOverlayView.h"
#include "ModalViewHelpers.h"
#include "OverlayPortal.h"
#include "ScrollView.h"
#include "TextView.h"
#include "AppTheme.h"
#include "../i18n/Localization.h"
#include <chrono>
using namespace storage_management;

class StorageCodecModal final : public BlockingOverlayView {
public:
  StorageCodecModal(std::function<void(VideoCodec)> choose, std::function<void()> close,
                    std::function<void()> poll)
      : poll_(std::move(poll)), close_(std::move(close)) {
    setThemedBackgroundColor(app_theme::scrim);
    panel_ = new View();
    panel_->setThemedBackgroundColor(app_theme::panelStrong);
    panel_->setCornerRadius(app_theme::controlRadius());
    panel_->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(12);
    panel_->setPadding(Edge::All, 20);
    addView(panel_);
    auto *title = makeText(i18n::message("storage.ui.codec_choose"), 28);
    title->setFlexShrink(0);
    panel_->addView(title);
    scroll_ = new ScrollView();
    scroll_->setFlex(1)->setMinHeight(0);
    content_ = new View();
    content_->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(16);
    status_ = makeText(i18n::message("storage.ui.codec_checking_help"), 20);
    content_->addView(status_);
    const char *names[] = {"H.264", "HEVC", "AV1"};
    const char *descriptions[] = {"storage.ui.codec_h264_tradeoff", "storage.ui.codec_hevc_tradeoff",
                                  "storage.ui.codec_av1_tradeoff"};
    for (int i = 0; i < 3; ++i) {
      cards_[i] = new View();
      cards_[i]->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(8);
      cards_[i]->setPadding(Edge::All, 14);
      cards_[i]->setThemedBackgroundColor(app_theme::panelSubtle);
      cards_[i]->setCornerRadius(app_theme::controlRadius());
      cards_[i]->addView(makeText(names[i], 24));
      cards_[i]->addView(makeText(i18n::message(descriptions[i]), 20));
      encoding_[i] = makeText("", 18);
      playback_[i] = makeText("", 18);
      cards_[i]->addView(encoding_[i]);
      cards_[i]->addView(playback_[i]);
      TextView *label = nullptr;
      choices_[i] = modal_view::makeModalButton(
          i18n::message("storage.ui.codec_select", {{"codec", names[i]}}), 22, &label);
      choices_[i]->setHeight(54)->setFlexShrink(0);
      modal_view::styleThemedActionButton(choices_[i], label, true, app_theme::primaryAction,
          app_theme::primaryActionHover, app_theme::primaryActionPressed, app_theme::primaryAction);
      choices_[i]->setOnClickListener([choose, i] { choose(static_cast<VideoCodec>(i)); });
      cards_[i]->addView(choices_[i]);
      content_->addView(cards_[i]);
    }
    scroll_->setContentView(content_);
    panel_->addView(scroll_);
    TextView *closeLabel = nullptr;
    auto *closeButton = modal_view::makeModalButton(i18n::message("storage.ui.codec_close"), 22, &closeLabel);
    closeButton->setHeight(54)->setFlexShrink(0);
    modal_view::styleThemedActionButton(closeButton, closeLabel, true, app_theme::control,
        app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
    closeButton->setOnClickListener(close_);
    panel_->addView(closeButton);
    Fit();
  }
  void Refresh(const StorageVideoCapabilities &capabilities, bool ready, VideoCodec selected) {
    bool any = false;
    for (int i = 0; i < 3; ++i) {
      const auto &support = capabilities[i];
      const bool available = ready && support.available && support.playbackAvailable;
      any = any || available;
      // Layout display and render/input visibility are independent in View.
      cards_[i]->setVisible(available);
      cards_[i]->setDisplay(available ? YGDisplayFlex : YGDisplayNone);
      choices_[i]->setEnabled(available);
      choices_[i]->setSelected(selected == static_cast<VideoCodec>(i));
      encoding_[i]->setLocalizedText(i18n::message(support.hardware ?
          "storage.ui.codec_encode_hardware_help" : "storage.ui.codec_encode_software_help"));
      playback_[i]->setLocalizedText(i18n::message(support.playbackHardware ?
          "storage.ui.codec_playback_hardware_help" : "storage.ui.codec_playback_software_help"));
    }
    status_->setVisible(!any);
    status_->setDisplay(any ? YGDisplayNone : YGDisplayFlex);
    status_->setLocalizedText(i18n::message(ready ? "storage.ui.codec_none" : "storage.ui.codec_checking_help"));
    scroll_->refreshContentLayout();
  }
private:
  static TextView *makeText(const i18n::Text &text, int size) {
    auto *result = new TextView(app_theme::font, size);
    result->setLocalizedText(text);
    result->setThemedColor(app_theme::textPrimary);
    result->setWrap(true);
    result->setFlexShrink(0);
    return result;
  }
  void Fit() {
    const int width = std::max(1, rendering::window_width);
    const int height = std::max(1, rendering::window_height);
    const int margin = std::min({20, width / 20, height / 20});
    const int panelWidth = std::min(760, width - 2 * margin);
    const int panelHeight = std::min(920, height - 2 * margin);
    if (getWidth() == width && getHeight() == height && panel_->getWidth() == panelWidth &&
        panel_->getHeight() == panelHeight) return;
    setPosition(0, 0, YGPositionTypeAbsolute);
    setSize(width, height);
    panel_->setPosition((width - panelWidth) / 2, (height - panelHeight) / 2,
                        YGPositionTypeAbsolute);
    panel_->setSize(panelWidth, panelHeight);
    applyYogaLayout();
    scroll_->refreshContentLayout();
  }
  void renderImpl(RenderContext &) override { poll_(); Fit(); }
  bool handleEventsImpl(SDL_Event &event) override {
    poll_();
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) close_();
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_TEXT_EDITING: return false;
    default: return true;
    }
  }
  std::function<void()> poll_, close_;
  View *panel_ = nullptr;
  ScrollView *scroll_ = nullptr;
  View *content_ = nullptr;
  TextView *status_ = nullptr;
  std::array<View *, 3> cards_{};
  std::array<Button *, 3> choices_{};
  std::array<TextView *, 3> encoding_{}, playback_{};
};

StoragePolicyView::StoragePolicyView(Policy policy, std::function<void(Policy)> onChange,
                                     OverlayPortal *portal)
    : policy_(policy), portal_(portal), onChange_(std::move(onChange)) {
  setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)->setGap(6);
  capabilitiesFuture_ = RequestStorageVideoCapabilities([] {
    // The probe can outlive this view. Wake the event loop without retaining UI pointers.
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
  });
  const char *labels[] = {"storage.ui.audio", "storage.ui.bga", "storage.ui.codec",
                          "storage.ui.quality", "storage.ui.resolution", "storage.ui.fps"};
  for (int i = 0; i < 6; ++i) {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(8);
    auto *label = new TextView(app_theme::font, 22);
    label->setLocalizedText(i18n::message(labels[i]));
    label->setWrap(true);
    label->setThemedColor(app_theme::textSecondary);
    label->setFlex(1)->setMinWidth(0)->setHeight(66);
    row->addView(label);
    if (i == 2) {
      codecButton_ = modal_view::makeModalButton("", 18, &codecButtonText_);
      codecButton_->setWidth(230)->setHeight(66);
      codecButton_->setOnClickListener([this] { OpenCodecModal(); });
      row->addView(codecButton_);
      addView(row);
      continue;
    }
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
          case 3: policy_.videoQuality = static_cast<VideoQuality>(value); break;
          case 4: policy_.resolution = static_cast<ResolutionCap>(value); break;
          case 5: policy_.maxFps = value; break;
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
  codecStatus_ = new TextView(app_theme::font, 18);
  codecStatus_->setWrap(true);
  codecStatus_->setThemedColor(app_theme::textSecondary);
  addView(codecStatus_);
  efficientVideoButton_ = new Button();
  efficientVideoContent_ = new ToggleButtonContent("", 20, 24);
  efficientVideoContent_->setJustifyContent(YGJustifyFlexStart);
  efficientVideoContent_->setThemedColor(app_theme::textSecondary);
  auto *efficientLabel = efficientVideoContent_->labelView();
  efficientLabel->setLocalizedText(i18n::message("storage.ui.include_efficient"));
  efficientLabel->setDisplay(YGDisplayFlex);
  efficientLabel->setWrap(true);
  efficientLabel->setFlex(1)->setMinWidth(0)->setHeight(66);
  efficientVideoButton_->setContentView(efficientVideoContent_);
  efficientVideoButton_->setHeight(66);
  efficientVideoButton_->setOnClickListener([this] {
    if (!enabled_ || policy_.bga != BgaAction::CompressH264) return;
    policy_.reencodeEfficientVideo = !policy_.reencodeEfficientVideo;
    Refresh();
    if (onChange_) onChange_(policy_);
  });
  addView(efficientVideoButton_);
  Refresh();
}
void StoragePolicyView::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  if (!enabled_) { openDropdown_ = -1; CloseCodecModal(); }
  Refresh();
}
void StoragePolicyView::SetPolicy(Policy policy) { CloseCodecModal(); policy_ = policy; openDropdown_ = -1; Refresh(); }
void StoragePolicyView::onLanguageChanged() { View::onLanguageChanged(); Refresh(); }
void StoragePolicyView::PollCapabilities() {
  if (capabilitiesReady_ || !capabilitiesFuture_.valid() ||
      capabilitiesFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  try {
    capabilities_ = capabilitiesFuture_.get();
  } catch (...) {
    // A failed probe leaves every encoder unavailable while preserving the saved policy.
  }
  capabilitiesReady_ = true;
  Refresh();
}
void StoragePolicyView::renderImpl(RenderContext &) { PollCapabilities(); }
bool StoragePolicyView::handleEventsImpl(SDL_Event &) { PollCapabilities(); return true; }
void StoragePolicyView::Refresh() {
  const std::vector<DropdownView::Option> options[] = {
      {{"0", i18n::message("storage.ui.keep")}, {"1", i18n::message("storage.ui.flac")},
       {"2", i18n::message("storage.ui.ogg")}},
      {{"0", i18n::message("storage.ui.keep")}, {"1", i18n::message("storage.ui.compress_video")},
       {"2", i18n::message("storage.ui.delete_video")}},
      {{"0", i18n::message("storage.ui.high")}, {"1", i18n::message("storage.ui.balanced")},
       {"2", i18n::message("storage.ui.smaller")}},
      {{"0", i18n::message("storage.ui.original")}, {"1", "1920 × 1080"},
       {"2", "1280 × 720"}, {"3", "854 × 480"}, {"4", "640 × 360"}},
      {{"0", i18n::message("storage.ui.original")}, {"60", "60"}, {"30", "30"},
       {"24", "24"}, {"15", "15"}}};
  const int values[] = {static_cast<int>(policy_.audio), static_cast<int>(policy_.bga),
      static_cast<int>(policy_.videoQuality),
      static_cast<int>(policy_.resolution), policy_.maxFps};
  for (std::size_t i = 0; i < dropdowns_.size(); ++i) {
    const bool enabled = enabled_ && (i < 2 || policy_.bga == BgaAction::CompressH264);
    dropdowns_[i]->refresh({.selectedId = std::to_string(values[i]), .options = options[i],
        .open = enabled && openDropdown_ == static_cast<int>(i < 2 ? i : i + 1), .enabled = enabled});
  }
  const bool compressing = policy_.bga == BgaAction::CompressH264;
  efficientVideoButton_->setEnabled(enabled_ && compressing);
  efficientVideoContent_->setChecked(policy_.reencodeEfficientVideo);
  const int codec = static_cast<int>(policy_.videoCodec);
  const bool available = capabilitiesReady_ && codec >= 0 && codec < 3 &&
      capabilities_[codec].available && capabilities_[codec].playbackAvailable;
  const char *codecNames[] = {"H.264", "HEVC", "AV1"};
  const char *name = codec >= 0 && codec < 3 ? codecNames[codec] : "?";
  codecButtonText_->setLocalizedText(available ? i18n::Text(name) : i18n::message(
      capabilitiesReady_ ? "storage.ui.codec_unavailable_option" : "storage.ui.codec_checking", {{"codec", name}}));
  modal_view::styleThemedActionButton(codecButton_, codecButtonText_, enabled_ && compressing,
      app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
  codecStatus_->setVisible(compressing);
  codecStatus_->setDisplay(compressing ? YGDisplayFlex : YGDisplayNone);
  codecStatus_->setLocalizedText(i18n::message(!capabilitiesReady_ ? "storage.ui.codec_checking_help" :
      !available ? "storage.ui.codec_unavailable_help" : "storage.ui.codec_verified_help"));
  if (codecModal_) codecModal_->Refresh(capabilities_, capabilitiesReady_, policy_.videoCodec);
}

StoragePolicyView::~StoragePolicyView() { CloseCodecModal(); }
void StoragePolicyView::OpenCodecModal() {
  if (!enabled_ || policy_.bga != BgaAction::CompressH264) return;
  openDropdown_ = -1;
  PollCapabilities();
  if (!codecModal_) {
    codecModal_ = std::make_unique<StorageCodecModal>([this](VideoCodec codec) {
      const auto index = static_cast<std::size_t>(codec);
      if (!enabled_ || !capabilitiesReady_ || index >= capabilities_.size() ||
          !capabilities_[index].available || !capabilities_[index].playbackAvailable) return;
      const bool changed = policy_.videoCodec != codec;
      policy_.videoCodec = codec;
      CloseCodecModal();
      Refresh();
      if (changed && onChange_) onChange_(policy_);
    }, [this] { CloseCodecModal(); }, [this] { PollCapabilities(); });
  }
  codecModal_->setVisible(true);
  if (portal_) portal_->present(codecModal_.get());
  Refresh();
}
void StoragePolicyView::CloseCodecModal() {
  if (!codecModal_) return;
  if (portal_) portal_->dismiss(codecModal_.get());
  codecModal_->setVisible(false);
}
