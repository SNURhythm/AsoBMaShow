#include "ReplayRecordsModal.h"

#include "../PlayOptionUtils.h"
#include "../rendering/common.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/IconText.h"
#include "../view/ResultRecordListView.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "../view/View.h"
#include "RemoteResultRecallController.h"
#include "../ReplayVideoExporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr uint32_t kIconXmark = 0xf00d;
constexpr uint32_t kIconFilter = 0xf0b0;
constexpr uint32_t kIconShare = 0xf1e0;
constexpr uint32_t kIconTrash = 0xf1f8;

struct ClearMarkFilterDefinition {
  const char *label;
  int rank;
};

constexpr ClearMarkFilterDefinition kDifficultyClearMarkFilters[] = {
    {"FULL COMBO", kClearTypeFullComboRank},
    {"EXH-CLEAR", kClearTypeExHardClearRank},
    {"H-CLEAR", kClearTypeHardClearRank},
    {"CLEAR", kClearTypeNormalClearRank},
    {"E-CLEAR", kClearTypeEasyClearRank},
    {"LIGHT ASSIST", kClearTypeLightAssistedEasyClearRank},
    {"A-CLEAR", kClearTypeAssistedEasyClearRank},
    {"FAILED", kClearTypeFailedRank},
    {"NO PLAY", kNoClearTypeRank},
};

TextView *makeText(std::string text, int size,
                   View::ThemeColorProvider color) {
  auto *view = new TextView(kFontPath, size);
  view->setText(std::move(text));
  view->setThemedColor(std::move(color));
  view->setVAlign(TextView::MIDDLE);
  view->setWrap(true);
  return view;
}

void styleThemedActionButton(Button *button, TextView *text, bool enabled,
                             View::ThemeColorProvider normal,
                             View::ThemeColorProvider hover,
                             View::ThemeColorProvider pressed,
                             View::ThemeColorProvider border) {
  if (button == nullptr || text == nullptr) {
    return;
  }
  button->setEnabled(enabled);
  button->setCornerRadius(ui_theme::controlRadius());
  if (enabled) {
    button->setThemedBackgroundColors(normal, hover, pressed);
    button->setThemedBorderColors(
        [border] { return ui_theme::withAlpha(border(), 150); },
        [border] { return ui_theme::withAlpha(border(), 190); },
        [border] { return ui_theme::withAlpha(border(), 220); });
    text->setThemedColor([normal] { return ui_theme::textOn(normal()); });
  } else {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
  }
}

