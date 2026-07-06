#include "ChartFilterSortPanelView.h"

#include "../scene/play/RhythmState.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../view/DropdownView.h"
#include "../view/IconText.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

constexpr uint32_t kIconCaretUp = 0xf0d8;
constexpr uint32_t kIconCaretDown = 0xf0d7;
constexpr uint32_t kIconSquare = 0xf0c8;
constexpr uint32_t kIconSquareCheck = 0xf14a;

struct ClearMarkFilterDefinition {
  const char *label;
  int rank;
};

constexpr ClearMarkFilterDefinition kClearMarkFilters[] = {
    {"FULL COMBO", kClearTypeFullComboRank},
    {"EXH-CLEAR", kClearTypeExHardClearRank},
    {"H-CLEAR", kClearTypeHardClearRank},
    {"CLEAR", kClearTypeNormalClearRank},
    {"E-CLEAR", kClearTypeEasyClearRank},
    {"A-CLEAR", kClearTypeAssistedEasyClearRank},
    {"FAILED", kClearTypeFailedRank},
    {"NO PLAY", kNoClearTypeRank},
};

void styleActionButton(Button *button, TextView *text, bool enabled,
                       View::ThemeColorProvider normal,
                       View::ThemeColorProvider hover,
                       View::ThemeColorProvider pressed,
                       View::ThemeColorProvider border) {
  if (button == nullptr || text == nullptr) {
    return;
  }
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
    styleActionButton(button, text, true, ui_theme::primaryAction,
                      ui_theme::primaryActionHover,
                      ui_theme::primaryActionPressed,
                      ui_theme::accentBorderStrong);
  } else {
    styleActionButton(button, text, true, ui_theme::control,
                      ui_theme::controlHover, ui_theme::controlPressed,
                      ui_theme::hairlineStrong);
  }
}

void configureSortGridCell(View *view) {
  if (view == nullptr) {
    return;
  }
  view->setHeight(42.0f);
  view->setWidth(160.0f);
  view->setFlexGrow(0.0f);
  view->setFlexBasis(160.0f);
  view->setFlexShrink(0.0f);
}

void configureSortGridContent(View *view) {
  if (view == nullptr) {
    return;
  }
  view->setHeight(42.0f);
  view->setWidth(160.0f);
  view->setFlexGrow(0.0f);
  view->setFlexBasis(160.0f);
  view->setFlexShrink(0.0f);
}

void setDisplayed(View *view, bool displayed) {
  if (view == nullptr) {
    return;
  }
  view->setVisible(displayed);
  view->setDisplay(displayed ? YGDisplayFlex : YGDisplayNone);
}

TextView *makePanelLabel(const std::string &text) {
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setText(text);
  label->setThemedColor(ui_theme::textSecondary);
  label->setHeight(28);
  return label;
}

View *makePanelOptionRow(float height = 42.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(8);
  row->setHeight(height);
  return row;
}

Button *makeCheckboxButton(const std::string &label, TextView **iconOut,
                           TextView **textOut) {
  auto *button = new Button(0, 0, 118, 42);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row);
  content->setAlignItems(YGAlignCenter);
  content->setJustifyContent(YGJustifyCenter);
  content->setPadding(Edge::Left, 8);
  content->setPadding(Edge::Right, 8);
  content->setGap(6);
  auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 14);
  icon->setAlign(TextView::CENTER);
  icon->setVAlign(TextView::MIDDLE);
  icon->setWidth(16);
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 14);
  text->setText(label);
  text->setAlign(TextView::LEFT);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setMinWidth(0);
  text->setFlex(1.0f);
  content->addView(icon);
  content->addView(text);
  button->setContentView(content);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  button->setWidth(118);
  button->setHeight(42);
  button->setFlexShrink(0.0f);
  if (iconOut != nullptr) {
    *iconOut = icon;
  }
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

std::string optionalStringId(const std::optional<std::string> &value) {
  return value.has_value() ? *value : std::string();
}

std::optional<std::string> optionalStringFromId(const std::string &id) {
  if (id.empty()) {
    return std::nullopt;
  }
  return id;
}

std::string clearMarkId(const std::optional<int> &rank) {
  return rank.has_value() ? std::to_string(*rank) : std::string();
}

