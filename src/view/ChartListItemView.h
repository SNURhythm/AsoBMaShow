#pragma once
#include "TextView.h"
#include "View.h"
#include "ImageView.h"
#include <SDL2/SDL.h>
#include <functional>
#include <string>
#include "../repositories/ChartRepository.h"

class Button;

class ChartListItemView : public View {
public:
  ChartListItemView(int x, int y, int width, int height,
                    const ChartMetaRecord &record);

  void setMeta(const ChartMetaRecord &record,
               bool prioritizeArtwork = false);
  void setClearRank(int clearRank);
  void setBestScoreRank(int score, int maxScore);
  void setFavoriteToggleHandler(
      std::function<bool(const ChartMetaRecord &, bool)> handler);
  void onSelected() override;
  void onUnselected() override;

private:
  void applyTextColors(bool selected);
  void setFavoriteState(bool favorite);
  void refreshFavoriteButton();
  void refreshScoreRankColor();

  ChartMetaRecord currentRecord;
  bool unavailable = false;
  bool solidArchive = false;
  bool favorite = false;
  bool selected = false;
  std::function<bool(const ChartMetaRecord &, bool)> favoriteToggleHandler;
  View *contentCard;
  ImageView *bannerImage;
  View *clearLamp;
  View *artworkFrame;
  ImageView *jacketImage;
  View *textLayout;
  View *detailsLayout;
  View *scoreRankColumn;
  TextView *titleView;
  TextView *artistView;
  TextView *levelView;
  TextView *keyModeView;
  TextView *scoreRankShadowView;
  TextView *scoreRankWeightView;
  TextView *scoreRankView;
  std::string scoreRank;
  Button *favoriteButton;
  TextView *favoriteIconView;
};
