#pragma once
#include "../ReplayData.h"
#include "Scene.h"
#include "play/RhythmState.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"
#include <optional>

class TextView;

class ResultScene : public Scene {
public:
  ResultScene(ApplicationContext &context, const bms_parser::ChartMeta &meta,
              const RhythmState &state, const ReplayData *replay = nullptr,
              bool shouldSaveScore = true);
  ~ResultScene() override;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void saveScore();
  void saveReplay();

  bms_parser::ChartMeta meta;
  RhythmState resultState;
  std::optional<ReplayData> replayToSave;
  View *rootLayout = nullptr;
  View *graphPlaceHolder = nullptr;
  ISkin* skin = nullptr;
  bool shouldSaveScore = true;
  bool scoreSaved = false;
  bool replaySaved = false;
};