std::optional<int> clearMarkRankFromId(const std::string &id) {
  if (id.empty()) {
    return std::nullopt;
  }
  return std::stoi(id);
}

std::vector<DropdownView::Option> clearMarkOptions() {
  std::vector<DropdownView::Option> options;
  options.push_back({.id = "", .label = "All"});
  for (const auto &filter : kClearMarkFilters) {
    options.push_back({
        .id = std::to_string(filter.rank),
        .label = filter.label,
        .leadingColor = clearLampColorForRank(filter.rank),
    });
  }
  return options;
}

std::vector<DropdownView::Option> scoreRankOptions() {
  std::vector<DropdownView::Option> options;
  options.push_back({.id = "", .label = "All"});
  constexpr std::array<const char *, 10> kScoreRankLabels = {
      "MAX", "MAX -", "AAA", "AA", "A", "B", "C", "D", "E", "F"};
  for (const char *rank : kScoreRankLabels) {
    options.push_back({.id = rank, .label = rank});
  }
  return options;
}

std::vector<DropdownView::Option>
difficultyOptions(const std::vector<DifficultyLevelInfo> &levels) {
  std::vector<DropdownView::Option> options;
  options.push_back({.id = "", .label = "All"});
  for (const auto &level : levels) {
    options.push_back({.id = level.level, .label = level.level});
  }
  return options;
}

std::optional<int>
effectiveClearMarkRank(const ChartFilterPanelView::State &state) {
  if (state.effectiveClearMarkRank.has_value()) {
    return state.effectiveClearMarkRank;
  }
  return state.filters.clearMarkRank;
}

} // namespace

ChartFilterPanelView::ChartFilterPanelView(Callbacks callbacks)
    : View(), callbacks(std::move(callbacks)) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(10);
  setVisible(false);
  setHeight(0);
  buildStaticContent();
}

