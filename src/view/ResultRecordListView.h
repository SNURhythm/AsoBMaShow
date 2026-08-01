#pragma once

#include "../ResultRecordFormatting.h"
#include "../ResultRecordSummary.h"
#include "../ScoreRankUtils.h"
#include "Button.h"
#include "ClearLampColors.h"
#include "IconText.h"
#include "RecyclerView.h"
#include "ReplaySummaryListView.h"
#include "TextView.h"
#include "UiTheme.h"
#include "View.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class ResultRecordListItemView : public View {
public:
  ResultRecordListItemView() {
    clearLamp = new View();
    textColumn = new View();
    irBadge = new Button();
    irBadgeContent = new View();
    irBadgeLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 14);
    irBadgeIcon = new TextView(ui_icons::kFontAwesomeSolidPath, 14);
    scoreColumn = new View();
    titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    detailText = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
    rankText = new TextView("assets/fonts/notosanscjkjp.ttf", 14);

    setFlexDirection(FlexDirection::Row)
        ->setAlignItems(YGAlignCenter)
        ->setPadding(Edge::All, 8)
        ->setGap(12);

    clearLamp->setName("recordClearLamp");
    clearLamp->setWidth(5)->setHeight(52)->setFlexShrink(0);
    clearLamp->setCornerRadius(3.0F);
    addView(clearLamp);

    textColumn->setFlexDirection(FlexDirection::Column)
        ->setJustifyContent(YGJustifyCenter)
        ->setFlexGrow(1)
        ->setFlexBasis(0)
        ->setMinWidth(0)
        ->setGap(4);
    titleText->setName("recordTitle");
    titleText->setHeight(28);
    titleText->setOverflow(TextView::TextOverflow::Marquee);
    detailText->setName("recordDetail");
    detailText->setHeight(22);
    detailText->setOverflow(TextView::TextOverflow::Hidden);
    textColumn->addView(titleText);
    textColumn->addView(detailText);
    addView(textColumn);

    irBadge->setWidth(0)->setHeight(28)->setFlexShrink(0);
    irBadge->setCornerRadius(6.0F);
    irBadge->setName("irUploadBadge");
    irBadgeContent->setFlexDirection(FlexDirection::Row)
        ->setAlignItems(YGAlignCenter)
        ->setJustifyContent(YGJustifyCenter)
        ->setGap(6);
    irBadgeLabel->setName("irBadgeLabel");
    irBadgeLabel->setWidth(20)->setHeight(28)->setFlexShrink(0);
    irBadgeLabel->setAlign(TextView::TextAlign::CENTER);
    irBadgeLabel->setVAlign(TextView::TextVAlign::MIDDLE);
    irBadgeLabel->setOverflow(TextView::TextOverflow::Hidden);
    irBadgeIcon->setName("irBadgeIcon");
    irBadgeIcon->setWidth(20)->setHeight(28)->setFlexShrink(0);
    irBadgeIcon->setAlign(TextView::TextAlign::CENTER);
    irBadgeIcon->setVAlign(TextView::TextVAlign::MIDDLE);
    irBadgeIcon->setOverflow(TextView::TextOverflow::Hidden);
    irBadgeContent->addView(irBadgeLabel);
    irBadgeContent->addView(irBadgeIcon);
    irBadge->setContentView(irBadgeContent);
    irBadge->setEnabled(false);
    irBadge->setVisible(false);
    addView(irBadge);

    scoreColumn->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignFlexEnd)
        ->setJustifyContent(YGJustifyCenter)
        ->setWidth(150)
        ->setHeight(52)
        ->setFlexShrink(0)
        ->setGap(2);

    scoreText->setName("recordScore");
    scoreText->setWidth(150)->setHeight(28);
    scoreText->setAlign(TextView::TextAlign::RIGHT);
    scoreText->setVAlign(TextView::TextVAlign::MIDDLE);
    scoreText->setOverflow(TextView::TextOverflow::Hidden);
    scoreColumn->addView(scoreText);

    rankText->setName("recordRank");
    rankText->setWidth(150)->setHeight(20);
    rankText->setAlign(TextView::TextAlign::RIGHT);
    rankText->setVAlign(TextView::TextVAlign::MIDDLE);
    rankText->setOverflow(TextView::TextOverflow::Hidden);
    scoreColumn->addView(rankText);

    addView(scoreColumn);
    onUnselected();
  }

  void
  setIrUploadHandler(std::function<void(const ResultRecordSummary &)> handler) {
    irUploadHandler = std::move(handler);
  }

  void setIrStatusFeedbackHandler(
      std::function<void(const ResultRecordSummary &)> handler) {
    irStatusFeedbackHandler = std::move(handler);
  }

  [[nodiscard]] std::string irBadgeIconFontPath() const {
    return ui_icons::kFontAwesomeSolidPath;
  }

  [[nodiscard]] const std::string &boundStableKey() const noexcept {
    return boundStableKey_;
  }

  [[nodiscard]] const std::optional<std::string> &
  irBadgeCallbackStableKey() const noexcept {
    return irBadgeCallbackStableKey_;
  }

  [[nodiscard]] Color currentIrBadgeAccent() const noexcept {
    return currentIrBadgeAccent_;
  }

  void setSummary(const ResultRecordSummary &summary) {
    // A bind is a complete state replacement: identity and every callback are
    // cleared before any current-record behavior is installed.
    boundStableKey_ = summary.stableKey();
    irBadgeCallbackStableKey_.reset();
    irBadge->setOnClickListener({});

    titleText->setText(
        summary.autoPlay
            ? "AUTO PLAY"
            : (!summary.displayedTime.empty() ? summary.displayedTime
                                              : "IR Record"));
    detailText->setText(result_record_ui::detailLabel(summary));
    scoreText->setText(result_record_ui::scoreLabel(summary));
    currentRank = result_record_ui::scoreRank(summary).value_or("");
    rankText->setText(result_record_ui::secondaryScoreLabel(summary));

    const record_list_ui::IrBadgeBinding badge =
        record_list_ui::bindingForIrRecordState(
            summary.isRemote() ? ir::IrRecordState::Uploaded : summary.irState);
    if (badge.visible) {
      irBadgeCallbackStableKey_ = boundStableKey_;
      const ResultRecordSummary boundSummary = summary;
      if (summary.isLocal() && summary.capabilities.irUpload &&
          badge.actionable) {
        const auto boundHandler = irUploadHandler;
        irBadge->setOnClickListener([boundSummary, boundHandler]() {
          if (boundHandler) {
            boundHandler(boundSummary);
          }
        });
      } else {
        const auto boundHandler = irStatusFeedbackHandler;
        irBadge->setOnClickListener([boundSummary, boundHandler]() {
          if (boundHandler) {
            boundHandler(boundSummary);
          }
        });
      }
    }
    irBadge->setVisible(badge.visible);
    irBadge->setWidth(badge.visible ? 62.0F : 0.0F);
    // Read-only badges remain enabled as event sinks so their taps can never
    // fall through and trigger row selection or a recycled upload action.
    irBadge->setEnabled(badge.visible);
    irBadgeLabel->setText(badge.visible ? "IR" : "");
    irBadgeIcon->setText(
        badge.visible ? ui_icons::textForCodepoint(badge.codepoint) : "");
    const auto accent = badge.accent;
    currentIrBadgeAccent_ = accent();
    irBadge->setThemedBackgroundColors(
        accent, [accent] { return ui_theme::withAlpha(accent(), 226); },
        [accent] { return ui_theme::withAlpha(accent(), 194); });
    const auto foreground = [accent] { return ui_theme::textOn(accent()); };
    irBadgeLabel->setThemedColor(foreground);
    irBadgeIcon->setThemedColor(foreground);

    if (summary.clearRankAvailable && hasClearLampColor(summary.clearRank)) {
      clearLamp->setBackgroundColor(clearLampColorForRank(summary.clearRank));
    } else {
      clearLamp->clearBackgroundColor();
    }
    applyRankColor();
  }

  void onSelected() override {
    setThemedBackgroundColor(ui_theme::panelStrong);
    setCornerRadius(ui_theme::controlRadius());
    setThemedBorderColor(ui_theme::accentBorderStrong);
    setBorderWidth(1);
    titleText->setThemedColor(ui_theme::textPrimary);
    detailText->setThemedColor(ui_theme::textSecondary);
    scoreText->setThemedColor(ui_theme::lime);
    rankText->setThemedColor(ui_theme::amber);
    applyRankColor();
  }

  void onUnselected() override {
    setThemedBackgroundColor(ui_theme::panelSubtle);
    setCornerRadius(ui_theme::controlRadius());
    setThemedBorderColor(ui_theme::hairlineSubtle);
    setBorderWidth(1);
    titleText->setThemedColor(ui_theme::textPrimary);
    detailText->setThemedColor(ui_theme::textMuted);
    scoreText->setThemedColor(
        [] { return ui_theme::withAlpha(ui_theme::cyan(), 218); });
    rankText->setThemedColor(
        [] { return ui_theme::withAlpha(ui_theme::amber(), 218); });
    applyRankColor();
  }