void styleOptionButton(Button *button, TextView *text, bool selected) {
  if (selected) {
    styleThemedActionButton(button, text, true, ui_theme::primaryAction,
                            ui_theme::primaryActionHover,
                            ui_theme::primaryActionPressed,
                            ui_theme::accentBorderStrong);
  } else {
    styleThemedActionButton(button, text, true, ui_theme::control,
                            ui_theme::controlHover, ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
  }
}

TextView *makeModalLabel(const std::string &text) {
  auto *label = new TextView(kFontPath, 20);
  label->setText(text);
  label->setThemedColor(ui_theme::textSecondary);
  label->setHeight(28);
  return label;
}

View *makeModalOptionRow(float height = 58.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(12);
  row->setHeight(height);
  return row;
}

Button *makeModalButton(const std::string &label, int fontSize,
                        TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView(kFontPath, fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Button *makeModalIconButton(uint32_t iconCodepoint, int fontSize,
                            TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 54, 54);
  auto *text = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  text->setText(ui_icons::textForCodepoint(iconCodepoint));
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Color modalPanelBorder() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? ui_theme::hairlineStrong()
             : Color(86, 118, 153, 210);
}

} // namespace

std::unique_ptr<ReplayRecordsModal>
ReplayRecordsModal::Create(View *parent,
                           ReplayRecordsModalCallbacks callbacks) {
  if (parent == nullptr) {
    return nullptr;
  }
  auto modal = std::unique_ptr<ReplayRecordsModal>(new ReplayRecordsModal());
  modal->callbacks_ = std::move(callbacks);

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelHeight = 660.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;
  constexpr float kModalContentHeight = 418.0f;

  auto *root = new BlockingOverlayView(0, 0, rendering::window_width,
                                       rendering::window_height);
  root->setPositionType(YGPositionTypeAbsolute);
  root->setPosition(Edge::Left, 0);
  root->setPosition(Edge::Top, 0);
  root->setZIndex(1000);
  root->setVisible(false);
  root->setFlexDirection(FlexDirection::Column);
  root->setAlignItems(YGAlignCenter);
  root->setJustifyContent(YGJustifyCenter);
  root->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(kModalPanelHeight)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10)
      ->setHeight(54);
  auto *title = new TextView(kFontPath, 30);
  title->setText("Records");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(54);
  title->setFlexGrow(1.0f);
  title->setFlexBasis(0.0f);
  title->setMinWidth(0.0f);
  header->addView(title);
  auto *shareButton = makeModalIconButton(kIconShare, 20,
                                          &modal->shareButtonText_);
  shareButton->setWidth(54);
  shareButton->setHeight(54);
  shareButton->setFlexShrink(0.0f);
  auto *deleteButton = makeModalIconButton(kIconTrash, 20,
                                           &modal->deleteButtonText_);
  deleteButton->setWidth(54);
  deleteButton->setHeight(54);
  deleteButton->setFlexShrink(0.0f);
  auto *filterButton = makeModalIconButton(kIconFilter, 20,
                                           &modal->filterButtonText_);
  filterButton->setWidth(54);
  filterButton->setHeight(54);
  filterButton->setFlexShrink(0.0f);
  auto *closeButton = makeModalIconButton(kIconXmark, 22,
                                          &modal->closeButtonText_);
  closeButton->setWidth(54);
  closeButton->setHeight(54);
  closeButton->setFlexShrink(0.0f);
  header->addView(shareButton);
  header->addView(deleteButton);
  header->addView(filterButton);
  header->addView(closeButton);
  panel->addView(header);

  auto *status = makeText({}, 18, ui_theme::textSecondary);
  status->setHeight(30);
  status->setOverflow(TextView::TextOverflow::Hidden);
  status->setVisible(false);
  panel->addView(status);

  auto *contentFrame = new View();
  contentFrame->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setFlexShrink(0);
  panel->addView(contentFrame);

  auto *listContent = new View();
  listContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(10);
  auto *list = new ResultRecordListView();
  list->setIrUploadAvailable(static_cast<bool>(modal->callbacks_.irUpload));
  list->onSelectionChanged = [raw = modal.get()](int idx) {
    raw->select(idx);
  };
  list->onIrUploadRequested =
      [raw = modal.get()](const ResultRecordSummary &record) {
        if (resultRecordActionTarget(record, ResultRecordAction::IrUpload) ==
                ResultRecordActionTarget::ModernChart &&
            record.modern.has_value()) {
          if (raw->callbacks_.irUpload) {
            raw->callbacks_.irUpload(*record.modern);
          }
        }
      };
  list->onIrStatusFeedbackRequested =
      [raw = modal.get()](const ResultRecordSummary &record) {
        if (raw->callbacks_.irStatusFeedback) {
          raw->callbacks_.irStatusFeedback(record.irState);
        }
      };
  list->setFlex(1);
  list->clearBackgroundColor();
  list->setThemedBorderColor(ui_theme::hairline);
  list->setBorderWidth(1);
  listContent->addView(list);
  contentFrame->addView(listContent);

  auto *filterSortContent = new View();
  filterSortContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight);
  filterSortContent->setVisible(false);

  constexpr float kFilterScrollRightPadding = 12.0f;
  constexpr float kFilterContentWidth =
      kModalContentWidth - kFilterScrollRightPadding;
  auto *filterScroll = new ScrollView(0, 0,
                                      static_cast<int>(kModalContentWidth),
                                      static_cast<int>(kModalContentHeight));
  filterScroll->setWidth(kModalContentWidth);
  filterScroll->setHeight(kModalContentHeight);
  filterScroll->setContentPadding(Edge::Right, kFilterScrollRightPadding);

  auto *filterContent = new View();
  filterContent->setFlexDirection(FlexDirection::Column);
  filterContent->setAlignItems(YGAlignStretch);
  filterContent->setGap(10);
  filterContent->setWidth(kFilterContentWidth);

  auto makeFilterButton = [](const std::string &label, int fontSize,
                             TextView **textOut) {
    auto *button = makeModalButton(label, fontSize, textOut);
    button->setHeight(46);
    button->setFlexGrow(1.0f);
    button->setFlexBasis(0.0f);
    button->setFlexShrink(1.0f);
    return button;
  };
  auto addFilterButton = [](View *&row, size_t &index, size_t columns,
                            Button *button, View *filterContent) {
    if (index % columns == 0) {
      row = makeModalOptionRow(46);
      filterContent->addView(row);
    }
    if (row != nullptr) {
      row->addView(button);
    }
    ++index;
  };

  filterContent->addView(makeModalLabel("Clear Mark"));
  View *filterRow = nullptr;
  size_t filterIndex = 0;
  auto makeClearFilterButton = [&modal, &makeFilterButton](
                                   const std::string &label,
                                   std::optional<int> rank,
                                   std::vector<replay_records_modal::ClearFilterButton> &out) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 15, &text);
    button->setOnClickListener([raw = modal.get(), rank]() {
      raw->filters_.clearMarkRank = rank;
      raw->applyFilters();
      raw->refreshFilterSortButtons();
    });
    out.push_back({.button = button, .text = text, .rank = rank});
    return button;
  };
  std::vector<replay_records_modal::ClearFilterButton> clearFilterButtons;
  addFilterButton(filterRow, filterIndex, 3,
                  makeClearFilterButton("All", std::nullopt,
                                        clearFilterButtons),
                  filterContent);
  for (const auto &filter : kDifficultyClearMarkFilters) {
    if (filter.rank == kNoClearTypeRank) {
      continue;
    }
    addFilterButton(filterRow, filterIndex, 3,
                    makeClearFilterButton(filter.label, filter.rank,
                                          clearFilterButtons),
                    filterContent);
  }

  filterContent->addView(makeModalLabel("Play Option"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makePlayOptionFilterButton = [&modal, &makeFilterButton](
                                        const std::string &label,
                                        std::optional<std::string> option,
                                        std::vector<replay_records_modal::OptionFilterButton>
                                            &out) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 14, &text);
    button->setOnClickListener([raw = modal.get(), option]() {
      raw->filters_.playOption =
          option.has_value()
              ? std::optional<std::string>(play_options::normalizePlayOption(
                    *option))
              : std::nullopt;
      raw->applyFilters();
      raw->refreshFilterSortButtons();
    });
    out.push_back({.button = button, .text = text, .option = option});
    return button;
  };
  std::vector<replay_records_modal::OptionFilterButton> playOptionFilterButtons;
  addFilterButton(filterRow, filterIndex, 4,
                  makePlayOptionFilterButton("All", std::nullopt,
                                             playOptionFilterButtons),
                  filterContent);
  for (std::string_view option : play_options::kPlayOptions) {
    const std::string optionName(option);
    addFilterButton(filterRow, filterIndex, 4,
                    makePlayOptionFilterButton(optionName, optionName,
                                               playOptionFilterButtons),
                    filterContent);
  }

  filterContent->addView(makeModalLabel("Score Rank"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makeScoreRankFilterButton = [&modal, &makeFilterButton](
                                       const std::string &label,
                                       std::optional<std::string> rank,
                                       std::vector<replay_records_modal::ScoreRankFilterButton>
                                           &out) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 16, &text);
    button->setOnClickListener([raw = modal.get(), rank]() {
      if (rank.has_value() && !raw->scoreRankFilterAvailable()) {
        return;
      }
      raw->filters_.scoreRank = rank;
      raw->applyFilters();
      raw->refreshFilterSortButtons();
    });
    out.push_back({.button = button, .text = text, .rank = rank});
    return button;
  };
  std::vector<replay_records_modal::ScoreRankFilterButton> scoreRankFilterButtons;
  addFilterButton(filterRow, filterIndex, 4,
                  makeScoreRankFilterButton("All", std::nullopt,
                                            scoreRankFilterButtons),
                  filterContent);
  constexpr std::array<const char *, 10> kScoreRankFilterLabels = {
      "MAX", "MAX -", "AAA", "AA", "A", "B", "C", "D", "E", "F"};
  for (const char *rank : kScoreRankFilterLabels) {
    addFilterButton(filterRow, filterIndex, 4,
                    makeScoreRankFilterButton(rank, std::string(rank),
                                              scoreRankFilterButtons),
                    filterContent);
  }

  filterContent->addView(makeModalLabel("Sort"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makeSortButton = [&modal, &makeFilterButton](
                            const std::string &label,
                            ReplayRecordSortCriterion criterion,
                            std::vector<replay_records_modal::SortButton> &out) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 16, &text);
    button->setOnClickListener([raw = modal.get(), criterion]() {
      raw->filters_.sort = criterion;
      raw->applyFilters();
      raw->refreshFilterSortButtons();
    });
    out.push_back({.button = button, .text = text, .criterion = criterion});
    return button;
  };
  std::vector<replay_records_modal::SortButton> sortButtons;
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Newest", ReplayRecordSortCriterion::Newest,
                                 sortButtons),
                  filterContent);
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Clear Mark",
                                 ReplayRecordSortCriterion::ClearMark,
                                 sortButtons),
                  filterContent);
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Score", ReplayRecordSortCriterion::Score,
                                 sortButtons),
                  filterContent);
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Max Combo",
                                 ReplayRecordSortCriterion::MaxCombo,
                                 sortButtons),
                  filterContent);

  filterScroll->setContentView(filterContent);
  filterSortContent->addView(filterScroll);
  contentFrame->addView(filterSortContent);

  auto *watchOptionsContent = new View();
  watchOptionsContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(12);
  watchOptionsContent->setVisible(false);

  watchOptionsContent->addView(makeModalLabel("Watch Visualization"));
  auto *replayTouchRow = makeModalOptionRow(52.0f);
  auto *replayTouchLabel = makeModalLabel("Touch Points");
  replayTouchLabel->setWidth(180);
  replayTouchLabel->setHeight(52);
  replayTouchLabel->setVAlign(TextView::MIDDLE);
  auto *touchShowButton = makeModalButton("Show", 18,
                                          &modal->touchShowButtonText_);
  auto *touchHideButton = makeModalButton("Hide", 18,
                                          &modal->touchHideButtonText_);
  touchShowButton->setFlex(1);
  touchHideButton->setFlex(1);
  touchShowButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderTouchPoints_ = true;
    raw->refreshExportOptionButtons();
  });
  touchHideButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderTouchPoints_ = false;
    raw->refreshExportOptionButtons();
  });
  replayTouchRow->addView(replayTouchLabel);
  replayTouchRow->addView(touchShowButton);
  replayTouchRow->addView(touchHideButton);
  watchOptionsContent->addView(replayTouchRow);
  auto *replayGhostRow = makeModalOptionRow(52.0f);
  auto *replayGhostLabel = makeModalLabel("Ghosts");
  replayGhostLabel->setWidth(180);
  replayGhostLabel->setHeight(52);
  replayGhostLabel->setVAlign(TextView::MIDDLE);
  auto *ghostShowButton = makeModalButton("Show", 18,
                                          &modal->ghostShowButtonText_);
  auto *ghostHideButton = makeModalButton("Hide", 18,
                                          &modal->ghostHideButtonText_);
  ghostShowButton->setFlex(1);
  ghostHideButton->setFlex(1);
  ghostShowButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderGhosts_ = true;
    raw->refreshExportOptionButtons();
  });
  ghostHideButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderGhosts_ = false;
    raw->refreshExportOptionButtons();
  });
  replayGhostRow->addView(replayGhostLabel);
  replayGhostRow->addView(ghostShowButton);
  replayGhostRow->addView(ghostHideButton);
  watchOptionsContent->addView(replayGhostRow);
  contentFrame->addView(watchOptionsContent);

  auto *exportOptionsContent = new View();
  exportOptionsContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(6);
  exportOptionsContent->setVisible(false);

  exportOptionsContent->addView(makeModalLabel("Frame Rate"));
  auto *fpsRow = makeModalOptionRow();
  auto *fps60Button = makeModalButton("60 fps", 20, &modal->fps60ButtonText_);
  auto *fps120Button =
      makeModalButton("120 fps", 20, &modal->fps120ButtonText_);
  fps60Button->setFlex(1);
  fps120Button->setFlex(1);
  fps60Button->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportFps_ = 60;
    raw->refreshExportOptionButtons();
  });
  fps120Button->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportFps_ = 120;
    raw->refreshExportOptionButtons();
  });
  fpsRow->addView(fps60Button);
  fpsRow->addView(fps120Button);
  exportOptionsContent->addView(fpsRow);

  exportOptionsContent->addView(makeModalLabel("Resolution"));
  auto *resolutionRow = makeModalOptionRow();
  auto *resolution1080Button = makeModalButton(
      "1080p", 20, &modal->resolution1080ButtonText_);
  auto *resolutionFullButton = makeModalButton(
      "Full Resolution", 20, &modal->resolutionFullButtonText_);
  resolution1080Button->setFlex(1);
  resolutionFullButton->setFlex(1);
  resolution1080Button->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportFullResolution_ = false;
    raw->refreshExportOptionButtons();
  });
  resolutionFullButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportFullResolution_ = true;
    raw->refreshExportOptionButtons();
  });
  resolutionRow->addView(resolution1080Button);
  resolutionRow->addView(resolutionFullButton);
  exportOptionsContent->addView(resolutionRow);

  exportOptionsContent->addView(makeModalLabel("Result Screen"));
  auto *resultRow = makeModalOptionRow();
  auto *resultIncludeButton = makeModalButton(
      "Include", 20, &modal->resultIncludeButtonText_);
  auto *resultSkipButton =
      makeModalButton("Skip", 20, &modal->resultSkipButtonText_);
  resultIncludeButton->setFlex(1);
  resultSkipButton->setFlex(1);
  resultIncludeButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportIncludeResultScreen_ = true;
    raw->refreshExportOptionButtons();
  });
  resultSkipButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_) {
      return;
    }
    raw->selectedExportIncludeResultScreen_ = false;
    raw->refreshExportOptionButtons();
  });
  resultRow->addView(resultIncludeButton);
  resultRow->addView(resultSkipButton);
  exportOptionsContent->addView(resultRow);

  auto *exportTouchRow = makeModalOptionRow();
  auto *exportTouchLabel = makeModalLabel("Touch Points");
  exportTouchLabel->setWidth(180);
  exportTouchLabel->setHeight(58);
  exportTouchLabel->setVAlign(TextView::MIDDLE);
  auto *exportTouchShowButton = makeModalButton(
      "Show", 18, &modal->exportTouchShowButtonText_);
  auto *exportTouchHideButton = makeModalButton(
      "Hide", 18, &modal->exportTouchHideButtonText_);
  exportTouchShowButton->setFlex(1);
  exportTouchHideButton->setFlex(1);
  exportTouchShowButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderTouchPoints_ = true;
    raw->refreshExportOptionButtons();
  });
  exportTouchHideButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderTouchPoints_ = false;
    raw->refreshExportOptionButtons();
  });
  exportTouchRow->addView(exportTouchLabel);
  exportTouchRow->addView(exportTouchShowButton);
  exportTouchRow->addView(exportTouchHideButton);
  exportOptionsContent->addView(exportTouchRow);

  auto *exportGhostRow = makeModalOptionRow();
  auto *exportGhostLabel = makeModalLabel("Ghosts");
  exportGhostLabel->setWidth(180);
  exportGhostLabel->setHeight(58);
  exportGhostLabel->setVAlign(TextView::MIDDLE);
  auto *exportGhostShowButton = makeModalButton(
      "Show", 18, &modal->exportGhostShowButtonText_);
  auto *exportGhostHideButton = makeModalButton(
      "Hide", 18, &modal->exportGhostHideButtonText_);
  exportGhostShowButton->setFlex(1);
  exportGhostHideButton->setFlex(1);
  exportGhostShowButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderGhosts_ = true;
    raw->refreshExportOptionButtons();
  });
  exportGhostHideButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->selectedIsAutoPlay()) {
      return;
    }
    raw->selectedReplayRenderGhosts_ = false;
    raw->refreshExportOptionButtons();
  });
  exportGhostRow->addView(exportGhostLabel);
  exportGhostRow->addView(exportGhostShowButton);
  exportGhostRow->addView(exportGhostHideButton);
  exportOptionsContent->addView(exportGhostRow);
  contentFrame->addView(exportOptionsContent);

  auto *exportProgressContent = new View();
  exportProgressContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(18);
  exportProgressContent->setVisible(false);

  auto *exportProgressMessageText = new TextView(kFontPath, 24);
  exportProgressMessageText->setText("Preparing export");
  exportProgressMessageText->setColor(
      ui_theme::sdl(ui_theme::textPrimary()));
  exportProgressMessageText->setHeight(38);
  exportProgressContent->addView(exportProgressMessageText);

  auto *exportProgressTrack = new View();
  exportProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  auto *exportProgressFill = new View();
  exportProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      ui_theme::progressFill());
  exportProgressTrack->addView(exportProgressFill);
  exportProgressContent->addView(exportProgressTrack);

  auto *exportProgressPercentText = new TextView(kFontPath, 22);
  exportProgressPercentText->setText("0%");
  exportProgressPercentText->setColor(
      ui_theme::sdl(ui_theme::textSecondary()));
  exportProgressPercentText->setHeight(34);
  exportProgressPercentText->setAlign(TextView::RIGHT);
  exportProgressContent->addView(exportProgressPercentText);
  contentFrame->addView(exportProgressContent);

  auto *deleteConfirmationContent = new View();
  deleteConfirmationContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(20);
  deleteConfirmationContent->setVisible(false);

  auto *deleteQuestion = new TextView(kFontPath, 28);
  deleteQuestion->setText("Delete this BRD replay file?");
  deleteQuestion->setThemedColor(ui_theme::textPrimary);
  deleteQuestion->setAlign(TextView::CENTER);
  deleteQuestion->setHeight(44);
  deleteConfirmationContent->addView(deleteQuestion);

  auto *deleteDetail = new TextView(kFontPath, 20);
  deleteDetail->setText(
      "Result history will be kept. The BRD file and replay actions cannot "
      "be restored.");
  deleteDetail->setThemedColor(ui_theme::textSecondary);
  deleteDetail->setAlign(TextView::CENTER);
  deleteDetail->setWrap(true);
  deleteDetail->setHeight(72);
  deleteConfirmationContent->addView(deleteDetail);

  auto *deleteConfirmationActions = makeModalOptionRow();
  deleteConfirmationActions->setJustifyContent(YGJustifyCenter);
  auto *deleteCancelButton =
      makeModalButton("Cancel", 20, &modal->deleteCancelButtonText_);
  auto *deleteConfirmButton =
      makeModalButton("Delete Replay", 18, &modal->deleteConfirmButtonText_);
  deleteCancelButton->setWidth(190);
  deleteConfirmButton->setWidth(210);
  deleteCancelButton->setOnClickListener(
      [raw = modal.get()]() { raw->cancelDeleteConfirmation(); });
  deleteConfirmButton->setOnClickListener([raw = modal.get()]() {
    if (raw->documentHandoffActive_ || raw->exportInProgress_ ||
        raw->resultRecallInProgress_ || raw->irUploadInProgress_) {
      return;
    }
    auto request = raw->deleteConfirmation_.confirm();
    if (!request) {
      return;
    }
    if (raw->deleteConfirmationContent_ != nullptr) {
      raw->deleteConfirmationContent_->setVisible(false);
    }
    raw->showListPage();
    if (raw->callbacks_.remove) {
      raw->callbacks_.remove(*request);
    }
  });
  styleThemedActionButton(
      deleteCancelButton, modal->deleteCancelButtonText_, true,
      ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed,
      ui_theme::hairlineStrong);
  styleThemedActionButton(
      deleteConfirmButton, modal->deleteConfirmButtonText_, true,
      ui_theme::warningAction, ui_theme::warningActionHover,
      ui_theme::warningActionPressed, ui_theme::amber);
  deleteConfirmationActions->addView(deleteCancelButton);
  deleteConfirmationActions->addView(deleteConfirmButton);
  deleteConfirmationContent->addView(deleteConfirmationActions);
  contentFrame->addView(deleteConfirmationContent);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(8);
  footer->setHeight(58);

  auto *watchButton = makeModalButton("Watch", 20, &modal->watchButtonText_);
  auto *gbattleButton =
      makeModalButton("G-BATTLE", 18, &modal->gbattleButtonText_);
  auto *resultButton =
      makeModalButton("View Result", 18, &modal->resultButtonText_);
  auto *exportButton =
      makeModalButton("Export Video", 18, &modal->exportButtonText_);

  closeButton->setOnClickListener([raw = modal.get()]() {
    if (raw->operationInProgress()) {
      return;
    }
    raw->hide();
  });
  shareButton->setOnClickListener([raw = modal.get()]() {
    if (raw->operationInProgress() || !raw->selected_.has_value() ||
        raw->deleteConfirmation_.active()) {
      return;
    }
    const auto selection = replay::replayFileActionSelection(
        *raw->selected_, !raw->operationInProgress());
    if (!selection.request || !selection.shareVisible) {
      return;
    }
    if (raw->callbacks_.share) {
      raw->callbacks_.share(*selection.request);
    }
  });
  deleteButton->setOnClickListener([raw = modal.get()]() {
    if (raw->operationInProgress() || !raw->selected_.has_value()) {
      return;
    }
    raw->showDeleteConfirmation();
  });
  filterButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->resultRecallInProgress_ ||
        raw->irUploadInProgress_) {
      return;
    }
    if (raw->filterSortContent_ != nullptr &&
        raw->filterSortContent_->getVisible()) {
      raw->showListPage();
      if (raw->list_ != nullptr) {
        raw->list_->restoreSelection(raw->selectedIndex_);
      }
      raw->refreshActions();
      return;
    }
    raw->showFilterSortOptions();
  });
  watchButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->resultRecallInProgress_ ||
        raw->irUploadInProgress_) {
      return;
    }
    if (!raw->selected_.has_value()) {
      return;
    }
    const auto target = resultRecordActionTarget(
        *raw->selected_, ResultRecordAction::Watch);
    if (target == ResultRecordActionTarget::None) {
      return;
    }
    if (raw->watchOptionsContent_ != nullptr &&
        raw->watchOptionsContent_->getVisible()) {
      raw->dispatchWatch(*raw->selected_);
      return;
    }
    raw->title_->setText("Watch Options");
    raw->listContent_->setVisible(false);
    raw->filterSortContent_->setVisible(false);
    raw->watchOptionsContent_->setVisible(true);
    raw->exportOptionsContent_->setVisible(false);
    raw->exportProgressContent_->setVisible(false);
    raw->deleteConfirmationContent_->setVisible(false);
    raw->refreshExportOptionButtons();
    raw->refreshActions();
    raw->root_->applyYogaLayoutFromRoot();
  });
  gbattleButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->resultRecallInProgress_ ||
        raw->irUploadInProgress_) {
      return;
    }
    if (!raw->selected_.has_value() ||
        resultRecordActionTarget(*raw->selected_,
                                 ResultRecordAction::GBattle) !=
            ResultRecordActionTarget::ModernChart) {
      return;
    }
    if (raw->selected_->modern.has_value() && raw->callbacks_.gbattle) {
      raw->callbacks_.gbattle(raw->record_, *raw->selected_->modern);
    }
  });
  resultButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->resultRecallInProgress_ ||
        raw->irUploadInProgress_ || !raw->selected_.has_value()) {
      return;
    }
    raw->dispatchResult(*raw->selected_);
  });
  exportButton->setOnClickListener([raw = modal.get()]() {
    if (raw->exportInProgress_ || raw->resultRecallInProgress_ ||
        raw->irUploadInProgress_) {
      return;
    }
    if (raw->watchOptionsContent_ != nullptr &&
        raw->watchOptionsContent_->getVisible()) {
      return;
    }
    if (raw->filterSortContent_ != nullptr &&
        raw->filterSortContent_->getVisible()) {
      return;
    }
    if (raw->exportOptionsContent_ != nullptr &&
        raw->exportOptionsContent_->getVisible()) {
      if (!raw->exportSelection_.has_value()) {
        return;
      }
      raw->dispatchExport(*raw->exportSelection_);
      return;
    }
    if (!raw->selected_.has_value() ||
        resultRecordActionTarget(*raw->selected_,
                                 ResultRecordAction::VideoExport) ==
            ResultRecordActionTarget::None) {
      return;
    }
    raw->showExportOptions();
  });
  footer->addView(watchButton);
  footer->addView(gbattleButton);
  footer->addView(resultButton);
  footer->addView(exportButton);
  panel->addView(footer);

  root->addView(panel);
  parent->addView(root);

  modal->root_ = root;
  modal->contentFrame_ = contentFrame;
  modal->listContent_ = listContent;
  modal->filterSortContent_ = filterSortContent;
  modal->watchOptionsContent_ = watchOptionsContent;
  modal->exportOptionsContent_ = exportOptionsContent;
  modal->exportProgressContent_ = exportProgressContent;
  modal->deleteConfirmationContent_ = deleteConfirmationContent;
  modal->exportProgressTrack_ = exportProgressTrack;
  modal->exportProgressFill_ = exportProgressFill;
  modal->title_ = title;
  modal->status_ = status;
  modal->exportProgressMessageText_ = exportProgressMessageText;
  modal->exportProgressPercentText_ = exportProgressPercentText;
  modal->list_ = list;
  modal->watchButton_ = watchButton;
  modal->gbattleButton_ = gbattleButton;
  modal->resultButton_ = resultButton;
  modal->exportButton_ = exportButton;
  modal->shareButton_ = shareButton;
  modal->deleteButton_ = deleteButton;
  modal->deleteCancelButton_ = deleteCancelButton;
  modal->deleteConfirmButton_ = deleteConfirmButton;
  modal->filterButton_ = filterButton;
  modal->closeButton_ = closeButton;
  modal->fps60Button_ = fps60Button;
  modal->fps120Button_ = fps120Button;
  modal->resolution1080Button_ = resolution1080Button;
  modal->resolutionFullButton_ = resolutionFullButton;
  modal->resultIncludeButton_ = resultIncludeButton;
  modal->resultSkipButton_ = resultSkipButton;
  modal->touchShowButton_ = touchShowButton;
  modal->touchHideButton_ = touchHideButton;
  modal->ghostShowButton_ = ghostShowButton;
  modal->ghostHideButton_ = ghostHideButton;
  modal->exportTouchShowButton_ = exportTouchShowButton;
  modal->exportTouchHideButton_ = exportTouchHideButton;
  modal->exportGhostShowButton_ = exportGhostShowButton;
  modal->exportGhostHideButton_ = exportGhostHideButton;
  modal->clearFilterButtons_ = std::move(clearFilterButtons);
  modal->playOptionFilterButtons_ = std::move(playOptionFilterButtons);
  modal->scoreRankFilterButtons_ = std::move(scoreRankFilterButtons);
  modal->sortButtons_ = std::move(sortButtons);
  modal->refreshExportOptionButtons();
  modal->refreshActions();
  return modal;
}