void ChartFilterPanelView::buildStaticContent() {
  auto configureDropdown = [](DropdownView *dropdown) {
    if (dropdown == nullptr) {
      return;
    }
    dropdown->setHeight(42);
  };

  clearMarkLabel = makePanelLabel("Clear Mark");
  addView(clearMarkLabel);
  clearMarkRow = makePanelOptionRow();
  clearMarkDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            if (callbacks.onClearMarkDropdownChanged) {
              callbacks.onClearMarkDropdownChanged(open);
            }
          },
      .onOptionSelected =
          [this](const std::string &id) {
            if (callbacks.onClearMarkChanged) {
              callbacks.onClearMarkChanged(clearMarkRankFromId(id));
            }
          },
  });
  configureDropdown(clearMarkDropdown);
  clearMarkRow->addView(clearMarkDropdown);
  clearMarkOrAboveButton = makeCheckboxButton("or above", &clearMarkOrAboveIcon,
                                              &clearMarkOrAboveText);
  clearMarkOrAboveButton->setOnClickListener([this]() {
    if (!currentState.filters.clearMarkRank.has_value()) {
      return;
    }
    const bool next = !currentState.filters.clearMarkOrAbove;
    if (callbacks.onClearMarkRangeChanged) {
      callbacks.onClearMarkRangeChanged(next, false);
    }
  });
  clearMarkRow->addView(clearMarkOrAboveButton);
  clearMarkOrBelowButton = makeCheckboxButton("or below", &clearMarkOrBelowIcon,
                                              &clearMarkOrBelowText);
  clearMarkOrBelowButton->setOnClickListener([this]() {
    if (!currentState.filters.clearMarkRank.has_value()) {
      return;
    }
    const bool next = !currentState.filters.clearMarkOrBelow;
    if (callbacks.onClearMarkRangeChanged) {
      callbacks.onClearMarkRangeChanged(false, next);
    }
  });
  clearMarkRow->addView(clearMarkOrBelowButton);
  addView(clearMarkRow);

  addView(makePanelLabel("Score Rank"));
  scoreRankRow = makePanelOptionRow();
  scoreRankDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            if (callbacks.onScoreRankDropdownChanged) {
              callbacks.onScoreRankDropdownChanged(open);
            }
          },
      .onOptionSelected =
          [this](const std::string &id) {
            if (callbacks.onScoreRankChanged) {
              callbacks.onScoreRankChanged(optionalStringFromId(id));
            }
          },
  });
  configureDropdown(scoreRankDropdown);
  scoreRankRow->addView(scoreRankDropdown);
  scoreRankOrAboveButton = makeCheckboxButton("or above", &scoreRankOrAboveIcon,
                                              &scoreRankOrAboveText);
  scoreRankOrAboveButton->setOnClickListener([this]() {
    if (!currentState.filters.scoreRank.has_value()) {
      return;
    }
    const bool next = !currentState.filters.scoreRankOrAbove;
    if (callbacks.onScoreRankRangeChanged) {
      callbacks.onScoreRankRangeChanged(next, false);
    }
  });
  scoreRankRow->addView(scoreRankOrAboveButton);
  scoreRankOrBelowButton = makeCheckboxButton("or below", &scoreRankOrBelowIcon,
                                              &scoreRankOrBelowText);
  scoreRankOrBelowButton->setOnClickListener([this]() {
    if (!currentState.filters.scoreRank.has_value()) {
      return;
    }
    const bool next = !currentState.filters.scoreRankOrBelow;
    if (callbacks.onScoreRankRangeChanged) {
      callbacks.onScoreRankRangeChanged(false, next);
    }
  });
  scoreRankRow->addView(scoreRankOrBelowButton);
  addView(scoreRankRow);

  addView(makePanelLabel("BPM Range"));
  auto *bpmRow = makePanelOptionRow(48);
  auto makeBpmLabel = [](const std::string &label) {
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
    text->setText(label);
    text->setThemedColor(ui_theme::textSecondary);
    text->setWidth(44);
    text->setVAlign(TextView::MIDDLE);
    return text;
  };
  bpmMinBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 24);
  bpmMinBox->setHeight(48);
  bpmMinBox->setFlex(1);
  bpmMinBox->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  bpmMinBox->setCornerRadius(ui_theme::controlRadius());
  bpmMinBox->setThemedBorderColor(ui_theme::hairlineSubtle);
  bpmMinBox->setBorderWidth(1);
  bpmMinBox->setVAlign(TextView::MIDDLE);
  bpmMinBox->setThemedColor(ui_theme::textPrimary);
  bpmMinBox->onTextChanged([this](const std::string &text) {
    if (callbacks.onBpmMinChanged) {
      callbacks.onBpmMinChanged(text);
    }
  });
  bpmMinBox->onSubmit([this](const std::string &text) {
    if (callbacks.onBpmMinChanged) {
      callbacks.onBpmMinChanged(text);
    }
  });

  bpmMaxBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 24);
  bpmMaxBox->setHeight(48);
  bpmMaxBox->setFlex(1);
  bpmMaxBox->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  bpmMaxBox->setCornerRadius(ui_theme::controlRadius());
  bpmMaxBox->setThemedBorderColor(ui_theme::hairlineSubtle);
  bpmMaxBox->setBorderWidth(1);
  bpmMaxBox->setVAlign(TextView::MIDDLE);
  bpmMaxBox->setThemedColor(ui_theme::textPrimary);
  bpmMaxBox->onTextChanged([this](const std::string &text) {
    if (callbacks.onBpmMaxChanged) {
      callbacks.onBpmMaxChanged(text);
    }
  });
  bpmMaxBox->onSubmit([this](const std::string &text) {
    if (callbacks.onBpmMaxChanged) {
      callbacks.onBpmMaxChanged(text);
    }
  });

  bpmRow->addView(makeBpmLabel("Min"));
  bpmRow->addView(bpmMinBox);
  bpmRow->addView(makeBpmLabel("Max"));
  bpmRow->addView(bpmMaxBox);
  addView(bpmRow);

  difficultyContent = new View();
  difficultyContent->setFlexDirection(FlexDirection::Column);
  difficultyContent->setAlignItems(YGAlignStretch);
  difficultyContent->setGap(8);
  difficultyContent->setVisible(false);
  difficultyContent->setHeight(0);
  difficultyContent->addView(makePanelLabel("Difficulty Range"));
  difficultyRow = makePanelOptionRow();
  difficultyMinDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            if (callbacks.onDifficultyDropdownChanged) {
              callbacks.onDifficultyDropdownChanged(true, open);
            }
          },
      .onOptionSelected =
          [this](const std::string &id) {
            if (callbacks.onDifficultyMinChanged) {
              callbacks.onDifficultyMinChanged(optionalStringFromId(id));
            }
          },
  });
  configureDropdown(difficultyMinDropdown);
  difficultyMaxDropdown = new DropdownView({
      .onOpenChanged =
          [this](bool open) {
            if (callbacks.onDifficultyDropdownChanged) {
              callbacks.onDifficultyDropdownChanged(false, open);
            }
          },
      .onOptionSelected =
          [this](const std::string &id) {
            if (callbacks.onDifficultyMaxChanged) {
              callbacks.onDifficultyMaxChanged(optionalStringFromId(id));
            }
          },
  });
  configureDropdown(difficultyMaxDropdown);
  difficultyRow->addView(difficultyMinDropdown);
  difficultyRow->addView(difficultyMaxDropdown);
  difficultyContent->addView(difficultyRow);
  addView(difficultyContent);
}

