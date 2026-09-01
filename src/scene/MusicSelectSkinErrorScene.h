#pragma once

#include "Scene.h"
#include "../skin/package/SkinPackageTypes.h"

#include <vector>

class View;

class MusicSelectSkinErrorScene final : public Scene {
public:
  MusicSelectSkinErrorScene(ApplicationContext &, std::vector<skin::SkinDiagnostic>);

  void init() override;
  void update(float) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void backToIntro();
  void openSettings();

  std::vector<skin::SkinDiagnostic> diagnostics_;
  View *rootLayout_ = nullptr;
  int layoutWidth_ = -1;
  int layoutHeight_ = -1;
};
