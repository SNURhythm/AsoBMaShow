#pragma once

#include "TextView.h"
#include "View.h"
#include <string>

class Button;
class GoalTargetBubble;
class LibraryFolderItemView : public View {
public:
  LibraryFolderItemView(int x, int y, int width, int height, bool goalBubbleInOverlay = false);
  ~LibraryFolderItemView() override;
  void renderGoalBubble(RenderContext &context);

  void setItem(const std::string &label, int depth, int count, bool selected,
               int clearRank, bool clearMarkFolder, bool expandable,
               bool expanded, uint32_t icon = 0, bool section = false);
  void setGoalAction(std::function<void()> action);
  void setGoalState(bool present, bool achieved = false);
  void setGoalTarget(int lampRank, const std::string &lamp, const std::string &score, const std::string &coverage = {});
  void onSelected() override;
  void onUnselected() override;

protected:
  void onLayout() override;
  void onMove(int, int) override;
  void onThemeChanged() override;

private:
  GoalTargetBubble *goalBubble = nullptr;
  bool goalBubbleInOverlay_ = false;
  void refreshGoalButton();
  Button *goalButton;
  TextView *goalIconView;
  TextView *goalOutlineView;
  bool goalPresent = false;
  bool goalAchieved = false;
  bool selected = false;
  View *contentCard;
  TextView *disclosureView;
  View *clearLamp;
  TextView *labelView;
  TextView *groupLabelView;
  TextView *sectionLabelView;
  View *sectionRule;
  bool sectionHeader = false;
  bool rootItem = false;
  TextView *countView;
  int itemDepth = 0;
  int itemClearRank = -1;
  bool itemClearMarkFolder = false;
};
