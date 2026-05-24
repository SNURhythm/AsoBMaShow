#pragma once

#include "Scene.h"

class View;
class TextView;
class TextInputBox;
class Button;
class ScrollView;

class SettingsScene : public Scene {
public:
  explicit SettingsScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  enum class SettingsTab {
    Timing,
    Visual,
    Lane,
  };

  View *rootLayout = nullptr;
  TextInputBox *offsetInput = nullptr;
  TextView *summaryOffsetValueText = nullptr;
  TextInputBox *visualOffsetInput = nullptr;
  TextView *summaryVisualOffsetValueText = nullptr;
  TextInputBox *visibleTimeInput = nullptr;
  TextView *summaryVisibleTimeValueText = nullptr;
  TextView *summaryKeysoundValueText = nullptr;
  TextView *summaryBgaValueText = nullptr;
  TextView *summaryBgaBrightnessValueText = nullptr;
  TextView *summaryBgaBlurValueText = nullptr;
  TextView *summaryBgaDisplayValueText = nullptr;
  TextView *summaryLaneAngleValueText = nullptr;
  TextView *summaryLaneLengthValueText = nullptr;
  TextView *visibleTimeModeText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *bgaModeText = nullptr;
  TextView *bgaDisplayModeText = nullptr;
  Button *visibleTimeModeButton = nullptr;
  Button *keysoundModeButton = nullptr;
  Button *bgaModeButton = nullptr;
  Button *bgaDisplayModeButton = nullptr;
  Button *timingTabButton = nullptr;
  Button *visualTabButton = nullptr;
  Button *laneTabButton = nullptr;
  TextInputBox *bgaBrightnessInput = nullptr;
  TextInputBox *bgaBlurInput = nullptr;
  TextInputBox *laneAngleInput = nullptr;
  TextInputBox *laneLengthInput = nullptr;
  ScrollView *scrollView = nullptr;
  SettingsTab activeTab = SettingsTab::Timing;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;

  void initView();
  void resetViewState();
  void ensureLayoutUpToDate();
  void refreshSettingsText();
  void persistSettings();
  void syncOffsetInputText(bool force = false);
  void syncVisualOffsetInputText(bool force = false);
  void syncVisibleTimeInputText(bool force = false);
  void syncBgaBrightnessInputText(bool force = false);
  void syncBgaBlurInputText(bool force = false);
  void syncLaneAngleInputText(bool force = false);
  void syncLaneLengthInputText(bool force = false);
  void commitOffsetInput();
  void commitVisualOffsetInput();
  void commitVisibleTimeInput();
  void commitBgaBrightnessInput();
  void commitBgaBlurInput();
  void commitLaneAngleInput();
  void commitLaneLengthInput();
};