ReplayRecordsModal::ReplayRecordsModal() = default;

ReplayRecordsModal::~ReplayRecordsModal() = default;

bool ReplayRecordsModal::isVisible() const noexcept {
  return root_ != nullptr && root_->getVisible();
}

void ReplayRecordsModal::showChart(const ChartMetaRecord &record) {
  if (root_ == nullptr) {
    return;
  }
  record_ = record;
  exportSelection_.reset();
  irUploadInProgress_ = false;
  deleteConfirmation_.cancel();
  clearSelection();
  selectedReplayRenderTouchPoints_ = touchVisualizationEnabled_;
  selectedReplayRenderGhosts_ = true;
  filters_ = {};
  reloadRecords(false);
  if (title_ != nullptr) title_->setText("Records");
  showListPage();
  setStatus({});
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  refreshFilterSortButtons();
  refreshExportOptionButtons();
  refreshActions();
  root_->applyYogaLayoutFromRoot();
}

void ReplayRecordsModal::hide() {
  if (root_ == nullptr) {
    return;
  }
  if (!canHide()) {
    return;
  }
  root_->setVisible(false);
  deleteConfirmation_.cancel();
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
  clearSelection();
  exportSelection_.reset();
  if (watchButtonText_ != nullptr) watchButtonText_->setText("Watch");
  if (gbattleButtonText_ != nullptr) gbattleButtonText_->setText("G-BATTLE");
  if (resultButtonText_ != nullptr) resultButtonText_->setText("View Result");
  if (exportButtonText_ != nullptr) exportButtonText_->setText("Export Video");
  setStatus({});
}

