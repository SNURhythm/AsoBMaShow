#pragma once
#include "TextView.h"
#include "View.h"
#include "ImageView.h"
#include <SDL3/SDL.h>
#include <functional>
#include <string>
#include "../repositories/ChartRepository.h"

class Button;

class ChartListItemView : public View {
public:
  ChartListItemView(int x, int y, int width, int height,
                    const ChartMetaRecord &record);

  static View *createColumnHeader();
  void setDisplayIndex(int index);
  void setMeta(const ChartMetaRecord &record,
               bool prioritizeArtwork = false);
  void setClearRank(int clearRank);
  void setBestScoreRank(int score, int maxScore);
  void setFavoriteToggleHandler(
      std::function<bool(const ChartMetaRecord &, bool)> handler);
  void onSelected() override;
  void onUnselected() override;

protected:
  void onLayout() override;

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
  bool compact_ = false;
  View *selectedRail;
  View *clearColumn;
  TextView *rowNumber;
  TextView *clearLabel;
  TextView *artworkPlaceholder;
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
  TextView *scoreRankView;
  TextView *bestScoreView;
  std::string scoreRank;
  Button *favoriteButton;
  TextView *favoriteIconView;
  TextView *favoriteOutlineView;
};
