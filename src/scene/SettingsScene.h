#pragma once

#include "Scene.h"

class View;
class TextView;
class Button;

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

  void initView();
  void refreshSettingsText();
  void persistSettings();
};