void ReplayRecordsModal::resize(int width, int height) {
  if (root_ != nullptr) root_->setSize(width, height);
}

bool ReplayRecordsModal::handleEvents(SDL_Event &event) {
  if (!isVisible()) {
    return true;
  }
  if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
      event.key.keysym.sym == SDLK_ESCAPE) {
    hide();
    return false;
  }
  return root_->handleEvents(event);
}

void ReplayRecordsModal::update() { updateTitleReset(); }

void ReplayRecordsModal::setStatus(std::string text) {
  if (status_ == nullptr) {
    return;
  }
  status_->setText(std::move(text));
  status_->setVisible(!text.empty());
  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::setTouchVisualizationEnabled(bool enabled) {
  touchVisualizationEnabled_ = enabled;
}

void ReplayRecordsModal::reloadRecords(bool preserveViewState) {
  std::optional<std::string> preferredStableKey =
      preserveViewState ? stableKey_ : std::nullopt;
  const float previousScrollOffset =
      preserveViewState && list_ != nullptr ? list_->scrollOffset : 0.0F;
  if (callbacks_.loadRecords) {
    records_ = callbacks_.loadRecords(record_);
  } else {
    records_.clear();
  }
  applyFilters(std::move(preferredStableKey));
  if (preserveViewState && list_ != nullptr) {
    list_->scrollOffset = previousScrollOffset;
    list_->rebindVisibleItems();
  }
}

void ReplayRecordsModal::refresh() {
  refreshFilterSortButtons();
  refreshExportOptionButtons();
  refreshActions();
}

void ReplayRecordsModal::setExportInProgress(bool inProgress) {
  exportInProgress_ = inProgress;
  refreshActions();
}

void ReplayRecordsModal::setResultRecallInProgress(bool inProgress) {
  resultRecallInProgress_ = inProgress;
  refreshActions();
}

void ReplayRecordsModal::setIrUploadInProgress(bool inProgress) {
  irUploadInProgress_ = inProgress;
  refreshActions();
}

void ReplayRecordsModal::setLoadInProgress(bool inProgress) {
  loadInProgress_ = inProgress;
  refreshActions();
}

void ReplayRecordsModal::setDocumentHandoffActive(bool active) {
  documentHandoffActive_ = active;
  refreshActions();
}

void ReplayRecordsModal::showExportProgress(const std::string &title,
                                            const std::string &message) {
  if (root_ == nullptr) {
    return;
  }
  if (title_ != nullptr) title_->setText(title);
  showListPage();
  if (listContent_ != nullptr) listContent_->setVisible(false);
  if (filterSortContent_ != nullptr) filterSortContent_->setVisible(false);
  if (watchOptionsContent_ != nullptr) watchOptionsContent_->setVisible(false);
  if (exportOptionsContent_ != nullptr) exportOptionsContent_->setVisible(false);
  if (exportProgressContent_ != nullptr) exportProgressContent_->setVisible(true);
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
  updateExportProgress(0.0, message);
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  refreshActions();
  root_->applyYogaLayoutFromRoot();
}

void ReplayRecordsModal::updateExportProgress(double fraction,
                                              const std::string &message) {
  const double clamped = std::clamp(fraction, 0.0, 1.0);
  const int displayedPercent = static_cast<int>(std::lround(clamped * 100.0));
  if (exportProgressMessageText_ != nullptr) {
    exportProgressMessageText_->setText(message);
  }
  if (exportProgressPercentText_ != nullptr) {
    exportProgressPercentText_->setText(std::to_string(displayedPercent) + "%");
  }
  if (exportProgressFill_ != nullptr) {
    exportProgressFill_->setWidthPercent(static_cast<float>(displayedPercent));
  }
  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::returnToList(const std::string &status) {
  if (root_ == nullptr) {
    return;
  }
  if (title_ != nullptr) title_->setText("Records");
  showListPage();
  exportSelection_.reset();
  if (list_ != nullptr) {
    list_->restoreSelection(selectedIndex_);
  }
  refreshActions();
  if (!status.empty()) {
    setStatus(status);
  }
  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::showIrFeedback(const std::string &message) {
  if (title_ != nullptr) {
    title_->setText(message);
  }
  titleResetPending_ = true;
  titleResetAt_ = SDL_GetTicks64() + 1400;
  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::selectRecord(const ResultRecordSummary &record) {
  selected_ = record;
  stableKey_ = record.stableKey();
  selectedIndex_ = -1;
  refreshActions();
}

bool ReplayRecordsModal::activate(ReplayRecordsModalAction action) {
  if (!selected_.has_value()) {
    return false;
  }
  return dispatchAction(action, record_, *selected_, callbacks_);
}

bool ReplayRecordsModal::dispatchAction(
    ReplayRecordsModalAction action, const ChartMetaRecord &record,
    const ResultRecordSummary &summary,
    const ReplayRecordsModalCallbacks &callbacks) {
  if (action == ReplayRecordsModalAction::Watch) {
    const auto target =
        resultRecordActionTarget(summary, ResultRecordAction::Watch);
    if (target == ResultRecordActionTarget::ModernCourse &&
        summary.modernCourse.has_value()) {
      if (!callbacks.watchModernCourse) return false;
      callbacks.watchModernCourse(record, *summary.modernCourse);
      return true;
    }
    if (target == ResultRecordActionTarget::ModernChart &&
        summary.modern.has_value()) {
      if (!callbacks.watchModernChart) return false;
      callbacks.watchModernChart(record, *summary.modern);
      return true;
    }
    if (target == ResultRecordActionTarget::AutoPlay) {
      if (!callbacks.watchAutoPlay) return false;
      callbacks.watchAutoPlay(record);
      return true;
    }
    return false;
  }
  const auto target =
      resultRecordActionTarget(summary, ResultRecordAction::VideoExport);
  if (target == ResultRecordActionTarget::ModernCourse &&
      summary.modernCourse.has_value()) {
    if (!callbacks.exportModernCourse) return false;
    callbacks.exportModernCourse(*summary.modernCourse, {});
    return true;
  }
  if (target == ResultRecordActionTarget::ModernChart &&
      summary.modern.has_value()) {
    if (!callbacks.exportModernChart) return false;
    callbacks.exportModernChart(record, *summary.modern, {});
    return true;
  }
  if (target == ResultRecordActionTarget::AutoPlay) {
    if (!callbacks.exportAutoPlay) return false;
    callbacks.exportAutoPlay(record, {});
    return true;
  }
  return false;
}

void ReplayRecordsModal::select(int index) {
  clearSelection();
  if (index < 0 ||
      index >= static_cast<int>(visibleRecords_.size())) {
    refreshActions();
    return;
  }
  selectedIndex_ = index;
  selected_ = visibleRecords_[static_cast<std::size_t>(index)];
  stableKey_ = selected_->stableKey();
  if (selectedIsAutoPlay()) {
    selectedReplayRenderTouchPoints_ = false;
    selectedReplayRenderGhosts_ = false;
    refreshExportOptionButtons();
  }
  refreshActions();
}

void ReplayRecordsModal::clearSelection() {
  selectedIndex_ = -1;
  selected_.reset();
  stableKey_.reset();
}

void ReplayRecordsModal::applyFilters(
    std::optional<std::string> preferredStableKey) {
  if (!preferredStableKey.has_value()) {
    preferredStableKey = stableKey_;
  }
  visibleRecords_ = replay_record_filters::apply(records_, filters_);
  if (list_ != nullptr) {
    list_->setResultRecords(visibleRecords_);
  }
  clearSelection();
  int restoreIndex = -1;
  if (preferredStableKey.has_value()) {
    for (std::size_t i = 0; i < visibleRecords_.size(); ++i) {
      if (visibleRecords_[i].stableKey() == *preferredStableKey) {
        restoreIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (restoreIndex >= 0) {
    selectedIndex_ = restoreIndex;
    selected_ = visibleRecords_[static_cast<std::size_t>(restoreIndex)];
    stableKey_ = selected_->stableKey();
    if (list_ != nullptr) {
      list_->restoreSelection(restoreIndex);
    }
  }
  refreshActions();
}

void ReplayRecordsModal::refreshActions() {
  const bool filterSortMode =
      filterSortContent_ != nullptr && filterSortContent_->getVisible();
  const bool watchOptionsMode =
      watchOptionsContent_ != nullptr && watchOptionsContent_->getVisible();
  const bool optionsMode =
      exportOptionsContent_ != nullptr && exportOptionsContent_->getVisible();
  const bool progressMode =
      exportProgressContent_ != nullptr && exportProgressContent_->getVisible();
  const bool deleteConfirmationMode =
      deleteConfirmationContent_ != nullptr &&
      deleteConfirmationContent_->getVisible();
  const bool modalOperationInProgress = operationInProgress();
  const auto fileActions =
      selected_.has_value()
          ? replay::replayFileActionSelection(
                *selected_, !modalOperationInProgress)
          : replay::ReplayFileActionSelection{};
  // Owners may omit a callback for an action they do not support.  Presenting
  // a control for an absent callback would look enabled but do nothing, so
  // suppress those controls entirely instead.
  const bool shareAvailable = static_cast<bool>(callbacks_.share);
  const bool removeAvailable = static_cast<bool>(callbacks_.remove);
  const bool recallAvailable =
      static_cast<bool>(callbacks_.recallModernChart) ||
      static_cast<bool>(callbacks_.recallModernCourse) ||
      static_cast<bool>(callbacks_.recallRemote);
  const bool watchVisible =
      selected_.has_value() && !filterSortMode && !deleteConfirmationMode &&
      !optionsMode && !progressMode &&
      resultRecordActionTarget(*selected_, ResultRecordAction::Watch) !=
          ResultRecordActionTarget::None;
  const bool gbattleVisible =
      selected_.has_value() && static_cast<bool>(callbacks_.gbattle) &&
      !filterSortMode && !watchOptionsMode &&
      !optionsMode && !progressMode && !deleteConfirmationMode &&
      resultRecordActionTarget(*selected_, ResultRecordAction::GBattle) ==
          ResultRecordActionTarget::ModernChart;
  const ResultRecordRecallActionState resultAction =
      resultRecordRecallActionState(
          selected_, !filterSortMode && !watchOptionsMode && !optionsMode &&
                         !progressMode && !deleteConfirmationMode,
          modalOperationInProgress);
  const bool resultVisible = recallAvailable && resultAction.visible;
  const bool exportVisible =
      selected_.has_value() && !filterSortMode && !watchOptionsMode &&
      !progressMode && !deleteConfirmationMode &&
      resultRecordActionTarget(*selected_, ResultRecordAction::VideoExport) !=
          ResultRecordActionTarget::None;

  if (closeButtonText_ != nullptr) {
    closeButtonText_->setText(ui_icons::textForCodepoint(kIconXmark));
  }
  if (filterButtonText_ != nullptr) {
    filterButtonText_->setText(ui_icons::textForCodepoint(kIconFilter));
  }
  if (shareButtonText_ != nullptr) {
    shareButtonText_->setText(ui_icons::textForCodepoint(kIconShare));
  }
  if (deleteButtonText_ != nullptr) {
    deleteButtonText_->setText(ui_icons::textForCodepoint(kIconTrash));
  }
  if (watchButtonText_ != nullptr && !loadInProgress_) {
    watchButtonText_->setText("Watch");
  }
  if (gbattleButtonText_ != nullptr && !loadInProgress_) {
    gbattleButtonText_->setText("G-BATTLE");
  }
  if (resultButtonText_ != nullptr) {
    resultButtonText_->setText(resultRecallInProgress_ ? "Loading..."
                                                       : "View Result");
  }
  if (exportButtonText_ != nullptr) {
    exportButtonText_->setText(exportInProgress_ ? "Exporting"
                                                 : "Export Video");
  }

  if (filterButton_ != nullptr) {
    const bool filterVisible =
        !watchOptionsMode && !optionsMode && !progressMode &&
        !deleteConfirmationMode;
    filterButton_->setVisible(filterVisible);
    filterButton_->setWidth(filterVisible ? 54.0f : 0.0f);
  }
  if (shareButton_ != nullptr) {
    const bool visible =
        shareAvailable && fileActions.shareVisible && !deleteConfirmationMode;
    shareButton_->setVisible(visible);
    shareButton_->setWidth(visible ? 54.0F : 0.0F);
  }
  if (deleteButton_ != nullptr) {
    const bool visible =
        removeAvailable && fileActions.deleteVisible && !deleteConfirmationMode;
    deleteButton_->setVisible(visible);
    deleteButton_->setWidth(visible ? 54.0F : 0.0F);
  }
  if (watchButton_ != nullptr) {
    const bool visible = watchVisible;
    watchButton_->setVisible(visible);
    watchButton_->setWidth(
        visible ? (watchOptionsMode ? 160.0F : 124.0F) : 0.0F);
  }
  if (gbattleButton_ != nullptr) {
    const bool visible = gbattleVisible;
    gbattleButton_->setVisible(visible);
    gbattleButton_->setWidth(visible ? 144.0F : 0.0F);
  }
  if (resultButton_ != nullptr) {
    const bool visible = resultVisible;
    resultButton_->setVisible(visible);
    resultButton_->setWidth(visible ? 142.0F : 0.0F);
  }
  if (exportButton_ != nullptr) {
    const bool visible = exportVisible;
    exportButton_->setVisible(visible);
    exportButton_->setWidth(
        visible ? (optionsMode ? 160.0F : 142.0F) : 0.0F);
  }

  styleThemedActionButton(closeButton_, closeButtonText_,
                          !modalOperationInProgress, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  styleThemedActionButton(shareButton_, shareButtonText_,
                          shareAvailable && fileActions.shareVisible &&
                              fileActions.enabled && !deleteConfirmationMode,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(deleteButton_, deleteButtonText_,
                          removeAvailable && fileActions.deleteVisible &&
                              fileActions.enabled && !deleteConfirmationMode,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed, ui_theme::amber);
  if (replay_record_filters::hasActiveCriteria(filters_) || filterSortMode) {
    styleThemedActionButton(
        filterButton_, filterButtonText_,
        !watchOptionsMode && !optionsMode && !progressMode &&
            !modalOperationInProgress,
        ui_theme::primaryAction, ui_theme::primaryActionHover,
        ui_theme::primaryActionPressed, ui_theme::accentBorderStrong);
  } else {
    styleThemedActionButton(
        filterButton_, filterButtonText_,
        !watchOptionsMode && !optionsMode && !progressMode &&
            !modalOperationInProgress,
        ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed,
        ui_theme::hairlineStrong);
  }
  styleThemedActionButton(watchButton_, watchButtonText_,
                          watchVisible && !modalOperationInProgress,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(gbattleButton_, gbattleButtonText_,
                          gbattleVisible && !modalOperationInProgress,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed, ui_theme::amber);
  styleThemedActionButton(resultButton_, resultButtonText_,
                          resultAction.enabled, ui_theme::successAction,
                          ui_theme::successActionHover,
                          ui_theme::successActionPressed, ui_theme::lime);
  styleThemedActionButton(exportButton_, exportButtonText_,
                          exportVisible && !modalOperationInProgress,
                          ui_theme::violetAction, ui_theme::violetActionHover,
                          ui_theme::violetActionPressed,
                          ui_theme::violetActionHover);

  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::refreshFilterSortButtons() {
  for (const replay_records_modal::ClearFilterButton &item : clearFilterButtons_) {
    styleOptionButton(item.button, item.text,
                      item.rank == filters_.clearMarkRank);
  }
  for (const replay_records_modal::OptionFilterButton &item : playOptionFilterButtons_) {
    const std::optional<std::string> normalized =
        item.option.has_value()
            ? std::optional<std::string>(
                  play_options::normalizePlayOption(*item.option))
            : std::nullopt;
    styleOptionButton(item.button, item.text,
                      normalized == filters_.playOption);
  }
  for (const replay_records_modal::ScoreRankFilterButton &item : scoreRankFilterButtons_) {
    if (item.rank.has_value() && !scoreRankFilterAvailable()) {
      styleThemedActionButton(item.button, item.text, false, ui_theme::control,
                              ui_theme::controlHover, ui_theme::controlPressed,
                              ui_theme::hairlineStrong);
    } else {
      styleOptionButton(item.button, item.text,
                        item.rank == filters_.scoreRank);
    }
  }
  for (const replay_records_modal::SortButton &item : sortButtons_) {
    styleOptionButton(item.button, item.text,
                      item.criterion == filters_.sort);
  }
}

void ReplayRecordsModal::refreshExportOptionButtons() {
  const bool autoPlaySelection = selectedIsAutoPlay();
  if (autoPlaySelection) {
    selectedReplayRenderTouchPoints_ = false;
    selectedReplayRenderGhosts_ = false;
  }
  styleOptionButton(fps60Button_, fps60ButtonText_, selectedExportFps_ == 60);
  styleOptionButton(fps120Button_, fps120ButtonText_, selectedExportFps_ == 120);
  styleOptionButton(resolution1080Button_, resolution1080ButtonText_,
                    !selectedExportFullResolution_);
  styleOptionButton(resolutionFullButton_, resolutionFullButtonText_,
                    selectedExportFullResolution_);
  styleOptionButton(resultIncludeButton_, resultIncludeButtonText_,
                    selectedExportIncludeResultScreen_);
  styleOptionButton(resultSkipButton_, resultSkipButtonText_,
                    !selectedExportIncludeResultScreen_);
  if (autoPlaySelection) {
    styleThemedActionButton(touchShowButton_, touchShowButtonText_, false,
                            ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed, ui_theme::hairlineStrong);
    styleOptionButton(touchHideButton_, touchHideButtonText_, true);
    styleThemedActionButton(exportTouchShowButton_, exportTouchShowButtonText_,
                            false, ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed, ui_theme::hairlineStrong);
    styleOptionButton(exportTouchHideButton_, exportTouchHideButtonText_, true);
    styleThemedActionButton(ghostShowButton_, ghostShowButtonText_, false,
                            ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed, ui_theme::hairlineStrong);
    styleOptionButton(ghostHideButton_, ghostHideButtonText_, true);
    styleThemedActionButton(exportGhostShowButton_, exportGhostShowButtonText_,
                            false, ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed, ui_theme::hairlineStrong);
    styleOptionButton(exportGhostHideButton_, exportGhostHideButtonText_, true);
    return;
  }
  styleOptionButton(touchShowButton_, touchShowButtonText_,
                    selectedReplayRenderTouchPoints_);
  styleOptionButton(touchHideButton_, touchHideButtonText_,
                    !selectedReplayRenderTouchPoints_);
  styleOptionButton(exportTouchShowButton_, exportTouchShowButtonText_,
                    selectedReplayRenderTouchPoints_);
  styleOptionButton(exportTouchHideButton_, exportTouchHideButtonText_,
                    !selectedReplayRenderTouchPoints_);
  styleOptionButton(ghostShowButton_, ghostShowButtonText_,
                    selectedReplayRenderGhosts_);
  styleOptionButton(ghostHideButton_, ghostHideButtonText_,
                    !selectedReplayRenderGhosts_);
  styleOptionButton(exportGhostShowButton_, exportGhostShowButtonText_,
                    selectedReplayRenderGhosts_);
  styleOptionButton(exportGhostHideButton_, exportGhostHideButtonText_,
                    !selectedReplayRenderGhosts_);
}

void ReplayRecordsModal::showFilterSortOptions() {
  if (root_ == nullptr || filterSortContent_ == nullptr) {
    return;
  }
  if (title_ != nullptr) title_->setText("Filter / Sort");
  if (listContent_ != nullptr) listContent_->setVisible(false);
  filterSortContent_->setVisible(true);
  if (watchOptionsContent_ != nullptr) watchOptionsContent_->setVisible(false);
  if (exportOptionsContent_ != nullptr) exportOptionsContent_->setVisible(false);
  if (exportProgressContent_ != nullptr) exportProgressContent_->setVisible(false);
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
  exportSelection_.reset();
  refreshFilterSortButtons();
  refreshActions();
  root_->applyYogaLayoutFromRoot();
}

void ReplayRecordsModal::showExportOptions() {
  if (root_ == nullptr || !selected_.has_value() ||
      resultRecordActionTarget(*selected_, ResultRecordAction::VideoExport) ==
          ResultRecordActionTarget::None) {
    return;
  }
  exportSelection_ = selected_;
  if (title_ != nullptr) title_->setText("Export Options");
  if (listContent_ != nullptr) listContent_->setVisible(false);
  if (filterSortContent_ != nullptr) filterSortContent_->setVisible(false);
  if (watchOptionsContent_ != nullptr) watchOptionsContent_->setVisible(false);
  if (exportOptionsContent_ != nullptr) exportOptionsContent_->setVisible(true);
  if (exportProgressContent_ != nullptr) exportProgressContent_->setVisible(false);
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
  selectedExportFps_ = 120;
  selectedExportFullResolution_ = true;
  selectedExportIncludeResultScreen_ = true;
  if (exportSelection_->autoPlay) {
    selectedReplayRenderTouchPoints_ = false;
    selectedReplayRenderGhosts_ = false;
  }
  refreshExportOptionButtons();
  refreshActions();
  root_->applyYogaLayoutFromRoot();
}

void ReplayRecordsModal::showDeleteConfirmation() {
  if (!selected_.has_value() || documentHandoffActive_ || exportInProgress_ ||
      resultRecallInProgress_ || irUploadInProgress_) {
    return;
  }
  const auto selection =
      replay::replayFileActionSelection(*selected_, true);
  if (deleteConfirmationContent_ == nullptr ||
      !deleteConfirmation_.begin(selection)) {
    return;
  }
  if (title_ != nullptr) title_->setText("Confirm Replay Deletion");
  if (listContent_ != nullptr) listContent_->setVisible(false);
  if (filterSortContent_ != nullptr) filterSortContent_->setVisible(false);
  if (watchOptionsContent_ != nullptr) watchOptionsContent_->setVisible(false);
  if (exportOptionsContent_ != nullptr) exportOptionsContent_->setVisible(false);
  if (exportProgressContent_ != nullptr) exportProgressContent_->setVisible(false);
  deleteConfirmationContent_->setVisible(true);
  refreshActions();
  root_->applyYogaLayoutFromRoot();
}

void ReplayRecordsModal::cancelDeleteConfirmation() {
  deleteConfirmation_.cancel();
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
  showListPage();
  if (title_ != nullptr) title_->setText("Records");
  if (list_ != nullptr) {
    list_->restoreSelection(selectedIndex_);
  }
  refreshActions();
  if (root_ != nullptr) {
    root_->applyYogaLayoutFromRoot();
  }
}

void ReplayRecordsModal::dispatchWatch(const ResultRecordSummary &summary) {
  const auto target =
      resultRecordActionTarget(summary, ResultRecordAction::Watch);
  if (target == ResultRecordActionTarget::ModernCourse &&
      summary.modernCourse.has_value()) {
    if (callbacks_.watchModernCourse) {
      callbacks_.watchModernCourse(record_, *summary.modernCourse);
    }
    return;
  }
  if (target == ResultRecordActionTarget::ModernChart &&
      summary.modern.has_value()) {
    if (callbacks_.watchModernChart) {
      callbacks_.watchModernChart(record_, *summary.modern);
    }
    return;
  }
  if (target == ResultRecordActionTarget::AutoPlay) {
    if (callbacks_.watchAutoPlay) {
      callbacks_.watchAutoPlay(record_);
    }
  }
}

void ReplayRecordsModal::dispatchExport(const ResultRecordSummary &summary) {
  const auto target =
      resultRecordActionTarget(summary, ResultRecordAction::VideoExport);
  if (target == ResultRecordActionTarget::None) {
    return;
  }
  ReplayVideoExportOptions options;
  options.fps = selectedExportFps_;
  options.includeResultScreen = selectedExportIncludeResultScreen_;
  options.renderTouchPoints =
      summary.autoPlay ? false : selectedReplayRenderTouchPoints_;
  options.renderReplayGhosts =
      summary.autoPlay ? false : selectedReplayRenderGhosts_;
  if (!selectedExportFullResolution_) {
    options.height = 1080;
  }
  if (target == ResultRecordActionTarget::ModernCourse &&
      summary.modernCourse.has_value()) {
    if (callbacks_.exportModernCourse) {
      callbacks_.exportModernCourse(*summary.modernCourse, options);
    }
    return;
  }
  if (target == ResultRecordActionTarget::ModernChart &&
      summary.modern.has_value()) {
    if (callbacks_.exportModernChart) {
      callbacks_.exportModernChart(record_, *summary.modern, options);
    }
    return;
  }
  if (target == ResultRecordActionTarget::AutoPlay) {
    if (callbacks_.exportAutoPlay) {
      callbacks_.exportAutoPlay(record_, options);
    }
  }
}

void ReplayRecordsModal::dispatchResult(const ResultRecordSummary &summary) {
  const auto target =
      resultRecordActionTarget(summary, ResultRecordAction::ResultRecall);
  if (target == ResultRecordActionTarget::ModernCourse &&
      summary.modernCourse.has_value()) {
    if (callbacks_.recallModernCourse) {
      callbacks_.recallModernCourse(*summary.modernCourse,
                                    summary.capabilities.retrySame);
    }
    return;
  }
  if (target == ResultRecordActionTarget::ModernChart &&
      summary.modern.has_value()) {
    if (callbacks_.recallModernChart) {
      callbacks_.recallModernChart(record_, *summary.modern);
    }
    return;
  }
  const auto *remoteIdentity =
      std::get_if<IrRemoteRecordId>(&summary.identity);
  if (target != ResultRecordActionTarget::Remote ||
      remoteIdentity == nullptr || !stableKey_.has_value()) {
    return;
  }
  if (callbacks_.recallRemote) {
    callbacks_.recallRemote(*remoteIdentity, *stableKey_);
  }
}

void ReplayRecordsModal::showListPage() {
  if (listContent_ != nullptr) listContent_->setVisible(true);
  if (filterSortContent_ != nullptr) filterSortContent_->setVisible(false);
  if (watchOptionsContent_ != nullptr) watchOptionsContent_->setVisible(false);
  if (exportOptionsContent_ != nullptr) exportOptionsContent_->setVisible(false);
  if (exportProgressContent_ != nullptr) exportProgressContent_->setVisible(false);
  if (deleteConfirmationContent_ != nullptr) {
    deleteConfirmationContent_->setVisible(false);
  }
}

bool ReplayRecordsModal::selectedIsAutoPlay() const {
  if (exportOptionsContent_ != nullptr &&
      exportOptionsContent_->getVisible() &&
      exportSelection_.has_value()) {
    return exportSelection_->autoPlay;
  }
  return selected_.has_value() && selected_->autoPlay;
}

bool ReplayRecordsModal::scoreRankFilterAvailable() const {
  return replay_record_filters::supportsScoreRankFilter(records_);
}

bool ReplayRecordsModal::operationInProgress() const noexcept {
  return exportInProgress_ || resultRecallInProgress_ || irUploadInProgress_ ||
         loadInProgress_ || documentHandoffActive_;
}

bool ReplayRecordsModal::canHide() const noexcept {
  return !operationInProgress();
}

void ReplayRecordsModal::updateTitleReset() {
  if (!titleResetPending_) {
    return;
  }
  if (irUploadInProgress_) {
    return;
  }
  if (SDL_GetTicks64() < titleResetAt_) {
    return;
  }
  titleResetPending_ = false;
  if (root_ != nullptr && root_->getVisible() && listContent_ != nullptr &&
      listContent_->getVisible() && title_ != nullptr) {
    title_->setText("Records");
    root_->applyYogaLayoutFromRoot();
  }
}