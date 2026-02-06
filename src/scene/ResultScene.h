#pragma once
#include "Scene.h"
#include "play/RhythmState.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"

class TextView;

class ResultScene : public Scene {
public:
  ResultScene(ApplicationContext &context, const bms_parser::ChartMeta &meta,
              const RhythmState &state);
  ~ResultScene() override;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  bms_parser::ChartMeta meta;
  RhythmState resultState;
  View *rootLayout = nullptr;
  View *graphPlaceHolder = nullptr;
  ISkin* skin = nullptr;
};
