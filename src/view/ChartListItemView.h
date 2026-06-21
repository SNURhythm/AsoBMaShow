#pragma once
#include "TextView.h"
#include "View.h"
#include "ImageView.h"
#include <SDL2/SDL.h>
#include <string>
#include "../ChartDBHelper.h"

class ChartListItemView : public View {
public:
  ChartListItemView(int x, int y, int width, int height,
                    const ChartMetaRecord &record);

  void setMeta(const ChartMetaRecord &record);
  void setClearRank(int clearRank);
  void onSelected() override;
  void onUnselected() override;

private:
  void applyTextColors(bool selected);

  bool unavailable = false;
  bool solidArchive = false;
  View *contentCard;
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
