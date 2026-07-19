#pragma once

#include "../repositories/ReplayRepository.h"
#include "../ReplayClearMarkUtils.h"
#include "../ReplaySummaryFormatting.h"
#include "../ScoreRankUtils.h"
#include "ClearLampColors.h"
#include "Button.h"
#include "IconText.h"
#include "RecyclerView.h"
#include "TextView.h"
#include "UiTheme.h"
#include "View.h"

#include <functional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace replay_summary_list_ui {

inline Color blockedYellow() { return Color(255, 224, 92, 255); }

struct IrBadgeBinding {
  bool visible = false;
  bool actionable = false;
  std::uint32_t codepoint = 0;
  Color (*accent)() = ui_theme::amber;
};

inline IrBadgeBinding bindingForIrRecordState(
    ir::IrRecordState state) noexcept {
  switch (state) {
  case ir::IrRecordState::Hidden:
    return {};
  case ir::IrRecordState::Eligible:
    return {.visible = true,
            .actionable = true,
            .codepoint = 0xf0ee,
            .accent = ui_theme::amber};
  case ir::IrRecordState::Queued:
    return {.visible = true,
            .actionable = false,
            .codepoint = 0xf017,
            .accent = ui_theme::amber};
  case ir::IrRecordState::Uploading:
    return {.visible = true,
            .actionable = false,
            .codepoint = 0xf2f1,
            .accent = ui_theme::cyan};
  case ir::IrRecordState::AwaitingRemote:
    return {.visible = true,
            .actionable = false,
            .codepoint = 0xf252,
            .accent = ui_theme::cyan};
  case ir::IrRecordState::Blocked:
    return {.visible = true,
            .actionable = false,
            .codepoint = 0xf084,
            .accent = blockedYellow};
  case ir::IrRecordState::Failed:
    return {.visible = true,
            .actionable = true,
            .codepoint = 0xf071,
            .accent = ui_theme::coral};
  case ir::IrRecordState::Uploaded:
    return {.visible = true,
            .actionable = false,
            .codepoint = 0xf00c,
            .accent = ui_theme::lime};
  }
  return {};
}

} // namespace replay_summary_list_ui

class ReplaySummaryListItemView : public View {
public:
  ReplaySummaryListItemView() {
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

    clearLamp->setWidth(5)->setHeight(52)->setFlexShrink(0);
    clearLamp->setCornerRadius(3.0f);
    addView(clearLamp);

    textColumn->setFlexDirection(FlexDirection::Column)
        ->setJustifyContent(YGJustifyCenter)
        ->setFlexGrow(1)
        ->setFlexBasis(0)
        ->setMinWidth(0)
        ->setGap(4);
    titleText->setHeight(28);
    titleText->setOverflow(TextView::TextOverflow::Marquee);
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
    irBadge->setOnClickListener([this]() {
      if (irBadgeActionable && irUploadHandler) {
        irUploadHandler(currentSummary);
      } else if (!irBadgeActionable && irStatusFeedbackHandler) {
        irStatusFeedbackHandler(currentSummary);
      }
    });
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

    scoreText->setWidth(150)->setHeight(28);
    scoreText->setAlign(TextView::TextAlign::RIGHT);
    scoreText->setVAlign(TextView::TextVAlign::MIDDLE);
    scoreText->setOverflow(TextView::TextOverflow::Hidden);
    scoreColumn->addView(scoreText);

    rankText->setWidth(150)->setHeight(20);
    rankText->setAlign(TextView::TextAlign::RIGHT);
    rankText->setVAlign(TextView::TextVAlign::MIDDLE);
    rankText->setOverflow(TextView::TextOverflow::Hidden);
    scoreColumn->addView(rankText);

    addView(scoreColumn);
    onUnselected();
  }

  void setIrUploadHandler(
      std::function<void(const ReplaySummary &)> handler) {
    irUploadHandler = std::move(handler);
  }

  void setIrStatusFeedbackHandler(
      std::function<void(const ReplaySummary &)> handler) {
    irStatusFeedbackHandler = std::move(handler);
  }

  [[nodiscard]] std::string irBadgeIconFontPath() const {
    return ui_icons::kFontAwesomeSolidPath;
  }

