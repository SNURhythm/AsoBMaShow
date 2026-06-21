#pragma once
#include "../ReplayData.h"
#include "Scene.h"
#include "play/RhythmState.h"
#include "../bms_parser.hpp"
#include "../skin/ISkin.h"
#include "../skin/SkinTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>

struct ResultPracticeOptions {
  bool enabled = false;
  unsigned long long startPosition = 0;
  bool autoKeySound = false;
  GaugeType gaugeType = GaugeType::Normal;
  bool gaugeAutoShift = false;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  unsigned long long leadInMicros = 0;
  Scene *returnScene = nullptr;
  std::function<void(const ReplayData &)> practiceGhostCallback;
};

class TextView;
class Button;

class ResultScene : public Scene {
public:
  ResultScene(ApplicationContext &context, const bms_parser::ChartMeta &meta,
              const RhythmState &state, const ReplayData *replay = nullptr,
              bool shouldSaveScore = true,
              const ReplayData *retrySource = nullptr,
              ResultPracticeOptions practiceOptions = {});
  ~ResultScene() override = default;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  void loadDifficultyLabel();
  void loadPreviousBest();
  void saveScore();
  void saveReplay();
  void addRetryButtons();
  void startRetry(bool samePattern);
  void startReplay();
  void exportPhoto();
  void exitResult();

  bms_parser::ChartMeta meta;
  RhythmState resultState;
  std::optional<ReplayData> replayToSave;
  std::optional<ReplayData> retryData;
  std::optional<ResultPreviousBestData> previousBest;
  ResultPracticeOptions practiceOptions;
  std::string playModeLabel;
  std::string difficultyLabel;
  View *rootLayout = nullptr;
  View *graphPlaceHolder = nullptr;
  Button *exportPhotoButton = nullptr;
  TextView *exportPhotoButtonText = nullptr;
  std::unique_ptr<ISkin> skin;
  bool shouldSaveScore = true;
  bool replayResult = false;
  bool scoreSaved = false;
  bool replaySaved = false;
  bool previousBestLoaded = false;
  bool resultPhotoExportInProgress = false;
};
