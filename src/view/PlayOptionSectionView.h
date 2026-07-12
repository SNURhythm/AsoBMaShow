#pragma once

#include "View.h"

#include <functional>
#include <string>
#include <vector>

class Button;
class TextInputBox;
class TextView;

struct PlayOptionSectionCallbacks {
  std::function<void(const std::string &)> onOptionSelected;
  std::function<void(const std::string &)> onLaneOrderSubmitted;
  std::function<bool(const std::string &)> isOptionAllowed;
};

struct PlayOptionSectionLayout {
  int columns = 4;
  float rowHeight = 58.0f;
  int buttonFontSize = 15;
  bool showLaneOrder = true;
};

class PlayOptionSectionView : public View {
public:
  explicit PlayOptionSectionView(
      PlayOptionSectionCallbacks callbacks,
      PlayOptionSectionLayout layout = PlayOptionSectionLayout{});

  void refresh(const std::string &selectedOption,
               const std::string &defaultLaneOrder, bool laneOrderEnabled);
  void setLaneOrderMessage(std::string message, bool error = false);

private:
  struct OptionButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::string option;
  };

  PlayOptionSectionCallbacks callbacks;
  std::vector<OptionButton> optionButtons;
  TextInputBox *laneOrderInput = nullptr;
  Button *applyLaneOrderButton = nullptr;
  Button *resetLaneOrderButton = nullptr;
  TextView *laneOrderMessage = nullptr;
};