  void setSummary(const ReplaySummary &summary) {
    currentSummary = summary;
    titleText->setText(summary.autoPlay
                           ? "AUTO PLAY"
                           : (summary.createdAt.empty()
                                  ? "Replay #" + std::to_string(summary.id)
                                  : summary.createdAt));
    detailText->setText(replay_summary_ui::detailLabel(summary));
    scoreText->setText(summary.autoPlay ? "AUTO"
                                        : std::to_string(summary.finalScore));
    currentRank = score_rank::labelForScore(summary.finalScore,
                                            summary.maxScore);
    rankText->setText(score_rank::displayLabelForScore(summary.finalScore,
                                                       summary.maxScore));

    const replay_summary_list_ui::IrBadgeBinding badge =
        replay_summary_list_ui::bindingForIrRecordState(
            summary.irRecordState);
    irBadgeActionable = badge.actionable;
    irBadge->setVisible(badge.visible);
    irBadge->setWidth(badge.visible ? 62.0F : 0.0F);
    // Non-actionable badges stay enabled as pointer event sinks. The bound
    // semantic action flag prevents queued/active/blocked/uploaded clicks from
    // dispatching an upload or falling through to select the row.
    irBadge->setEnabled(badge.visible);
    irBadgeLabel->setText(badge.visible ? "IR" : "");
    irBadgeIcon->setText(
        badge.visible ? ui_icons::textForCodepoint(badge.codepoint) : "");
    const auto accent = badge.accent;
    irBadge->setThemedBackgroundColors(
        accent,
        [accent] { return ui_theme::withAlpha(accent(), 226); },
        [accent] { return ui_theme::withAlpha(accent(), 194); });
    const auto foreground = [accent] { return ui_theme::textOn(accent()); };
    irBadgeLabel->setThemedColor(foreground);
    irBadgeIcon->setThemedColor(foreground);

    const int clearRank = replay_clear_mark::effectiveClearRank(summary);
    if (hasClearLampColor(clearRank)) {
      clearLamp->setBackgroundColor(clearLampColorForRank(clearRank));
    } else {
      clearLamp->clearBackgroundColor();
    }
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
    const std::string rank = score_rank::displayLabel(currentRank);
    rankText->setThemedColor(
        [rank] { return ui_theme::scoreRankColor(rank); });
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
  ReplaySummary currentSummary;
  bool irBadgeActionable = false;
  std::function<void(const ReplaySummary &)> irUploadHandler;
  std::function<void(const ReplaySummary &)> irStatusFeedbackHandler;
};

class ReplaySummaryListView : public RecyclerView<ReplaySummary> {
public:
  ReplaySummaryListView()
      : RecyclerView<ReplaySummary>(
            [](const ReplaySummary &a, const ReplaySummary &b) {
              return a.id == b.id;
            }) {
    itemHeight = 74;
    onCreateView = [this](const ReplaySummary &) {
      auto *itemView = new ReplaySummaryListItemView();
      itemView->setIrUploadHandler([this](const ReplaySummary &summary) {
        if (onIrUploadRequested) {
          onIrUploadRequested(summary);
        }
      });
      itemView->setIrStatusFeedbackHandler(
          [this](const ReplaySummary &summary) {
            if (onIrStatusFeedbackRequested) {
              onIrStatusFeedbackRequested(summary);
            }
          });
      return itemView;
    };
    onBind = [this](View *view, const ReplaySummary &item, int,
                    bool isSelected) {
      auto *itemView = dynamic_cast<ReplaySummaryListItemView *>(view);
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
    onSelected = [this](const ReplaySummary &, int idx) {
      if (lastSelectedIndex >= 0 && lastSelectedIndex < size()) {
        if (auto *oldView = getViewByIndex(lastSelectedIndex)) {
          oldView->onUnselected();
        }
      }
      lastSelectedIndex = idx;
      if (auto *newView = getViewByIndex(idx)) {
        newView->onSelected();
      }
      if (onSelectionChanged != nullptr) {
        onSelectionChanged(idx);
      }
    };
    onUnselected = [this](const ReplaySummary &, int idx) {
      if (auto *view = getViewByIndex(idx)) {
        view->onUnselected();
      }
      if (lastSelectedIndex == idx) {
        lastSelectedIndex = -1;
      }
    };
  }

  void setReplaySummaries(const std::vector<ReplaySummary> &summaries) {
    lastSelectedIndex = -1;
    setItems(summaries);
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

  [[nodiscard]] int selectedReplayIndex() const { return selectedIndex; }

  std::function<void(int)> onSelectionChanged;
  std::function<void(const ReplaySummary &)> onIrUploadRequested;
  std::function<void(const ReplaySummary &)> onIrStatusFeedbackRequested;

private:
  int lastSelectedIndex = -1;
};