private:
  void applyRankColor() {
    if (currentRank.empty()) {
      rankText->setThemedColor(ui_theme::textMuted);
      return;
    }
    const std::string rank = score_rank::displayLabel(currentRank);
    rankText->setThemedColor([rank] { return ui_theme::scoreRankColor(rank); });
  }

  View *clearLamp = nullptr;
  View *textColumn = nullptr;
  Button *irBadge = nullptr;
  View *irBadgeContent = nullptr;
  TextView *irBadgeLabel = nullptr;
  TextView *irBadgeIcon = nullptr;
  View *scoreColumn = nullptr;
  TextView *titleText = nullptr;
  TextView *detailText = nullptr;
  TextView *scoreText = nullptr;
  TextView *rankText = nullptr;
  std::string currentRank;
  std::string boundStableKey_;
  std::optional<std::string> irBadgeCallbackStableKey_;
  Color currentIrBadgeAccent_;
  std::function<void(const ResultRecordSummary &)> irUploadHandler;
  std::function<void(const ResultRecordSummary &)> irStatusFeedbackHandler;
};

class ResultRecordListView : public RecyclerView<ResultRecordSummary> {
public:
  ResultRecordListView()
      : RecyclerView<ResultRecordSummary>(
            [](const ResultRecordSummary &a, const ResultRecordSummary &b) {
              return a.identity == b.identity;
            }) {
    itemHeight = 74;
    onCreateView = [this](const ResultRecordSummary &) {
      auto *itemView = new ResultRecordListItemView();
      itemView->setIrUploadHandler([this](const ResultRecordSummary &summary) {
        if (onIrUploadRequested) {
          onIrUploadRequested(summary);
        }
      });
      itemView->setIrStatusFeedbackHandler(
          [this](const ResultRecordSummary &summary) {
            if (onIrStatusFeedbackRequested) {
              onIrStatusFeedbackRequested(summary);
            }
          });
      return itemView;
    };
    onBind = [](View *view, const ResultRecordSummary &item, int,
                bool isSelected) {
      auto *itemView = dynamic_cast<ResultRecordListItemView *>(view);
      if (itemView == nullptr) {
        return;
      }
      itemView->setSummary(item);
      if (isSelected) {
        itemView->onSelected();
      } else {
        itemView->onUnselected();
      }
    };
    onSelected = [this](const ResultRecordSummary &, int index) {
      if (lastSelectedIndex >= 0 && lastSelectedIndex < size()) {
        if (auto *oldView = getViewByIndex(lastSelectedIndex)) {
          oldView->onUnselected();
        }
      }
      lastSelectedIndex = index;
      if (auto *newView = getViewByIndex(index)) {
        newView->onSelected();
      }
      if (onSelectionChanged) {
        onSelectionChanged(index);
      }
    };
    onUnselected = [this](const ResultRecordSummary &, int index) {
      if (auto *view = getViewByIndex(index)) {
        view->onUnselected();
      }
      if (lastSelectedIndex == index) {
        lastSelectedIndex = -1;
      }
    };
  }

  void setResultRecords(const std::vector<ResultRecordSummary> &records) {
    lastSelectedIndex = -1;
    setItems(records);
  }

  void restoreSelection(int index) {
    lastSelectedIndex = index >= 0 && index < size() ? index : -1;
    selectedIndex = lastSelectedIndex;
    if (lastSelectedIndex >= 0) {
      if (auto *selectedView = getViewByIndex(lastSelectedIndex)) {
        selectedView->onSelected();
      }
    }
  }

  [[nodiscard]] int selectedResultRecordIndex() const { return selectedIndex; }

  std::function<void(int)> onSelectionChanged;
  std::function<void(const ResultRecordSummary &)> onIrUploadRequested;
  std::function<void(const ResultRecordSummary &)> onIrStatusFeedbackRequested;

private:
  int lastSelectedIndex = -1;
};