void ChartFilterPanelView::refresh(const State &state, bool open) {
  currentState = state;
  if (bpmMinBox != nullptr && bpmMinBox->getText() != state.bpmMinText) {
    bpmMinBox->setEditingText(state.bpmMinText);
  }
  if (bpmMaxBox != nullptr && bpmMaxBox->getText() != state.bpmMaxText) {
    bpmMaxBox->setEditingText(state.bpmMaxText);
  }
  refreshDropdowns(state);
  refreshRangeButtons(state);
  setVisible(open);
  setZIndex(open ? 100 : 0);
  setHeight(open ? preferredHeight(state) : 0.0f);
}

void ChartFilterPanelView::refreshDropdowns(const State &state) {
  if (clearMarkDropdown != nullptr) {
    clearMarkDropdown->refresh({
        .selectedId = clearMarkId(state.filters.clearMarkRank),
        .options = clearMarkOptions(),
        .open = state.clearMarkFilterVisible && state.clearMarkDropdownOpen,
        .enabled = state.clearMarkFilterVisible,
        .maxVisibleItems = 6,
    });
  }
  setDisplayed(clearMarkLabel, state.clearMarkFilterVisible);
  setDisplayed(clearMarkRow, state.clearMarkFilterVisible);
  if (clearMarkRow != nullptr) {
    clearMarkRow->setZIndex(
        state.clearMarkFilterVisible && state.clearMarkDropdownOpen ? 100 : 0);
  }
  const bool scoreRankEnabled =
      chart_record_filters::scoreRankFilterEnabled(
          effectiveClearMarkRank(state));
  if (scoreRankDropdown != nullptr) {
    scoreRankDropdown->refresh({
        .selectedId = optionalStringId(state.filters.scoreRank),
        .options = scoreRankOptions(),
        .open = scoreRankEnabled && state.scoreRankDropdownOpen,
        .enabled = scoreRankEnabled,
        .maxVisibleItems = 6,
    });
  }
  if (scoreRankRow != nullptr) {
    scoreRankRow->setZIndex(scoreRankEnabled && state.scoreRankDropdownOpen
                                ? 100
                                : 0);
  }

  const bool showDifficulty =
      state.difficultyRangeEnabled && !state.difficultyLevels.empty();
  const bool difficultyDropdownOpen =
      showDifficulty &&
      (state.difficultyMinDropdownOpen || state.difficultyMaxDropdownOpen);
  if (difficultyContent != nullptr) {
    difficultyContent->setVisible(showDifficulty);
    difficultyContent->setHeight(showDifficulty ? 78.0f : 0.0f);
    difficultyContent->setZIndex(difficultyDropdownOpen ? 100 : 0);
  }
  if (difficultyRow != nullptr) {
    difficultyRow->setZIndex(difficultyDropdownOpen ? 100 : 0);
  }
  const auto options = difficultyOptions(state.difficultyLevels);
  if (difficultyMinDropdown != nullptr) {
    difficultyMinDropdown->refresh({
        .label = "Min",
        .selectedId = optionalStringId(state.filters.difficultyMinLevel),
        .options = options,
        .open = showDifficulty && state.difficultyMinDropdownOpen,
        .maxVisibleItems = 6,
    });
  }
  if (difficultyMaxDropdown != nullptr) {
    difficultyMaxDropdown->refresh({
        .label = "Max",
        .selectedId = optionalStringId(state.filters.difficultyMaxLevel),
        .options = options,
        .open = showDifficulty && state.difficultyMaxDropdownOpen,
        .maxVisibleItems = 6,
    });
  }
}

