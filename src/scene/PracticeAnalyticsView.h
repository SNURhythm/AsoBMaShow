#pragma once

#include "../practice/PracticeResultModel.h"
#include "../view/View.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

class TextView;

enum class PracticeAnalyticsMode { Histogram, Lanes, Sections };

class PracticeAnalyticsView : public View {
public:
  explicit PracticeAnalyticsView(practice::ResultModel model);

  void setAttemptSelection(std::optional<std::size_t> attemptIndex);
  void setAggregateSelection(std::size_t groupIndex);
  void setMode(PracticeAnalyticsMode mode);
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
  View *chartView = nullptr;

  void build();
  void moveSelection(int delta);
  void applyChoice();
  void refreshText();
  void selectSections(std::size_t firstSection, std::size_t lastSection);
};
