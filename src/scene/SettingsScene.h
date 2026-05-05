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
  View *rootLayout = nullptr;
  TextInputBox *offsetInput = nullptr;
  TextView *summaryOffsetValueText = nullptr;
  TextInputBox *visualOffsetInput = nullptr;
  TextView *summaryVisualOffsetValueText = nullptr;
  TextInputBox *visibleTimeInput = nullptr;
  TextView *summaryVisibleTimeValueText = nullptr;
  TextView *summaryKeysoundValueText = nullptr;
  TextView *summaryBgaValueText = nullptr;
  TextView *visibleTimeModeText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *bgaModeText = nullptr;
  Button *visibleTimeModeButton = nullptr;
  Button *keysoundModeButton = nullptr;
  Button *bgaModeButton = nullptr;
  ScrollView *scrollView = nullptr;
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
  void commitOffsetInput();
  void commitVisualOffsetInput();
  void commitVisibleTimeInput();
};
