#include "ResultScene.h"
#include "../PlayOptionUtils.h"
#include "../ReplayDBHelper.h"
#include "../ResultImageExporter.h"
#include "../ScoreDBHelper.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include "play/GamePlayScene.h"

#include "../rendering/Color.h"
#include "../rendering/SimpleBatchRenderer.h"
#include "../rendering/common.h"
#include "bgfx/bgfx.h"
#include "../skin/DefaultSkin.h"
#include "../skin/SkinTypes.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
Color resultGaugeLineColor(float value) {
  if (value > 80.0f) {
    return ui_theme::withAlpha(ui_theme::cyan(), 210);
  }
  if (value > 30.0f) {
    return ui_theme::withAlpha(ui_theme::lime(), 210);
  }
  return ui_theme::withAlpha(ui_theme::coral(), 210);
}

void drawResultGaugeLineGraph(rendering::SimpleBatchRenderer &batch,
                              const RhythmState &resultState, float x, float y,
                              float w, float h) {
  batch.addRect(x, y, w, h, ui_theme::resultPanelSubtle().toABGR());

  const float padding = 8.0f;
  const float graphX = x + padding;
  const float graphY = y + padding;
  const float graphW = std::max(1.0f, w - padding * 2.0f);
  const float graphH = std::max(1.0f, h - padding * 2.0f);
  auto valueY = [&](float value) {
    const float clamped = std::clamp(value, 0.0f, 100.0f);
    return graphY + graphH - (clamped / 100.0f) * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  batch.addLine(graphX, valueY(80.0f), graphX + graphW, valueY(80.0f), 1.0f,
                guideColor);
  batch.addLine(graphX, valueY(30.0f), graphX + graphW, valueY(30.0f), 1.0f,
                guideColor);

  const size_t count = resultState.gaugeHistory.size();
  if (count == 1) {
    const float value = std::clamp(resultState.gaugeHistory.front(), 0.0f,
                                   100.0f);
    batch.addCircle(graphX, valueY(value), 3.5f,
                    resultGaugeLineColor(value).toABGR());
    return;
  }

  for (size_t i = 1; i < count; ++i) {
    const float prevValue =
        std::clamp(resultState.gaugeHistory[i - 1], 0.0f, 100.0f);
    const float value = std::clamp(resultState.gaugeHistory[i], 0.0f, 100.0f);
    const float x0 =
        graphX + (static_cast<float>(i - 1) / static_cast<float>(count - 1)) *
                     graphW;
    const float x1 =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addLine(x0, valueY(prevValue), x1, valueY(value), 3.0f,
                  resultGaugeLineColor(value).toABGR());
  }

  const size_t markerStep = std::max<size_t>(1, count / 40);
  for (size_t i = 0; i < count; i += markerStep) {
    const float value = std::clamp(resultState.gaugeHistory[i], 0.0f, 100.0f);
    const float pointX =
        graphX + (static_cast<float>(i) / static_cast<float>(count - 1)) *
                     graphW;
    batch.addCircle(pointX, valueY(value), 2.5f,
                    resultGaugeLineColor(value).toABGR());
  }
}

std::string resultPlayModeLabel(
    const bms_parser::ChartMeta &meta,
    const std::optional<ReplayData> &replayToSave,
    const std::optional<ReplayData> &retryData,
    const ResultPracticeOptions &practiceOptions) {
  if (practiceOptions.enabled) {
    return play_options::formatPlayModeLabel(
        meta, practiceOptions.playOption, practiceOptions.playOptionSeed,
        practiceOptions.playOption2, practiceOptions.playOption2Seed);
  }
  if (replayToSave.has_value()) {
    return play_options::formatPlayModeLabel(*replayToSave);
  }
  if (retryData.has_value()) {
    return play_options::formatPlayModeLabel(*retryData);
  }
  return play_options::formatPlayModeLabel(meta, std::nullopt);
}

ResultPreviousBestData toResultPreviousBestData(
    const ScoreBestSnapshot &snapshot) {
  return {.score = snapshot.score,
          .maxScore = snapshot.maxScore,
          .maxCombo = snapshot.maxCombo,
          .comboBreak = snapshot.comboBreak,
          .finalGauge = snapshot.finalGauge,
          .clearType = snapshot.clearType,
          .createdAt = snapshot.createdAt};
}
} // namespace

