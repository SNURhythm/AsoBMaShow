#pragma once

#include "../scene/play/RhythmState.h"
#include "View.h"

#include <functional>
#include <string>
#include <vector>

class Button;
class DropdownView;
class OverlayPortal;
class PlayOptionSectionView;
class SnappedSlider;
class TextView;

struct PlayOptionsPanelState {
  GaugeType gaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  std::string playOption = "NORMAL";
  std::string defaultLaneOrder;
  bool laneOrderEnabled = false;
  std::string longNoteMode = "LN";
  bool longNoteModeLocked = false;
  std::string assistOption = "OFF";
  bool assistOptionLocked = false;
  int playbackRatePercent = 100;
  bool playbackLocked = false;
  bool clubMode = false;
  std::string pacemakerTarget = "BEST";
};

struct PlayOptionsPanelCallbacks {
  std::function<void(GaugeType, GaugeAutoShiftMode)> onGaugeSelected;
  std::function<void(const std::string &)> onPlayOptionSelected;
  std::function<void(const std::string &)> onLaneOrderSubmitted;
  std::function<bool(const std::string &)> isPlayOptionAllowed;
  std::function<void(const std::string &)> onLongNoteModeSelected;
  std::function<void(const std::string &)> onAssistOptionSelected;
  std::function<void(int)> onPlaybackRateSelected;
  std::function<void(const std::string &)> onPlaybackModeSelected;
  std::function<void()> onClubModeToggled;
  std::function<void(const std::string &)> onPacemakerSelected;
};

struct PlayOptionsPanelLayout {
  float width = 620.0f;
  int playOptionColumns = 4;
  bool showGauge = true;
  bool showLaneOrder = false;
  bool showPacemaker = true;
};

class PlayOptionsPanelView : public View {
public:
  PlayOptionsPanelView(PlayOptionsPanelCallbacks callbacks,
                       PlayOptionsPanelLayout layout,
                       OverlayPortal *overlayPortal);

  void refresh(const PlayOptionsPanelState &state);
  void setLaneOrderMessage(std::string message, bool error = false);
  void closeDropdowns();

private:
  struct SelectionButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::string id;
    GaugeType gaugeType = GaugeType::Normal;
    GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  };

  PlayOptionsPanelCallbacks callbacks;
  PlayOptionsPanelState state;
  PlayOptionSectionView *playOptionSection = nullptr;
  std::vector<SelectionButton> gaugeButtons;
  std::vector<SelectionButton> longNoteModeButtons;
  std::vector<SelectionButton> assistOptionButtons;
  std::vector<SelectionButton> pacemakerButtons;
  TextView *assistOptionLabel = nullptr;
  SnappedSlider *playbackRateSlider = nullptr;
  TextView *playbackRateText = nullptr;
  DropdownView *playbackModeDropdown = nullptr;
  bool playbackModeDropdownOpen = false;
  Button *clubModeButton = nullptr;
  TextView *clubModeButtonText = nullptr;
};
