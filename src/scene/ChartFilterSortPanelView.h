#pragma once

#include "../ChartRecordFilters.h"
#include "../ChartDBHelper.h"
#include "../view/View.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class Button;
class DropdownView;
class TextInputBox;
class TextView;

class ChartFilterPanelView : public View {
public:
  struct State {
    ChartRecordFilters filters;
    std::string bpmMinText;
    std::string bpmMaxText;
    bool clearMarkFilterVisible = true;
    std::optional<int> effectiveClearMarkRank;
    bool clearMarkDropdownOpen = false;
    bool scoreRankDropdownOpen = false;
    bool difficultyRangeEnabled = false;
    bool difficultyMinDropdownOpen = false;
    bool difficultyMaxDropdownOpen = false;
    std::vector<DifficultyLevelInfo> difficultyLevels;
  };

  struct Callbacks {
    std::function<void(std::optional<int>)> onClearMarkChanged;
    std::function<void(std::optional<std::string>)> onScoreRankChanged;
    std::function<void(const std::string &)> onBpmMinChanged;
    std::function<void(const std::string &)> onBpmMaxChanged;
    std::function<void(std::optional<std::string>)> onDifficultyMinChanged;
    std::function<void(std::optional<std::string>)> onDifficultyMaxChanged;
    std::function<void(bool)> onClearMarkDropdownChanged;
    std::function<void(bool)> onScoreRankDropdownChanged;
    std::function<void(bool, bool)> onClearMarkRangeChanged;
    std::function<void(bool, bool)> onScoreRankRangeChanged;
    std::function<void(bool, bool)> onDifficultyDropdownChanged;
  };

  explicit ChartFilterPanelView(Callbacks callbacks);

  void refresh(const State &state, bool open);

private:
  Callbacks callbacks;
  State currentState;
  TextView *clearMarkLabel = nullptr;
  View *clearMarkRow = nullptr;
  View *scoreRankRow = nullptr;
  DropdownView *clearMarkDropdown = nullptr;
  DropdownView *scoreRankDropdown = nullptr;
  Button *clearMarkOrAboveButton = nullptr;
  TextView *clearMarkOrAboveIcon = nullptr;
  TextView *clearMarkOrAboveText = nullptr;
  Button *clearMarkOrBelowButton = nullptr;
  TextView *clearMarkOrBelowIcon = nullptr;
  TextView *clearMarkOrBelowText = nullptr;
  Button *scoreRankOrAboveButton = nullptr;
  TextView *scoreRankOrAboveIcon = nullptr;
  TextView *scoreRankOrAboveText = nullptr;
  Button *scoreRankOrBelowButton = nullptr;
  TextView *scoreRankOrBelowIcon = nullptr;
  TextView *scoreRankOrBelowText = nullptr;
  DropdownView *difficultyMinDropdown = nullptr;
  DropdownView *difficultyMaxDropdown = nullptr;
  TextInputBox *bpmMinBox = nullptr;
  TextInputBox *bpmMaxBox = nullptr;
  View *difficultyContent = nullptr;
  View *difficultyRow = nullptr;

  void buildStaticContent();
  void refreshDropdowns(const State &state);
  void refreshRangeButtons(const State &state);
  [[nodiscard]] float preferredHeight(const State &state) const;
};

class ChartSortPanelView : public View {
public:
  struct State {
    ChartRecordSortState sort;
    bool difficultySortEnabled = false;
  };

  struct Callbacks {
    std::function<void(ChartRecordSortCriterion)> onSortChanged;
  };

  explicit ChartSortPanelView(Callbacks callbacks);

  void refresh(const State &state, bool open);

private:
  struct SortButton {
    View *cell = nullptr;
    Button *button = nullptr;
    TextView *text = nullptr;
    TextView *icon = nullptr;
    std::string label;
    ChartRecordSortCriterion criterion = ChartRecordSortCriterion::Default;
  };

  Callbacks callbacks;
  std::vector<SortButton> sortButtons;
  std::vector<View *> sortGridRows;
  View *difficultySortCell = nullptr;
  Button *difficultySortButton = nullptr;
  bool difficultySortEnabled = false;
  float lastSortGridWidth = -1.0f;

  void onLayout() override;
  void buildContent();
  void refreshButtons(const State &state);
  void syncSortGridColumnWidths();
};