ResultScene::ResultScene(ApplicationContext &context,
                         const bms_parser::ChartMeta &meta,
                         const RhythmState &state, const ReplayData *replay,
                         bool shouldSaveScore, const ReplayData *retrySource,
                         ResultPracticeOptions practiceOptions)
    : Scene(context), meta(meta), resultState(state),
      replayToSave(replay != nullptr ? std::optional<ReplayData>(*replay)
                                     : std::nullopt),
      retryData(retrySource != nullptr
                    ? std::optional<ReplayData>(*retrySource)
                    : (replay != nullptr ? std::optional<ReplayData>(*replay)
                                         : std::nullopt)),
      practiceOptions(std::move(practiceOptions)),
      shouldSaveScore(shouldSaveScore),
      replayResult(!shouldSaveScore && retrySource != nullptr &&
                   !this->practiceOptions.enabled) {
  playModeLabel =
      resultPlayModeLabel(this->meta, replayToSave, retryData,
                          this->practiceOptions);
  skin = std::make_unique<DefaultSkin>();
}

void ResultScene::saveScore() {
  if (scoreSaved || !shouldSaveScore) {
    return;
  }
  scoreSaved = true;

  if (!ScoreDBHelper::GetInstance().SaveScore(meta, resultState)) {
    SDL_Log("Failed to save score for chart: %s", meta.Title.c_str());
  }
}

void ResultScene::loadPreviousBest() {
  if (previousBestLoaded) {
    return;
  }
  previousBestLoaded = true;

  std::optional<std::string> beforeCreatedAt;
  if (!shouldSaveScore && retryData.has_value() && !retryData->createdAt.empty()) {
    beforeCreatedAt = retryData->createdAt;
  }

  const auto best =
      ScoreDBHelper::GetInstance().LoadBestScore(meta, beforeCreatedAt);
  if (best.has_value()) {
    previousBest = toResultPreviousBestData(*best);
  }
}

void ResultScene::saveReplay() {
  if (replaySaved || !replayToSave.has_value() ||
      replayToSave->events.empty()) {
    return;
  }
  replaySaved = true;

  if (!ReplayDBHelper::GetInstance().SaveReplay(*replayToSave).has_value()) {
    SDL_Log("Failed to save replay for chart: %s", meta.Title.c_str());
  }
}

void ResultScene::addRetryButtons() {
  if (rootLayout == nullptr) {
    return;
  }

  View *actionHost = rootLayout->findViewByName("resultActions");
  if (actionHost == nullptr) {
    actionHost = rootLayout;
  }

  auto retryRow = new View();
  retryRow->setFlexDirection(FlexDirection::Row);
  retryRow->setAlignItems(YGAlignCenter);
  retryRow->setJustifyContent(YGJustifyCenter);
  retryRow->setFlexWrap(YGWrapWrap);
  retryRow->setGap(14);

  auto makeButton = [this](const std::string &label, bool samePattern,
                           bool replay, Color normal, Color hover,
                           Color pressed, Color border) {
    auto button = new Button();
    auto text = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    text->setVAlign(TextView::MIDDLE);
    text->setColor(ui_theme::sdl(ui_theme::textOn(normal)));
    button->setContentView(text);
    button->setOnClickListener([this, samePattern, replay]() {
      if (replay) {
        startReplay();
      } else {
        startRetry(samePattern);
      }
    });
    button->setSize(232, 64);
    button->setCornerRadius(ui_theme::controlRadius());
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(ui_theme::withAlpha(border, 150),
                            ui_theme::withAlpha(border, 190),
                            ui_theme::withAlpha(border, 220));
    button->setStyledBorderWidth(1);
    return button;
  };

  if (replayResult) {
    retryRow->addView(makeButton("Replay", true, true, ui_theme::infoAction(),
                                 ui_theme::infoActionHover(),
                                 ui_theme::infoActionPressed(),
                                 ui_theme::cyan()));
  } else if (practiceOptions.enabled) {
    retryRow->addView(makeButton("Retry", true, false,
                                 ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                                 ui_theme::primaryActionPressed(),
                                 ui_theme::cyan()));
  } else {
    retryRow->addView(makeButton("Retry", false, false,
                                 ui_theme::primaryAction(),
                                 ui_theme::primaryActionHover(),
                                 ui_theme::primaryActionPressed(),
                                 ui_theme::cyan()));
    const bool canRetrySame =
        retryData.has_value()
            ? play_options::hasSamePatternRandomization(*retryData)
            : play_options::hasSamePatternRandomization(meta);
    if (canRetrySame) {
      retryRow->addView(makeButton("Retry Same", true, false,
                                   ui_theme::successAction(),
                                   ui_theme::successActionHover(),
                                   ui_theme::successActionPressed(),
                                   ui_theme::lime()));
    }
  }

  exportPhotoButton = new Button();
  exportPhotoButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  exportPhotoButtonText->setText("Export Photo");
  exportPhotoButtonText->setAlign(TextView::CENTER);
  exportPhotoButtonText->setVAlign(TextView::MIDDLE);
  exportPhotoButtonText->setColor(
      ui_theme::sdl(ui_theme::textOn(ui_theme::violetAction())));
  exportPhotoButton->setContentView(exportPhotoButtonText);
  exportPhotoButton->setOnClickListener([this]() { exportPhoto(); });
  exportPhotoButton->setSize(232, 64);
  exportPhotoButton->setCornerRadius(ui_theme::controlRadius());
  exportPhotoButton->setBackgroundColors(ui_theme::violetAction(),
                                         ui_theme::violetActionHover(),
                                         ui_theme::violetActionPressed());
  exportPhotoButton->setBorderColors(
      ui_theme::withAlpha(ui_theme::violetActionHover(), 150),
      ui_theme::withAlpha(ui_theme::violetActionHover(), 190),
      ui_theme::withAlpha(ui_theme::violetActionHover(), 220));
  exportPhotoButton->setStyledBorderWidth(1);
  retryRow->addView(exportPhotoButton);
  actionHost->addView(retryRow);
}

