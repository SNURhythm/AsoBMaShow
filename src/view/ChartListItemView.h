#pragma once
#include "TextView.h"
#include "View.h"
#include "ImageView.h"
#include <SDL2/SDL.h>
#include <string>
#include "../bms_parser.hpp"

class ChartListItemView : public View {
public:
  ChartListItemView(int x, int y, int width, int height,
                    const bms_parser::ChartMeta &meta);

  void setMeta(const bms_parser::ChartMeta &meta);
  void setClearRank(int clearRank);
  void onSelected() override;
  void onUnselected() override;

private:
  View *clearLamp;
  View *artworkFrame;
  ImageView *jacketImage;
  View *textLayout;
  View *detailsLayout;
  TextView *titleView;
  TextView *artistView;
  TextView *levelView;
  TextView *keyModeView;
};
