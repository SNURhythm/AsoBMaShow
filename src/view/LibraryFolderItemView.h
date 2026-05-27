#pragma once

#include "TextView.h"
#include "View.h"
#include <string>

class LibraryFolderItemView : public View {
public:
  LibraryFolderItemView(int x, int y, int width, int height);

  void setItem(const std::string &label, int depth, int count, bool selected,
               int clearRank);
  void onSelected() override;
  void onUnselected() override;

private:
  View *clearLamp;
  TextView *labelView;
  TextView *countView;
  int itemDepth = 0;
};