void ResultScene::exportPhoto() {
  if (resultPhotoExportInProgress || exportPhotoButtonText == nullptr) {
    return;
  }

  resultPhotoExportInProgress = true;
  exportPhotoButtonText->setText("Saving...");
  const auto result = ResultImageExporter::Export(
      context, meta, resultState, playModeLabel, previousBest);
  resultPhotoExportInProgress = false;

  if (result.success) {
    exportPhotoButtonText->setText(result.message == "Saved to Photos"
                                       ? "Saved"
                                       : "Exported");
    SDL_Log("Result image exported: %s (%s)",
            result.outputPath.string().c_str(), result.message.c_str());
  } else {
    exportPhotoButtonText->setText("Export Failed");
    SDL_Log("Result image export failed: %s (%s)", result.message.c_str(),
            result.outputPath.string().c_str());
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }

  defer(
      [this]() {
        if (!resultPhotoExportInProgress && exportPhotoButtonText != nullptr) {
          exportPhotoButtonText->setText("Export Photo");
          if (rootLayout != nullptr) {
            rootLayout->applyYogaLayout();
          }
        }
        return true;
      },
      result.success ? 1800 : 1400, true);
}

void ResultScene::startRetry(bool samePattern) {
  ReplayData retrySource;
  if (retryData.has_value()) {
    retrySource = *retryData;
  } else {
    retrySource.chartMeta = meta;
    retrySource.randomSeed = meta.RandomSeed;
    retrySource.randomPrng = meta.RandomPrng;
    retrySource.randomValues = meta.RandomValues;
  }
  if (practiceOptions.enabled) {
    retrySource.playOption = practiceOptions.playOption;
    retrySource.playOptionSeed = practiceOptions.playOptionSeed;
    retrySource.playOption2 = practiceOptions.playOption2;
    retrySource.playOption2Seed = practiceOptions.playOption2Seed;
    retrySource.initialGaugeType = practiceOptions.gaugeType;
    retrySource.gaugeAutoShift = practiceOptions.gaugeAutoShift;
  }

  context.jukebox.stop();
  defer(
      [this, retrySource, samePattern]() {
        std::atomic_bool parseCancelled = false;
        auto retryChart = play_options::parseChartForRetry(
            retrySource, meta, parseCancelled, samePattern);
        if (retryChart == nullptr || parseCancelled) {
          return true;
        }

        StartOptions options;
        options.startPosition =
            practiceOptions.enabled ? practiceOptions.startPosition : 0;
        options.autoKeySound =
            practiceOptions.enabled ? practiceOptions.autoKeySound
                                    : !context.settings.inputKeysoundEnabled;
        options.autoPlay = false;
        options.gaugeType = retrySource.initialGaugeType;
        options.gaugeAutoShift = retrySource.gaugeAutoShift;
        options.ownsChart = true;
        if (practiceOptions.enabled) {
          options.practiceMode = true;
          options.practiceLeadInMicros = practiceOptions.leadInMicros;
          options.returnScene = practiceOptions.returnScene;
          options.practiceGhostCallback =
              practiceOptions.practiceGhostCallback;
        }

        if (retrySource.playOption.has_value()) {
          if (samePattern &&
              play_options::usesRandomizer(*retrySource.playOption) &&
              !retrySource.playOptionSeed.has_value()) {
            SDL_Log("Cannot retry same pattern: missing play option seed");
            return true;
          }
          const std::optional<long long> optionSeed =
              samePattern ? retrySource.playOptionSeed
                          : std::optional<long long>();
          if (!play_options::applyPlayOptionModifier(
                  *retryChart, *retrySource.playOption, optionSeed, 0,
                  options.playOption, options.playOptionSeed, "retry")) {
            return true;
          }
        }

        if (retryChart->Meta.IsDP && retrySource.playOption2.has_value()) {
          if (samePattern &&
              play_options::usesRandomizer(*retrySource.playOption2) &&
              !retrySource.playOption2Seed.has_value()) {
            SDL_Log("Cannot retry same pattern: missing P2 play option seed");
            return true;
          }
          const std::optional<long long> optionSeed =
              samePattern ? retrySource.playOption2Seed
                          : std::optional<long long>();
          if (!play_options::applyPlayOptionModifier(
                  *retryChart, *retrySource.playOption2, optionSeed, 1,
                  options.playOption2, options.playOption2Seed, "retry")) {
            return true;
          }
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*retryChart, true, parseCancelled);
        if (parseCancelled) {
          return true;
        }

        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(context, std::move(retryChart),
                                            options),
            false);
        return false;
      },
      0, true);
}

