#pragma once

#include "../practice/PracticeConfiguration.h"
#include "../practice/PracticePresetStore.h"
#include "../view/View.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Button;
class DropdownView;
class OverlayPortal;
class SnappedSlider;
class TextInputBox;
class TextView;

struct PracticePanelCallbacks {
  std::function<void(const practice::Configuration &)> onChanged;
  std::function<void()> onStart;
  std::function<void(std::string)> onSaveAs;
  std::function<void(std::string)> onRename;
  std::function<void()> onUpdateNamed;
  std::function<void()> onDeleteNamed;
};

class PracticePanelView : public View {
public:
  PracticePanelView(long long chartEndMicros, PracticePanelCallbacks callbacks,
                    OverlayPortal *portal,
                    std::function<void(practice::Marker)> onMarkerSelected);

  void refresh(const practice::Configuration &configuration,
               const std::vector<practice::NamedPreset> &namedPresets,
               std::optional<std::string> selectedPresetId,
               practice::Marker activeMarker);
  void setChartEndMicros(long long value) {
    chartEndMicros = std::max(0LL, value);
  }
  void setStartingGaugeMaximum(int value) {
    startingGaugeMaximum = std::clamp(value, 100, 120);
  }
  void setPresetMessage(std::string message, bool error = false);

  [[nodiscard]] const practice::Configuration &configuration() const {
    return currentConfiguration;
  }
  [[nodiscard]] const std::optional<std::string> &selectedPresetId() const {
    return selectedNamedPresetId;
  }
  [[nodiscard]] bool isEditingPresetName() const;

private:
  enum class DropDownIndex : int { Preset, Gauge, Mode, Count };

  long long chartEndMicros = 0;
  int startingGaugeMaximum = 100;
  PracticePanelCallbacks callbacks;
  std::function<void(practice::Marker)> onMarkerSelected;
  practice::Configuration currentConfiguration;
  std::vector<practice::NamedPreset> namedPresets;
  std::optional<std::string> selectedNamedPresetId;
  practice::Marker activeMarker = practice::Marker::Start;
  std::vector<bool> dropdownOpen;
  std::vector<DropdownView *> dropdowns;
  TextView *rangeText = nullptr;
  TextView *diagnosticText = nullptr;
  TextView *presetMessageText = nullptr;
  TextView *presetMessageSecondLine = nullptr;
  TextInputBox *presetNameInput = nullptr;
  Button *loopButton = nullptr;
  TextView *loopButtonText = nullptr;
  SnappedSlider *countInSlider = nullptr;
  SnappedSlider *startingGaugeSlider = nullptr;
  SnappedSlider *judgeSlider = nullptr;
  SnappedSlider *rateSlider = nullptr;
  TextView *countInValueText = nullptr;
  TextView *startingGaugeValueText = nullptr;
  TextView *judgeValueText = nullptr;
  TextView *rateValueText = nullptr;
  Button *startingGaugeDefaultButton = nullptr;
  TextView *startingGaugeDefaultText = nullptr;
  Button *startButton = nullptr;
  Button *updateButton = nullptr;
  Button *renameButton = nullptr;
  Button *deleteButton = nullptr;

  void build(OverlayPortal *portal);
  void refreshControls();
  void setDropdownOpen(DropDownIndex index, bool open);
  void selectDropdownOption(DropDownIndex index, const std::string &id);
  void publishConfiguration();
};
