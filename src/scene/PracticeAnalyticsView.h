#pragma once

#include "../practice/PracticeResultModel.h"
#include "../view/View.h"
#include "PracticeAnalyticsPresentation.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

class TextView;
class Button;

class PracticeAnalyticsView : public View {
public:
  explicit PracticeAnalyticsView(practice::ResultModel model);

  void setAttemptSelection(std::optional<std::size_t> attemptIndex);
  void setAggregateSelection(std::size_t groupIndex);
  void setMode(PracticeAnalyticsMode mode);
  void setPhotoExportPresentation(bool showSharedInformation);
  void setSectionSelectionListener(
      std::function<void(long long, long long)> listener);
  [[nodiscard]] std::optional<practice::RangeSelection> selectedSection() const;

private:
  struct SelectionChoice {
    std::optional<std::size_t> attemptIndex;
    std::size_t groupIndex = 0;
  };

  practice::ResultModel model;
  PracticeAnalyticsMode mode = PracticeAnalyticsMode::Histogram;
  std::vector<SelectionChoice> choices;
  std::size_t choiceIndex = 0;
  std::function<void(long long, long long)> sectionSelectionListener;
  TextView *selectionText = nullptr;
  TextView *summaryText = nullptr;
  TextView *detailText = nullptr;
  View *modeControlsRow = nullptr;
  View *choiceRow = nullptr;
  Button *previousChoiceButton = nullptr;
  Button *nextChoiceButton = nullptr;
  View *chartView = nullptr;
  std::vector<View *> modeButtons;

  void build();
  void moveSelection(int delta);
  void applyChoice();
  void refreshText();
  void selectSections(std::size_t firstSection, std::size_t lastSection);
};
