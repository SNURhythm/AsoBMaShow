#pragma once

#include "Scene.h"

class View;
class TextView;
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
  TextView *offsetValueText = nullptr;
  TextView *summaryOffsetValueText = nullptr;
  TextView *summaryKeysoundValueText = nullptr;
  TextView *summaryBgaValueText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *bgaModeText = nullptr;
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
};
