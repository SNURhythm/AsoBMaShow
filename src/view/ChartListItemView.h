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

  void setMeta(const ChartMetaRecord &record,
               bool prioritizeArtwork = false);
  void setClearRank(int clearRank);
  void setBestScoreRank(int score, int maxScore);
  void setGoalHandler(std::function<void(const ChartMetaRecord &)> handler);
  void setGoalState(bool goal);
  void onSelected() override;
  void onUnselected() override;

protected:
  void onLayout() override;

private:
  void applyTextColors(bool selected);
  void refreshGoalButton();
  void refreshScoreRankColor();
  void refreshColumnWidths();

  ChartMetaRecord currentRecord;
  bool unavailable = false;
  bool solidArchive = false;
  bool goal = false;
  bool selected = false;
  std::function<void(const ChartMetaRecord &)> goalHandler;
  View *contentCard;
  bool compact_ = false;
  int desiredDifficultyWidth_ = 86;
  int desiredLevelWidth_ = 24;
  int appliedLevelWidth_ = -1;
  int appliedDifficultyWidth_ = -1;
  int appliedTitleMinimumWidth_ = -1;
  TextView *artworkPlaceholder;
  View *clearLamp;
  View *clearLampHalo;
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
  Button *goalButton;
  TextView *goalIconView;
  TextView *goalOutlineView;
};
