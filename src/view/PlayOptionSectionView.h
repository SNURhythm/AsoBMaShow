#pragma once

#include "View.h"
#include "../i18n/Localization.h"

#include <functional>
#include <string>

class Button;
class ChoiceControlView;
class OverlayPortal;
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
  bool showHeading = true;
};

class PlayOptionSectionView : public View {
public:
  explicit PlayOptionSectionView(
      PlayOptionSectionCallbacks callbacks,
      PlayOptionSectionLayout layout = PlayOptionSectionLayout{},
      OverlayPortal *overlayPortal = nullptr);

  void refresh(const std::string &selectedOption,
               const std::string &defaultLaneOrder, bool laneOrderEnabled);
  void setLaneOrderMessage(i18n::Text message, bool error = false);
  void closeDropdowns();
  void resetLaneOrderDraft() { laneOrderSnapshotInitialized = false; }

private:
  PlayOptionSectionCallbacks callbacks;
  ChoiceControlView *optionChoice = nullptr;
  TextInputBox *laneOrderInput = nullptr;
  bool laneOrderSnapshotInitialized = false;
  std::string lastLaneOrderOption;
  std::string lastDefaultLaneOrder;
  bool lastLaneOrderEnabled = false;
  Button *applyLaneOrderButton = nullptr;
  Button *resetLaneOrderButton = nullptr;
  TextView *laneOrderMessage = nullptr;
};
