#pragma once

#include "View.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ScoreRepositoryModels.h"

class ImageView;
class TextView;

// Presentation only: binding uses the selector's existing metadata and score
// caches and never reads a database or parses a chart.
class ChartDetailsView : public View {
public:
  explicit ChartDetailsView(ImageView *artwork);
  static constexpr float minimumChartHeight() {
    return kIdentityHeight + kDifficultyHeight + 2 * kSectionGap +
           3 * kFactRowHeight + 2 * kFactGap + 2 * kFactPadding;
  }
  void setScoreContainer(View *container);
  void setChart(const ChartMetaRecord *record,
                const std::optional<ScoreBestSnapshot> &best,
                int clearRank, const std::string &total);

private:
  static constexpr float kIdentityHeight = 132;
  static constexpr float kDifficultyHeight = 34;
  static constexpr float kSectionGap = 10;
  static constexpr float kFactRowHeight = 60;
  static constexpr float kFactGap = 8;
  static constexpr float kFactPadding = 14;
  View *artworkFrame_ = nullptr;
  View *facts_ = nullptr;
  View *personalBest_ = nullptr;
  View *clearLamp_ = nullptr;
  TextView *title_ = nullptr;
  TextView *artist_ = nullptr;
  TextView *genre_ = nullptr;
  TextView *difficulty_ = nullptr;
  TextView *bpm_ = nullptr;
  TextView *judge_ = nullptr;
  TextView *length_ = nullptr;
  TextView *notes_ = nullptr;
  TextView *total_ = nullptr;
  TextView *totalLabel_ = nullptr;
  TextView *noteTypes_ = nullptr;
  TextView *bestLabel_ = nullptr;
  TextView *clear_ = nullptr;
  TextView *score_ = nullptr;
  TextView *rate_ = nullptr;
  TextView *next_ = nullptr;
  TextView *emptyScore_ = nullptr;
};
