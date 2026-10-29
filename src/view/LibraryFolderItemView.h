#pragma once

#include "TextView.h"
#include "View.h"
#include <string>

class Button;
class LibraryFolderItemView : public View {
public:
  LibraryFolderItemView(int x, int y, int width, int height);

  void setItem(const std::string &label, int depth, int count, bool selected,
               int clearRank, bool clearMarkFolder, bool expandable,
               bool expanded, uint32_t icon = 0, bool section = false);
  void setGoalAction(std::function<void()> action);
  void onSelected() override;
  void onUnselected() override;

private:
  Button *goalButton;
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