void ChartFilterPanelView::refreshRangeButtons(const State &state) {
  auto refreshButton = [](Button *button, TextView *icon, TextView *text,
                          bool enabled, bool selected) {
    if (icon != nullptr) {
      icon->setText(ui_icons::textForCodepoint(selected ? kIconSquareCheck
                                                        : kIconSquare));
      if (selected) {
        icon->setThemedColor(
            [] { return ui_theme::textOn(ui_theme::primaryAction()); });
      } else {
        icon->setThemedColor(ui_theme::textMuted);
      }
    }
    styleActionButton(
        button, text, enabled,
        selected ? ui_theme::primaryAction : ui_theme::control,
        selected ? ui_theme::primaryActionHover : ui_theme::controlHover,
        selected ? ui_theme::primaryActionPressed : ui_theme::controlPressed,
        selected ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong);
  };

  const bool clearEnabled = state.filters.clearMarkRank.has_value();
  refreshButton(clearMarkOrAboveButton, clearMarkOrAboveIcon,
                clearMarkOrAboveText, clearEnabled,
                clearEnabled && state.filters.clearMarkOrAbove);
  refreshButton(clearMarkOrBelowButton, clearMarkOrBelowIcon,
                clearMarkOrBelowText, clearEnabled,
                clearEnabled && !state.filters.clearMarkOrAbove &&
                    state.filters.clearMarkOrBelow);

  const bool scoreEnabled =
      chart_record_filters::scoreRankFilterEnabled(
          effectiveClearMarkRank(state)) &&
      state.filters.scoreRank.has_value();
  refreshButton(scoreRankOrAboveButton, scoreRankOrAboveIcon,
                scoreRankOrAboveText, scoreEnabled,
                scoreEnabled && state.filters.scoreRankOrAbove);
  refreshButton(scoreRankOrBelowButton, scoreRankOrBelowIcon,
                scoreRankOrBelowText, scoreEnabled,
                scoreEnabled && !state.filters.scoreRankOrAbove &&
                    state.filters.scoreRankOrBelow);
}

float ChartFilterPanelView::preferredHeight(const State &state) const {
  constexpr float kBaseHeight = 276.0f;
  constexpr float kClearMarkGroupHeight = 90.0f;
  float height = kBaseHeight;
  if (!state.clearMarkFilterVisible) {
    height -= kClearMarkGroupHeight;
  }
  if (state.difficultyRangeEnabled && !state.difficultyLevels.empty()) {
    height += 78.0f;
  }
  return std::max(0.0f, height);
}

ChartSortPanelView::ChartSortPanelView(Callbacks callbacks)
    : View(), callbacks(std::move(callbacks)) {
  setFlexDirection(FlexDirection::Column);
  setAlignItems(YGAlignStretch);
  setGap(10);
  setVisible(false);
  setHeight(0);
  buildContent();
}

