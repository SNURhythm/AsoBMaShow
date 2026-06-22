#pragma once

#include "../PlayOptionUtils.h"
#include "../ReplayDBHelper.h"
#include "ClearLampColors.h"
#include "RecyclerView.h"
#include "TextView.h"
#include "UiTheme.h"
#include "View.h"

#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace replay_summary_ui {

inline std::string formatGauge(float gauge) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(1) << gauge << "%";
  return stream.str();
}

inline std::string gaugeLabel(GaugeType gaugeType, bool autoShift) {
  return autoShift ? "GAS" : gaugeTypeToShortLabel(gaugeType);
}

inline std::string playOptionLabel(const ReplaySummary &summary) {
  return play_options::formatPlayOptionLabel(
      summary.playOption, summary.playOptionSeed, summary.playOption2,
      summary.playOption2Seed);
}

} // namespace replay_summary_ui

class ReplaySummaryListItemView : public View {
public:
  ReplaySummaryListItemView() {
    clearLamp = new View();
    textColumn = new View();
    titleText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    detailText = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    scoreText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);

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

    scoreText->setWidth(140)->setHeight(32);
    scoreText->setAlign(TextView::TextAlign::RIGHT);
    scoreText->setVAlign(TextView::TextVAlign::MIDDLE);
    addView(scoreText);
    onUnselected();
  }

  void setSummary(const ReplaySummary &summary) {
    titleText->setText(summary.createdAt.empty()
                           ? "Replay #" + std::to_string(summary.id)
                           : summary.createdAt);
    std::string detail = replay_summary_ui::gaugeLabel(summary.initialGaugeType,
                                                       summary.gaugeAutoShift) +
                         "  Gauge " +
                         replay_summary_ui::formatGauge(summary.finalGauge) +
                         "  Events " + std::to_string(summary.eventCount);
    if (summary.touchSampleCount > 0) {
      detail += "  Touches " + std::to_string(summary.touchSampleCount);
    }
    const std::string optionLabel = replay_summary_ui::playOptionLabel(summary);
    if (!optionLabel.empty()) {
      detail += "  " + optionLabel;
    }
    if (assist_options::isEnabled(summary.assistOption)) {
      detail += "  Assist " + assist_options::normalize(summary.assistOption);
    }
    detailText->setText(detail);
    scoreText->setText(std::to_string(summary.finalScore));

    if (hasClearLampColor(summary.clearType)) {
      clearLamp->setBackgroundColor(clearLampColorForRank(summary.clearType));
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
  }

private:
  View *clearLamp = nullptr;
  View *textColumn = nullptr;
  TextView *titleText = nullptr;
  TextView *detailText = nullptr;
  TextView *scoreText = nullptr;
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
