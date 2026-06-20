#include "ResultScene.h"
#include "../PlayOptionUtils.h"
#include "../ReplayDBHelper.h"
#include "../ScoreDBHelper.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "play/GamePlayScene.h"

#include "../rendering/Color.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/common.h"
#include "bgfx/bgfx.h"
#include "../skin/DefaultSkin.h"
#include "../skin/SkinTypes.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

static void drawRect(float x, float y, float width, float height, Color color) {
  static const bgfx::ProgramHandle kSimpleProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::VertexLayout layout = rendering::PosColorVertex::ms_decl;

  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);

  auto *vertices = (rendering::PosColorVertex *)tvb.data;
  auto *index = (uint16_t *)tib.data;

  uint32_t abgr = color.toABGR();
  vertices[0] = {x, y, 0.0f, abgr};
  vertices[1] = {x + width, y, 0.0f, abgr};
  vertices[2] = {x + width, y + height, 0.0f, abgr};
  vertices[3] = {x, y + height, 0.0f, abgr};

  index[0] = 0;
  index[1] = 1;
  index[2] = 2;
  index[3] = 2;
  index[4] = 3;
  index[5] = 0;

  uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
  bgfx::setState(state);

  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  bgfx::submit(rendering::main_view, kSimpleProgram);
}

// ... (drawRect function remains the same)

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

  auto retryRow = new View();
  retryRow->setFlexDirection(FlexDirection::Row);
  retryRow->setAlignItems(YGAlignCenter);
  retryRow->setJustifyContent(YGJustifyCenter);
  retryRow->setGap(12);

  auto makeButton = [this](const std::string &label, bool samePattern,
                           bool replay) {
    auto button = new Button();
    auto text = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
    text->setText(label);
    text->setAlign(TextView::CENTER);
    button->setContentView(text);
    button->setOnClickListener([this, samePattern, replay]() {
      if (replay) {
        startReplay();
      } else {
        startRetry(samePattern);
      }
    });
    button->setSize(220, 70);
    return button;
  };

  if (replayResult) {
    retryRow->addView(makeButton("Replay", true, true));
  } else if (practiceOptions.enabled) {
    retryRow->addView(makeButton("Retry", true, false));
  } else {
    retryRow->addView(makeButton("Retry", false, false));
    retryRow->addView(makeButton("Retry Same", true, false));
  }
  rootLayout->addView(retryRow);
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
              !play_options::isNormalPlayOption(*retrySource.playOption) &&
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
              !play_options::isNormalPlayOption(*retrySource.playOption2) &&
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
  saveScore();
  saveReplay();

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);

  ResultSkinData data = {&resultState, &meta, &context};
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
  // Draw Gauge Graph
  if (graphPlaceHolder && !resultState.gaugeHistory.empty()) {
    float x = graphPlaceHolder->getX();
    float y = graphPlaceHolder->getY();
    float w = graphPlaceHolder->getWidth();
    float h = graphPlaceHolder->getHeight();

    // Draw background
    drawRect(x, y, w, h, Color(50, 50, 50, 200));

    // Draw graph
    size_t count = resultState.gaugeHistory.size();
    if (count > 1) {
      bgfx::TransientVertexBuffer tvb{};
      bgfx::TransientIndexBuffer tib{};

      if (bgfx::getAvailTransientVertexBuffer(
              count * 4, rendering::PosColorVertex::ms_decl) == count * 4 &&
          bgfx::getAvailTransientIndexBuffer(count * 6) == count * 6) {

        bgfx::allocTransientVertexBuffer(&tvb, count * 4,
                                         rendering::PosColorVertex::ms_decl);
        bgfx::allocTransientIndexBuffer(&tib, count * 6);

        auto *vertices = (rendering::PosColorVertex *)tvb.data;
        auto *index = (uint16_t *)tib.data;

        float step = w / static_cast<float>(count);

        for (size_t i = 0; i < count; ++i) {
          float val = resultState.gaugeHistory[i]; // 0 to 100
          float barH = (val / 100.0f) * h;
          // Color based on value
          Color barColor = val > 80.0f ? Color(0, 255, 255, 200)
                                       : (val > 30.0f ? Color(0, 255, 0, 200)
                                                      : Color(255, 0, 0, 200));
          uint32_t abgr = barColor.toABGR();
          float bx = x + i * step;
          float by = y + h - barH;

          vertices[i * 4 + 0] = {bx, by, 0.0f, abgr};
          vertices[i * 4 + 1] = {bx + step, by, 0.0f, abgr};
          vertices[i * 4 + 2] = {bx + step, by + barH, 0.0f, abgr};
          vertices[i * 4 + 3] = {bx, by + barH, 0.0f, abgr};

          index[i * 6 + 0] = i * 4 + 0;
          index[i * 6 + 1] = i * 4 + 1;
          index[i * 6 + 2] = i * 4 + 2;
          index[i * 6 + 3] = i * 4 + 2;
          index[i * 6 + 4] = i * 4 + 3;
          index[i * 6 + 5] = i * 4 + 0;
        }

        uint64_t state =
            BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA;
        bgfx::setState(state);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setIndexBuffer(&tib);
        static const bgfx::ProgramHandle kSimpleProgram =
            rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
        bgfx::submit(rendering::ui_view, kSimpleProgram);
      } else {
        // Fallback or just don't draw if too many
        SDL_Log("Too many points in gauge history to draw: %zu", count);
      }
    }
  }
}

void ResultScene::cleanupScene() {
  // Resources are cleaned up by Scene logic usually, but we have Views.
  // Scene::cleanup() deletes all views.
}
