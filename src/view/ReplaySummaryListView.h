#pragma once

#include "../ReplayDBHelper.h"
#include "../ReplayClearMarkUtils.h"
#include "../ReplaySummaryFormatting.h"
#include "../ScoreRankUtils.h"
#include "ClearLampColors.h"
#include "RecyclerView.h"
#include "TextView.h"
#include "UiTheme.h"
#include "View.h"

#include <functional>
#include <string>
#include <vector>

class ReplaySummaryListItemView : public View {
public:
  ReplaySummaryListItemView() {
    clearLamp = new View();
    textColumn = new View();
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

  void setSummary(const ReplaySummary &summary) {
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
    rankText->setText(currentRank);

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
    const std::string rank = currentRank;
    rankText->setThemedColor(
        [rank] { return ui_theme::scoreRankColor(rank); });
  }

  View *clearLamp = nullptr;
  View *textColumn = nullptr;
  View *scoreColumn = nullptr;
  TextView *titleText = nullptr;
  TextView *detailText = nullptr;
  TextView *scoreText = nullptr;
  TextView *rankText = nullptr;
  std::string currentRank;
};

class ReplaySummaryListView : public RecyclerView<ReplaySummary> {
public:
  ReplaySummaryListView()
      : RecyclerView<ReplaySummary>(
            [](const ReplaySummary &a, const ReplaySummary &b) {
              return a.id == b.id;
            }) {
    itemHeight = 74;
    onCreateView = [](const ReplaySummary &) {
      return new ReplaySummaryListItemView();
    };
    onBind = [](View *view, const ReplaySummary &item, int, bool isSelected) {
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

private:
  int lastSelectedIndex = -1;
};