void ResultScene::exitResult() {
  context.jukebox.stop();
  if (practiceOptions.enabled && practiceOptions.returnScene != nullptr) {
    context.sceneManager->changeScene(practiceOptions.returnScene, false);
    return;
  }
  context.sceneManager->changeScene("MainMenu");
}

void ResultScene::startReplay() {
  if (!retryData.has_value()) {
    return;
  }

  const ReplayData replaySource = *retryData;
  context.jukebox.stop();
  defer(
      [this, replaySource]() {
        std::atomic_bool parseCancelled = false;
        auto replayChart = play_options::prepareReplayChart(
            meta.BmsPath, replaySource, parseCancelled);
        if (replayChart == nullptr || parseCancelled) {
          return true;
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*replayChart, true, parseCancelled);
        if (parseCancelled) {
          return true;
        }

        auto replayData = std::make_shared<ReplayData>(replaySource);
        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(
                context, std::move(replayChart),
                StartOptions{
                    .startPosition = 0,
                    .autoKeySound = false,
                    .autoPlay = false,
                    .gaugeType = replayData->initialGaugeType,
                    .gaugeAutoShift = replayData->gaugeAutoShift,
                    .replayData = replayData,
                    .ownsChart = true,
                }),
            false);
        return false;
      },
      0, true);
}

void ResultScene::init() {
  loadPreviousBest();
  saveScore();
  saveReplay();

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);

  ResultSkinData data = {&resultState, &meta, &context};
  data.playModeLabel = playModeLabel;
  data.previousBest = previousBest;
  skin->buildLayout("Result", rootLayout, &data);
  addRetryButtons();

  if (auto *backButton =
          dynamic_cast<Button *>(rootLayout->findViewByName("backButton"));
      backButton != nullptr) {
    backButton->setOnClickListener([this]() { exitResult(); });
  }

  graphPlaceHolder = rootLayout->findViewByName("graph");

  rootLayout->applyYogaLayout();
}

void ResultScene::update(float dt) {}

void ResultScene::renderScene() {
  if (graphPlaceHolder && !resultState.gaugeHistory.empty()) {
    float x = graphPlaceHolder->getX();
    float y = graphPlaceHolder->getY();
    float w = graphPlaceHolder->getWidth();
    float h = graphPlaceHolder->getHeight();

    rendering::SimpleBatchRenderer graphBatch;
    graphBatch.setSubmitView(rendering::ui_view);
    graphBatch.setSubmitDepth(0);
    graphBatch.begin();
    drawResultGaugeLineGraph(graphBatch, resultState, x, y, w, h);
    graphBatch.end();
  }
}

void ResultScene::cleanupScene() {
  rootLayout = nullptr;
  graphPlaceHolder = nullptr;
  exportPhotoButton = nullptr;
  exportPhotoButtonText = nullptr;
  resultPhotoExportInProgress = false;
}
