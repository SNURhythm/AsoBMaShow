#pragma once

#include "AppTheme.h"
#include "Button.h"
#include "DropdownView.h"
#include "IconText.h"
#include "TextView.h"
#include "ToggleButtonContent.h"
#include "../rendering/ResponsiveUi.h"

// Static settings choices. Only opposing states opt into cycling.
// Dynamic device/package/file lists use DropdownView.
class ChoiceControlView final : public View {
public:
  using Select = std::function<bool(const std::string &)>;
  explicit ChoiceControlView(Select select, OverlayPortal *portal = nullptr,
                             int fontSize = 18, float height = 0, bool oppositeStates = false, std::string activeStateId = {})
      : select_(std::move(select)), fontSize_(fontSize), height_(height), oppositeStates_(oppositeStates), activeStateId_(std::move(activeStateId)) {
    View::LayoutBatchScope batch;
    setWidthPercent(100)->setMinWidth(0)->setFlexShrink(0);
    setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch);
    cycle_ = new toggle_button::CompactButton([this] {
      if (!cycleText_) return 96.0f;
      float width = 0;
      const auto label = state_.label.resolve();
      for (const auto &option : state_.options)
        width = std::max(width, static_cast<float>(cycleText_->measureTextWidth(
            label.empty() ? option.label.resolve() : label + ": " + option.label.resolve())));
      return width + cycle_button::kContentInset;
    });
    cycle_->setName("choice-cycle");
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter);
    content->setPadding(Edge::Left, 12)->setPadding(Edge::Right, 12)->setGap(8);
    auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 16);
    icon->setText(ui_icons::textForCodepoint(ui_icons::kRotate));
    icon->setWidth(20)->setFlexShrink(0);
    icon->setThemedColor(app_theme::textSecondary);
    content->addView(icon);
    cycleText_ = new TextView(app_theme::font, fontSize_);
    cycleText_->setFlex(1)->setMinWidth(0);
    cycleText_->setAlign(TextView::CENTER);
    cycleText_->setVAlign(TextView::MIDDLE);
    cycleText_->setThemedColor(app_theme::textPrimary);
    content->addView(cycleText_);
    cycle_->setContentView(content);
    cycle_->setOnClickListener([this] {
      if (state_.options.size() != 2) return;
      choose(state_.options[state_.selectedId == state_.options[0].id ? 1 : 0].id);
    });
    style(cycle_, cycleText_, false, true);
    addView(cycle_);
    buttons_ = new View();
    buttons_->setName("choice-buttons");
    buttons_->setFlexDirection(FlexDirection::Row)->setGap(8);
    addView(buttons_);
    dropdown_ = new DropdownView({.onOptionSelectedResult = [this](const std::string &id) {
      return choose(id);
    }}, portal);
    dropdown_->setName("choice-dropdown");
    dropdown_->setWidthPercent(100);
    addView(dropdown_);
    refresh({});
  }
  ~ChoiceControlView() override { *alive_ = false; }

  void refresh(DropdownView::State state) {
    if (dispatching_) {
      pending_ = std::move(state);
      return;
    }
    View::LayoutBatchScope batch;
    state_ = std::move(state);
    needsUpdate_ = true;
    state_.open = false;
    bool rebuild = items_.size() != state_.options.size();
    if (!rebuild) for (size_t i = 0; i < items_.size(); ++i)
      rebuild = rebuild || items_[i].id != state_.options[i].id;
    if (rebuild) {
      buttons_->clearChildren();
      items_.clear();
      for (const auto &option : state_.options) {
        auto *button = new Button();
        button->setName("choice:" + option.id);
        button->setFlex(1)->setMinWidth(0);
        auto *text = new TextView(app_theme::font, fontSize_);
        text->setAlign(TextView::CENTER);
        text->setVAlign(TextView::MIDDLE);
        button->setContentView(text);
        button->setOnClickListener([this, id = option.id] { choose(id); });
        buttons_->addView(button);
        items_.push_back({button, text, option.id});
      }
    }
    for (size_t i = 0; i < items_.size(); ++i)
      items_[i].text->setLocalizedText(state_.options[i].label);
    dropdown_->refresh(state_);
    update();
  }
  const std::string &selectedId() const { return state_.selectedId; }
  void closeMenu() { auto state = state_; state.open = false; dropdown_->refresh(state); }
  void onLayout() override { update(); }
  void onLanguageChanged() override { View::onLanguageChanged(); needsUpdate_ = true; update(); }

