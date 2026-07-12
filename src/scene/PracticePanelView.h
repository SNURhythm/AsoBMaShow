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
  void setPresetMessage(std::string message, bool error = false);

  [[nodiscard]] const practice::Configuration &configuration() const {
    return currentConfiguration;
  }
  [[nodiscard]] const std::optional<std::string> &selectedPresetId() const {
    return selectedNamedPresetId;
  }
  [[nodiscard]] bool isEditingPresetName() const;

private:
  enum class DropDownIndex : int {
    Preset,
    Loop,
    CountIn,
    Gauge,
    StartingGauge,
    Judge,
    Rate,
    Mode,
    Count
  };

  long long chartEndMicros = 0;
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
