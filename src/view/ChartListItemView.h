#pragma once
#include "TextView.h"
#include "View.h"
#include "ImageView.h"
#include <SDL2/SDL.h>
#include <functional>
#include <string>
#include "../ChartDBHelper.h"

class Button;

class ChartListItemView : public View {
public:
  ChartListItemView(int x, int y, int width, int height,
                    const ChartMetaRecord &record);

  void setMeta(const ChartMetaRecord &record);
  void setClearRank(int clearRank);
  void setFavoriteToggleHandler(
      std::function<bool(const ChartMetaRecord &, bool)> handler);
  void onSelected() override;
  void onUnselected() override;

private:
  void applyTextColors(bool selected);
  void setFavoriteState(bool favorite);
  void refreshFavoriteButton();

  ChartMetaRecord currentRecord;
  bool unavailable = false;
  bool solidArchive = false;
  bool favorite = false;
  bool selected = false;
  std::function<bool(const ChartMetaRecord &, bool)> favoriteToggleHandler;
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
  Button *favoriteButton;
  TextView *favoriteIconView;
};