void ChartSortPanelView::buildContent() {
  addView(makePanelLabel("Sort"));
  View *row = nullptr;
  size_t index = 0;
  auto addGridCell = [&](View *cell) {
    if (index % 3 == 0) {
      row = makePanelOptionRow();
      sortGridRows.push_back(row);
      addView(row);
    }
    if (row != nullptr) {
      row->addView(cell);
    }
    ++index;
  };
  auto makeSortCell = [](View *content) {
    auto *cell = new View();
    cell->setFlexDirection(FlexDirection::Row);
    cell->setAlignItems(YGAlignStretch);
    configureSortGridCell(cell);
    configureSortGridContent(content);
    cell->addView(content);
    return cell;
  };
  auto makeSortButton = [this, makeSortCell](const std::string &label,
                                             ChartRecordSortCriterion criterion) {
    auto *button = new Button(0, 0, 160, 42);
    auto *content = new View();
    content->setFlexDirection(FlexDirection::Row);
    content->setAlignItems(YGAlignCenter);
    content->setJustifyContent(YGJustifyCenter);
    content->setGap(6);
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 14);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setMinWidth(0);
    auto *icon = new TextView(ui_icons::kFontAwesomeSolidPath, 13);
    icon->setText("");
    icon->setAlign(TextView::CENTER);
    icon->setVAlign(TextView::MIDDLE);
    icon->setWidth(16);
    content->addView(text);
    content->addView(icon);
    button->setContentView(content);
    button->setStyledBorderWidth(1);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setHeight(42);
    button->setOnClickListener([this, criterion]() {
      if (criterion == ChartRecordSortCriterion::Difficulty &&
          !difficultySortEnabled) {
        return;
      }
      if (callbacks.onSortChanged) {
        callbacks.onSortChanged(criterion);
      }
    });
    auto *cell = makeSortCell(button);
    SortButton item{
        .cell = cell,
        .button = button,
        .text = text,
        .icon = icon,
        .label = label,
        .criterion = criterion,
    };
    sortButtons.push_back(item);
    return item;
  };

  for (const auto &option : kSortOptions) {
    const SortButton sortButton =
        makeSortButton(option.label, option.criterion);
    if (option.criterion == ChartRecordSortCriterion::Difficulty) {
      difficultySortCell = sortButton.cell;
      difficultySortButton = sortButton.button;
    }
    addGridCell(sortButton.cell);
  }
  setDisplayed(difficultySortCell, false);
  setDisplayed(difficultySortButton, false);
}

void ChartSortPanelView::refresh(const State &state, bool open) {
  refreshButtons(state);
  setVisible(open);
  setZIndex(open ? 100 : 0);
  setHeight(open ? 184.0f : 0.0f);
}

void ChartSortPanelView::onLayout() { syncSortGridColumnWidths(); }

void ChartSortPanelView::refreshButtons(const State &state) {
  difficultySortEnabled = state.difficultySortEnabled;
  syncSortGridColumnWidths();
  for (const auto &item : sortButtons) {
    if (item.criterion == ChartRecordSortCriterion::Difficulty &&
        !difficultySortEnabled) {
      setDisplayed(item.cell, false);
      setDisplayed(item.button, false);
      continue;
    }
    setDisplayed(item.cell, true);
    setDisplayed(item.button, true);
    if (item.text != nullptr) {
      item.text->setText(item.label);
    }

    const bool selected = item.criterion == state.sort.criterion;
    styleOptionButton(item.button, item.text, selected);
    if (item.icon == nullptr) {
      continue;
    }
    if (selected && item.criterion != ChartRecordSortCriterion::Default) {
      item.icon->setText(ui_icons::textForCodepoint(
          state.sort.direction == ChartRecordSortDirection::Ascending
              ? kIconCaretUp
              : kIconCaretDown));
      item.icon->setThemedColor(
          [] { return ui_theme::textOn(ui_theme::primaryAction()); });
    } else {
      item.icon->setText("");
      item.icon->setThemedColor(ui_theme::textMuted);
    }
  }
}

void ChartSortPanelView::syncSortGridColumnWidths() {
  constexpr float kColumns = 3.0f;
  constexpr float kColumnGap = 8.0f;
  const float panelWidth = static_cast<float>(getWidth());
  if (!std::isfinite(panelWidth) || panelWidth <= 0.0f) {
    return;
  }

  const float columnWidth =
      std::max(0.0f, (panelWidth - kColumnGap * (kColumns - 1.0f)) / kColumns);
  if (std::abs(columnWidth - lastSortGridWidth) <= 0.5f) {
    return;
  }
  lastSortGridWidth = columnWidth;

  for (auto *row : sortGridRows) {
    if (row == nullptr) {
      continue;
    }
    row->setWidth(panelWidth);
    row->setFlexShrink(0.0f);
  }

  for (const auto &item : sortButtons) {
    if (item.cell != nullptr) {
      item.cell->setWidth(columnWidth);
      item.cell->setFlexGrow(0.0f);
      item.cell->setFlexBasis(columnWidth);
      item.cell->setFlexShrink(0.0f);
    }
    if (item.button != nullptr) {
      item.button->setWidth(columnWidth);
      item.button->setFlexGrow(0.0f);
      item.button->setFlexBasis(columnWidth);
      item.button->setFlexShrink(0.0f);
    }
  }
}