private:
  struct Item { Button *button; TextView *text; std::string id; };
  Select select_;
  DropdownView::State state_;
  std::optional<DropdownView::State> pending_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  bool dispatching_ = false, updating_ = false;
  int fontSize_;
  float height_;
  bool oppositeStates_;
  std::string activeStateId_;
  bool needsUpdate_ = true, lastCompact_ = false;
  float lastWidth_ = -1;
  Button *cycle_ = nullptr;
  TextView *cycleText_ = nullptr;
  View *buttons_ = nullptr;
  DropdownView *dropdown_ = nullptr;
  std::vector<Item> items_;

  static void show(View *view, bool visible) {
    view->setVisible(visible);
    view->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
  }
  static void style(Button *button, TextView *text, bool selected, bool enabled) {
    button->setCornerRadius(app_theme::controlRadius());
    button->setStyledBorderWidth(1);
    button->setSelected(selected);
    if (!enabled) {
      const View::ThemeColorProvider background = selected ? app_theme::panelStrong : app_theme::panelSubtle;
      const View::ThemeColorProvider border = selected ? app_theme::accentBorder : app_theme::hairlineSubtle;
      button->setThemedBackgroundColors(background, background, background);
      button->setThemedBorderColors(border, border, border);
      text->setThemedColor(app_theme::textMuted);
      return;
    }
    button->setThemedBackgroundColors(selected ? app_theme::primaryAction : app_theme::control,
        selected ? app_theme::primaryActionHover : app_theme::controlHover,
        selected ? app_theme::primaryActionPressed : app_theme::controlPressed);
    button->setThemedBorderColors(app_theme::hairline, app_theme::hairlineStrong, app_theme::accentBorder);
    text->setThemedColor(selected ? View::ThemeColorProvider{[] { return app_theme::textOn(app_theme::primaryAction()); }}
                                  : View::ThemeColorProvider{app_theme::textPrimary});
  }
  void update() {
    if (updating_ || !dropdown_) return;
    const bool compact = rendering::compactMenuCanvasActive;
    if (!needsUpdate_ && lastWidth_ == getWidth() && lastCompact_ == compact) return;
    needsUpdate_ = false;
    lastWidth_ = getWidth();
    lastCompact_ = compact;
    updating_ = true;
    {
      View::LayoutBatchScope batch;
      const float height = std::max(height_ > 0 ? height_ : 48.0f,
          rendering::compactMenuCanvasActive ? rendering::compactTouchTarget : 0.0f);
      setHeight(height);
      cycle_->setHeight(height);
      buttons_->setHeight(height);
      dropdown_->setHeight(height);
      float widest = 96;
      for (size_t i = 0; i < items_.size(); ++i) {
        const auto &option = state_.options[i];
        widest = std::max(widest, items_[i].text->measureTextWidth(option.label.resolve()) + 28.0f);
        items_[i].button->setHeight(height);
        items_[i].button->setEnabled(state_.enabled && option.available);
        style(items_[i].button, items_[i].text, option.id == state_.selectedId,
              state_.enabled && option.available);
      }
      const bool knownSelection = std::ranges::any_of(state_.options,
          [this](const auto &option) { return option.id == state_.selectedId; });
      const bool cycle = oppositeStates_ && knownSelection && state_.options.size() == 2 &&
          state_.options[0].available && state_.options[1].available;
      const bool buttons = !cycle && knownSelection && state_.options.size() >= 2 && state_.options.size() <= 4 &&
          getWidth() >= widest * state_.options.size() + 8 * (state_.options.size() - 1);
      const bool menu = !cycle && !buttons;
      if (dropdown_->getVisible() != menu) closeMenu();
      show(cycle_, cycle);
      show(buttons_, buttons);
      show(dropdown_, menu);
      cycle_->setEnabled(state_.enabled);
      const auto activeId = activeStateId_.empty() && !state_.options.empty()
          ? state_.options.back().id : activeStateId_;
      for (const auto &option : state_.options) if (option.id == state_.selectedId) {
        const auto label = state_.label.resolve();
        cycleText_->setText(label.empty() ? option.label.resolve() : label + ": " + option.label.resolve());
        break;
      }
      toggle_button::style(cycle_, state_.selectedId == activeId);
    }
    updating_ = false;
  }
  bool choose(const std::string &id) {
    if (!state_.enabled) return false;
    const auto option = std::ranges::find_if(state_.options, [&](const auto &item) { return item.id == id; });
    if (option == state_.options.end() || !option->available) return false;
    if (state_.selectedId == id) return true;
    const auto previous = state_.selectedId;
    state_.selectedId = id;
    const auto alive = alive_;
    const auto callback = select_;
    dispatching_ = true;
    const bool accepted = !callback || callback(id);
    if (!*alive) return accepted;
    dispatching_ = false;
    if (!accepted) state_.selectedId = previous;
    if (pending_) {
      auto state = std::move(*pending_);
      pending_.reset();
      View::deferAfterEvent([this, alive, state = std::move(state)]() mutable {
        if (*alive) refresh(std::move(state));
      });
    } else {
      dropdown_->refresh(state_);
      needsUpdate_ = true;
      update();
    }
    return accepted;
  }
};
