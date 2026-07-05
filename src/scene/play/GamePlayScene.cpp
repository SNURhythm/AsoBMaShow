//
// Created by XF on 8/25/2024.
//

#include "GamePlayScene.h"
#include "GamePlayTiming.h"
#include "../../GBattleMode.h"
#include "../../PlayOptionUtils.h"
#include "../../PrepMetronome.h"
#include "../../ReplayDBHelper.h"
#include "../../ResultPresentationUtils.h"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../view/TextView.h"
#include "BMSRenderer.h"
#include "RhythmLaneInputController.h"
#include "../../input/RhythmInputHandler.h"
#include "../../targets.h"
#include "../../view/Button.h"
#include "../../view/UiTheme.h"
#include "../../scene/MainMenuScene.h"
#include "../ResultScene.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr long long kReplayTouchMoveMinIntervalMicros = 8000LL;
constexpr float kReplayTouchMoveMinDistance = 0.002f;
constexpr long long kHellChargeGaugeTickMicros = 200000LL;
constexpr long long kCoursePauseHoldMicros = 650000LL;
constexpr long long kCoursePauseRewindMicros = 260000LL;
constexpr float kPi = 3.14159265358979323846f;

long long nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string replayNoteKey(int lane, long long noteTimeMicros) {
  return std::to_string(lane) + ":" + std::to_string(noteTimeMicros);
}

void markPracticeSkippedNote(bms_parser::Note *note, long long startTime) {
  if (note == nullptr) {
    return;
  }
  note->IsPlayed = true;
  note->IsDead = true;
  note->PlayedTime = startTime;
  if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
      longNote != nullptr) {
    longNote->IsHolding = false;
    if (!longNote->IsTail() && longNote->Tail != nullptr) {
      longNote->Tail->IsPlayed = true;
      longNote->Tail->IsDead = true;
      longNote->Tail->PlayedTime = startTime;
      longNote->Tail->IsHolding = false;
    }
    if (longNote->IsTail() && longNote->Head != nullptr) {
      longNote->Head->IsPlayed = true;
      longNote->Head->IsDead = true;
      longNote->Head->PlayedTime = startTime;
      longNote->Head->IsHolding = false;
    }
  }
}

bool laneIsPressed(const std::unordered_map<int, bool> &lanePressed,
                   int lane) {
  const auto it = lanePressed.find(lane);
  return it != lanePressed.end() && it->second;
}

bool isInsideButton(const Button &button, float uiX, float uiY) {
  return uiX >= button.getX() && uiX <= button.getX() + button.getWidth() &&
         uiY >= button.getY() && uiY <= button.getY() + button.getHeight();
}

void mouseEventToUi(const SDL_MouseButtonEvent &event, float &uiX,
                    float &uiY) {
  const float screenX = static_cast<float>(event.x) * rendering::widthScale;
  const float screenY = static_cast<float>(event.y) * rendering::heightScale;
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

void mouseMotionToUi(const SDL_MouseMotionEvent &event, float &uiX,
                     float &uiY) {
  const float screenX = static_cast<float>(event.x) * rendering::widthScale;
  const float screenY = static_cast<float>(event.y) * rendering::heightScale;
  rendering::screenToUi(screenX, screenY, uiX, uiY);
}

void fingerEventToUi(const SDL_TouchFingerEvent &event, float &uiX,
                     float &uiY) {
  rendering::normalizedToUi(event.x, event.y, uiX, uiY);
}

void addRingArc(rendering::SimpleBatchRenderer &batch, float cx, float cy,
                float radius, float startAngle, float sweep, float thickness,
                uint32_t color) {
  if (radius <= 0.0f || thickness <= 0.0f || sweep <= 0.0f) {
    return;
  }

  const int segments =
      std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 28.0f))));
  float previousX = cx + std::cos(startAngle) * radius;
  float previousY = cy + std::sin(startAngle) * radius;
  for (int i = 1; i <= segments; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(segments);
    const float angle = startAngle + sweep * t;
    const float x = cx + std::cos(angle) * radius;
    const float y = cy + std::sin(angle) * radius;
    batch.addLine(previousX, previousY, x, y, thickness, color);
    previousX = x;
    previousY = y;
  }
}

JudgeResult normalizeLongNoteReleaseJudge(const JudgeResult &judgeResult) {
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    return JudgeResult(Bad, judgeResult.Diff);
  }
  return judgeResult;
}

JudgeResult judgeClassicLongNoteRelease(Judge &judge,
                                        bms_parser::LongNote *tail,
                                        long long releasedTime) {
  if (tail == nullptr || !tail->IsTail() || tail->Head == nullptr) {
    return JudgeResult(None, 0);
  }

  const JudgeResult headJudge =
      judge.judgeNow(tail->Head, tail->Head->PlayedTime);
  const JudgeResult tailJudge = judge.judgeNow(tail, releasedTime);
  const auto absDiff = [](long long value) {
    return value < 0 ? -value : value;
  };
  return normalizeLongNoteReleaseJudge(
      absDiff(tailJudge.Diff) > absDiff(headJudge.Diff) ? tailJudge
                                                        : headJudge);
}

bool longNoteTailJudgedBeforeTiming(const bms_parser::LongNote *longNote,
                                    long long judgedTime) {
  return longNote != nullptr && longNote->IsTail() &&
         longNote->Timeline != nullptr && longNote->Head != nullptr &&
         judgedTime < longNote->Timeline->Timing;
}

void markLongNoteMissed(bms_parser::LongNote *longNote, long long judgedTime,
                        bool dead = true) {
  if (longNote == nullptr) {
    return;
  }
  longNote->IsPlayed = true;
  longNote->IsDead = dead;
  longNote->PlayedTime = judgedTime;
  longNote->IsHolding = false;
}

void markReplayMissedNote(bms_parser::Note *note, long long judgedTime) {
  if (note == nullptr) {
    return;
  }
  note->IsPlayed = true;
  note->PlayedTime = judgedTime;
  if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
      longNote != nullptr) {
    note->IsDead = !longNoteTailJudgedBeforeTiming(longNote, judgedTime);
    longNote->IsHolding = false;
    if (longNote->IsTail() && longNote->Head != nullptr) {
      longNote->Head->IsHolding = false;
    } else if (!longNote->IsTail() && longNote->Tail != nullptr) {
      longNote->Tail->IsHolding = false;
    }
  } else {
    note->IsDead = true;
  }
}

std::string gameplayPlayOptionLabel(const StartOptions &options) {
  std::optional<std::string> option = options.playOption;
  std::optional<long long> seed = options.playOptionSeed;
  std::optional<std::string> option2 = options.playOption2;
  std::optional<long long> seed2 = options.playOption2Seed;

  if (options.replayData != nullptr) {
    if (!option.has_value()) {
      option = options.replayData->playOption;
    }
    if (!seed.has_value()) {
      seed = options.replayData->playOptionSeed;
    }
    if (!option2.has_value()) {
      option2 = options.replayData->playOption2;
    }
    if (!seed2.has_value()) {
      seed2 = options.replayData->playOption2Seed;
    }
  }
  if (options.gbattleRecordData != nullptr) {
    if (!option.has_value()) {
      option = options.gbattleRecordData->playOption;
    }
    if (!seed.has_value()) {
      seed = options.gbattleRecordData->playOptionSeed;
    }
    if (!option2.has_value()) {
      option2 = options.gbattleRecordData->playOption2;
    }
    if (!seed2.has_value()) {
      seed2 = options.gbattleRecordData->playOption2Seed;
    }
  }

  const std::string label =
      play_options::formatPlayOptionLabel(option, seed, option2, seed2);
  return label.empty() ? "" : "Option: " + label;
}

bool gameplayHasSamePatternRandomization(const bms_parser::Chart &chart,
                                         const StartOptions &options) {
  std::optional<std::string> option = options.playOption;
  std::optional<std::string> option2 = options.playOption2;

  if (options.replayData != nullptr) {
    if (!option.has_value()) {
      option = options.replayData->playOption;
    }
    if (!option2.has_value()) {
      option2 = options.replayData->playOption2;
    }
  }
  if (options.gbattleRecordData != nullptr) {
    if (!option.has_value()) {
      option = options.gbattleRecordData->playOption;
    }
    if (!option2.has_value()) {
      option2 = options.gbattleRecordData->playOption2;
    }
  }

  return play_options::hasSamePatternRandomization(chart.Meta, option, option2);
}

int noSpeedGreenNumberForChart(const bms_parser::Chart *chart) {
  const double bpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  const double referenceBpm = std::isfinite(bpm) && bpm > 0.0 ? bpm : 120.0;
  return std::max(1, static_cast<int>(std::lround(144000.0 / referenceBpm)));
}

bool prepareRetryChart(const bms_parser::ChartMeta &meta,
                       const StartOptions &sourceOptions,
                       std::unique_ptr<bms_parser::Chart> &retryChart,
                       StartOptions &retryOptions,
                       std::atomic_bool &cancelled) {
  retryChart = play_options::parseChart(meta.BmsPath, cancelled, "retry");
  if (retryChart == nullptr || cancelled) {
    return false;
  }
  if (sourceOptions.courseSession != nullptr) {
    applyCourseConstraintsToChart(*retryChart, sourceOptions.courseConstraints);
  }

  retryOptions = sourceOptions;
  retryOptions.startPosition = 0;
  retryOptions.autoPlay = false;
  retryOptions.replayData = nullptr;
  retryOptions.gbattleRecordData = nullptr;
  retryOptions.playOption.reset();
  retryOptions.playOptionSeed.reset();
  retryOptions.playOption2.reset();
  retryOptions.playOption2Seed.reset();
  retryOptions.ownsChart = true;

  std::optional<std::string> playOption = sourceOptions.playOption;
  std::optional<std::string> playOption2 = sourceOptions.playOption2;
  if (sourceOptions.replayData != nullptr) {
    if (!playOption.has_value()) {
      playOption = sourceOptions.replayData->playOption;
    }
    if (!playOption2.has_value()) {
      playOption2 = sourceOptions.replayData->playOption2;
    }
  }
  if (sourceOptions.gbattleRecordData != nullptr) {
    if (!playOption.has_value()) {
      playOption = sourceOptions.gbattleRecordData->playOption;
    }
    if (!playOption2.has_value()) {
      playOption2 = sourceOptions.gbattleRecordData->playOption2;
    }
  }

  if (playOption.has_value() &&
      !play_options::applyPlayOptionModifier(
          *retryChart, *playOption, std::nullopt, 0, retryOptions.playOption,
          retryOptions.playOptionSeed, "retry")) {
    return false;
  }

  if (retryChart->Meta.IsDP && playOption2.has_value() &&
      !play_options::applyPlayOptionModifier(
          *retryChart, *playOption2, std::nullopt, 1, retryOptions.playOption2,
          retryOptions.playOption2Seed, "retry")) {
    return false;
  }

  applyEffectiveLongNoteModeToChart(*retryChart,
                                    retryOptions.longNoteMode);
  return true;
}

#if defined(DEBUG) || defined(_DEBUG)
constexpr bool kShowLaneStateOverlay = true;
#else
constexpr bool kShowLaneStateOverlay = false;
#endif
} // namespace

GamePlayScene::GamePlayScene(ApplicationContext &context,
                             bms_parser::Chart *chart, StartOptions options)
    : Scene(context), ownedChart(options.ownsChart ? chart : nullptr),
      chart(options.ownsChart ? ownedChart.get() : chart),
      judge(chart->Meta.Rank), options(std::move(options)) {
  judge.applyCourseJudgementConstraint(this->options.courseConstraints.judgement);
  latePoorTiming = judge.timingWindows[Bad].second;
}

GamePlayScene::GamePlayScene(ApplicationContext &context,
                             std::unique_ptr<bms_parser::Chart> chart,
                             StartOptions options)
    : Scene(context), ownedChart(std::move(chart)),
      chart(ownedChart.get()), judge(this->chart->Meta.Rank),
      options(std::move(options)) {
  this->options.ownsChart = true;
  judge.applyCourseJudgementConstraint(this->options.courseConstraints.judgement);
  latePoorTiming = judge.timingWindows[Bad].second;
}

GamePlayScene::~GamePlayScene() = default;

void GamePlayScene::init() {
  if (chart != nullptr) {
    const int replayLongNoteMode =
        options.replayData != nullptr ? options.replayData->chartMeta.LnMode
        : (options.gbattleRecordData != nullptr
               ? options.gbattleRecordData->chartMeta.LnMode
               : 0);
    applyEffectiveLongNoteModeToChart(
        *chart, replayLongNoteMode > 0 ? replayLongNoteMode
                                       : options.longNoteMode);
  }
  ownedRenderer = std::make_unique<BMSRenderer>(
      chart, judge.timingWindows, effectiveVisibleTimeGreenNumber());
  renderer = ownedRenderer.get();
  renderer->setVisibleTimeBpmStrategy(
      courseNoSpeed() ? AppSettings::VisibleTimeBpmStrategy::Chart
                      : context.settings.visibleTimeBpmStrategy);
  renderer->setVisibleTimeUseMilliseconds(
      courseNoSpeed() ? false : context.settings.visibleTimeUseMilliseconds);
  renderer->setPlayAreaWidth(
      context.settings.playAreaWidthForKeyMode(chart->Meta.KeyMode));
  renderer->setLaneBeamLengthPercent(context.settings.laneBeamLengthPercent);
  renderer->setNoteStartPositionPercent(effectiveNoteStartPositionPercent());
  renderer->setLaneCoverFloatingEnabled(
      !courseNoSpeed() && context.settings.floatingLaneCoverEnabled);
  renderer->setJudgementIndicatorConfig(
      context.settings.judgementIndicatorEnabled,
      context.settings.judgementIndicatorY,
      context.settings.judgementIndicatorWidthScale,
      context.settings.judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D);
  renderer->setJudgementTextY(context.settings.judgementTextY);
  renderer->setJudgementTimingFastSlowCriteria(
      context.settings.judgementTimingFastSlowCriteria);
  renderer->setJudgementTimingMillisecondsCriteria(
      context.settings.judgementTimingMillisecondsCriteria);
  renderer->setJudgementCounterEnabled(
      context.settings.judgementCounterEnabled);
  renderer->setJudgementCounterPosition(
      context.settings.judgementCounterPosition);
  renderer->setGaugeBarPosition(context.settings.gaugeBarPosition);
  renderer->setReplayData(options.replayData.get());
  renderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
  renderer->setTouchVisualizationEnabled(
      options.touchVisualizationEnabled.value_or(
          context.settings.touchVisualizationEnabled));
  renderer->setReplayGhostRenderingEnabled(
      options.replayGhostRenderingEnabled.value_or(true));
  renderer->setPlayOptionStatus(gameplayPlayOptionLabel(options));
  std::string musicStopError;
  context.musicPlayer.Stop(musicStopError);
  context.jukebox.stop();
  reset();
  if (!isReplayPlayback() && !options.autoPlay) {
    ownedInputHandler = std::make_unique<RhythmInputHandler>(
        this, chart->Meta,
        context.settings.playAreaWidthForKeyMode(chart->Meta.KeyMode));
    inputHandler = ownedInputHandler.get();
    inputHandler->setDragModeEnabled(
        assist_options::isDragMode(options.assistOption));
    inputHandler->setTouchEventCallback(
        [this](SDL_FingerID fingerIndex, ReplayTouchAction action,
               Vector3 normalizedLocation) {
          return handleTouchInput(fingerIndex, action, normalizedLocation);
        });
    inputHandler->discardPendingTouchEvents();
    inputHandler->startListenSDL();
#if !(TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
    inputHandler->startListenTouch();
#endif
  }

  for (const auto &lane : chart->Meta.GetTotalLaneIndices()) {
    lanePressed[lane] = false;
  }

  ownedLaneInputController =
      std::make_unique<RhythmLaneInputController>(
          chart, renderer, lanePressed, options.courseConstraints.judgement,
          options.longNoteMode);
  laneInputController = ownedLaneInputController.get();

  if constexpr (kShowLaneStateOverlay) {
    ownedLaneStateText =
        std::make_unique<TextView>("assets/fonts/notosanscjkjp.ttf", 32);
    laneStateText = ownedLaneStateText.get();
    laneStateText->setPosition(100, 100);
    updateLaneStateText();
  }

  /* pause screen */
  const bool coursePlayback = isCoursePlayback();
  pauseLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(pauseLayout);
  pauseLayout->setFlexDirection(FlexDirection::Column);
  pauseLayout->setAlignItems(YGAlignCenter);
  pauseLayout->setJustifyContent(YGJustifyCenter);
  pauseLayout->setBackgroundColor(Color(2, 5, 9, 198));
  {
    auto pauseScreen = new View();
    pauseScreen->setWidth(520);
    pauseScreen->setHeight(430);
    pauseScreen->setFlexDirection(FlexDirection::Column);
    pauseScreen->setAlignItems(YGAlignCenter);
    pauseScreen->setJustifyContent(YGJustifyCenter);
    pauseScreen->setGap(14);
    pauseScreen->setPadding(Edge::All, 28);
    pauseScreen->setBackgroundColor(ui_theme::panelStrong());
    pauseScreen->setCornerRadius(ui_theme::panelRadius());
    pauseScreen->setShadow(ui_theme::shadow(), ui_theme::kModalShadow);
    pauseScreen->setBorderColor(ui_theme::hairlineSubtle());
    pauseScreen->setBorderWidth(1);
    {
      auto makePauseButton = [](const std::string &label, const Color &normal,
                                const Color &hover, const Color &pressed,
                                const Color &border, auto onClick) {
        auto button = new Button();
        auto text = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
        text->setText(label);
        text->setAlign(TextView::CENTER);
        text->setVAlign(TextView::MIDDLE);
        text->setColor(ui_theme::sdl(ui_theme::textPrimary()));
        button->setContentView(text);
        button->setOnClickListener(onClick);
        button->setSize(360, 64);
        button->setCornerRadius(ui_theme::controlRadius());
        button->setBackgroundColors(normal, hover, pressed);
        button->setBorderColors(ui_theme::withAlpha(border, 150),
                                ui_theme::withAlpha(border, 190),
                                ui_theme::withAlpha(border, 220));
        button->setStyledBorderWidth(1);
        return button;
      };

      auto pauseText = new TextView("assets/fonts/notosanscjkjp.ttf", 46);
      pauseText->setSize(420, 72);
      pauseText->setText(coursePlayback ? "COURSE MENU" : "PAUSED");
      pauseText->setAlign(TextView::CENTER);
      pauseText->setVAlign(TextView::MIDDLE);
      pauseText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
      pauseScreen->addView(pauseText);
      pauseScreen->addView(makePauseButton(
          coursePlayback ? "Close" : "Resume", Color(22, 132, 126, 238),
          Color(28, 151, 144, 248),
          Color(40, 173, 164, 255), ui_theme::accentBorderStrong(), [this]() {
        closePauseMenu();
      }));
      const bool canRetrySame =
          !coursePlayback && !isReplayPlayback() && !options.practiceMode &&
          chart != nullptr && gameplayHasSamePatternRandomization(*chart,
                                                                  options);
      pauseScreen->addView(makePauseButton(
          coursePlayback ? "Restart Course"
                         : (isReplayPlayback() ? "Replay" : "Retry"),
          Color(57, 105, 42, 238),
          Color(72, 127, 51, 248), Color(91, 153, 61, 255),
          ui_theme::lime(), [this, canRetrySame]() {
            if (isCoursePlayback()) {
              restartCourseFromBeginning();
            } else if (isReplayPlayback() || options.practiceMode ||
                       options.autoPlay || !canRetrySame) {
              restartCurrentPattern();
            } else {
              retryWithNewPattern();
            }
          }));
      if (canRetrySame) {
        pauseScreen->addView(makePauseButton(
            "Retry Same", ui_theme::control(), ui_theme::controlHover(),
            ui_theme::controlPressed(), ui_theme::hairline(),
            [this]() { restartCurrentPattern(); }));
      }
      pauseScreen->addView(makePauseButton("Exit", Color(119, 45, 46, 238),
                                           Color(145, 53, 51, 248),
                                           Color(174, 64, 57, 255),
                                           ui_theme::coral(), [this]() {
        finishReplayRecording();
        publishPracticeGhost();
        context.jukebox.stop();
        defer(
            [this]() {
              if (options.practiceMode && options.returnScene != nullptr) {
                context.sceneManager->changeScene(options.returnScene, false);
              } else {
                context.sceneManager->changeScene("MainMenu");
              }
              return false;
            },
            0, true);
      }));
    }

    pauseLayout->addView(pauseScreen);
  }
  pauseLayout->setVisible(false);

  /* pause button */
  pauseButton = new Button(rendering::window_width - 70, 50, 40, 40);
  addView(pauseButton);
  auto pauseText = new TextView("assets/fonts/notosanscjkjp.ttf", 28);
  pauseText->setText("||");
  pauseText->setAlign(TextView::CENTER);
  pauseText->setVAlign(TextView::MIDDLE);
  pauseText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  pauseButton->setContentView(pauseText);
  pauseButton->setSize(52, 52);
  pauseButton->setCornerRadius(ui_theme::controlRadius());
  pauseButton->setBackgroundColors(Color(236, 253, 255, 42),
                                   Color(70, 230, 224, 88),
                                   Color(255, 204, 81, 120));
  pauseButton->setBorderColors(ui_theme::hairlineSubtle(),
                               ui_theme::accentBorder(),
                               ui_theme::withAlpha(ui_theme::amber(), 190));
  pauseButton->setStyledBorderWidth(1);
  pauseButton->setOnClickListener([this]() {
    if (isCoursePlayback()) {
      return;
    }
    showPauseMenu(true);
  });
}

void GamePlayScene::reset() {
  ownedState.reset();
  state = nullptr;
  renderer->reset();
  if (laneInputController != nullptr) {
    laneInputController->resetLaneStates();
    updateLaneStateText();
  }
  // reset all notes
  for (const auto &measure : chart->Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (const auto &note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
      for (const auto &note : timeline->InvisibleNotes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
      for (const auto &note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        note->Reset();
      }
    }
  }
  context.jukebox.stop();
  const long long startPositionMicros = getStartPositionMicros();
  const std::optional<long long> practiceKeySoundCutoff =
      options.practiceLeadInMicros > 0 ? std::optional<long long>(
                                             startPositionMicros)
                                       : std::nullopt;
  const long long audioSeekPosition = getAudioSeekPositionMicros();
  const bool prepMetronomeEnabled = gameplay_timing::shouldApplyPrepMetronome(
      context.settings.prepMetronomeEnabled, options.practiceLeadInMicros,
      startPositionMicros);
  const auto prepPlan = prep_metronome::buildPlan(
      *chart, prepMetronomeEnabled, false, audioSeekPosition);
  context.jukebox.schedule(*chart, options.autoKeySound, isCancelled,
                           practiceKeySoundCutoff,
                           prepPlan.enabled ? &prepPlan : nullptr);
  context.jukebox.play(prepPlan.enabled ? prepPlan.startTimeMicros
                                        : audioSeekPosition);
  currentGameplayBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  if (renderer != nullptr) {
    renderer->setCurrentBpm(currentGameplayBpm);
  }
  ownedState = std::make_unique<RhythmState>(chart, false);
  state = ownedState.get();
  const bool courseReplayPlayback =
      isReplayPlayback() && isCoursePlayback() &&
      options.courseSession != nullptr &&
      options.courseSession->courseReplayPlayback;
  const GaugeType initialGaugeType =
      courseReplayPlayback
          ? options.gaugeType
          : (isReplayPlayback() ? options.replayData->initialGaugeType
                                : options.gaugeType);
  const GaugeProfile gaugeProfile =
      isReplayPlayback() && !isCoursePlayback() ? GaugeProfile::Standard
                                                : options.gaugeProfile;
  const bool gaugeAutoShift =
      courseReplayPlayback
          ? options.gaugeAutoShift
          : (isReplayPlayback() ? options.replayData->gaugeAutoShift
                                : options.gaugeAutoShift);
  state->configureGauge(initialGaugeType, gaugeAutoShift, gaugeProfile);
  if (options.courseSession != nullptr &&
      options.courseSession->carriedGauge.has_value()) {
    state->restoreGaugeState(*options.courseSession->carriedGauge);
  }
  if (isCoursePlayback()) {
    state->combo = options.courseSession->carriedCombo;
    state->maxCombo = options.courseSession->maxCombo;
  }
  const std::string assistOption =
      isReplayPlayback() ? options.replayData->assistOption
                         : options.assistOption;
  state->setAssistClearMark(assist_options::isEnabled(assistOption));
  initializeStartPositionState();
  configurePacemakerTarget();
  updatePacemakerStatus();
  resetHellChargeGaugeTracking(
      getGameplayTimeMicros(context.jukebox.getTimeMicros()));
  state->isPlaying = true;
  renderer->setJudgementCounters(state->judgeCount, state->comboBreak);
  renderer->setAutoPlayMarkVisible(
      options.autoPlay ||
      (options.replayData != nullptr && options.replayData->autoPlay));
  replayKeySoundCursor = 0;
  replayEventCursor = 0;
  replayLaneCoverCursor = 0;
  touchVisualizerLoaded = false;
  floatingLaneCoverDragActive = false;
  floatingLaneCoverDragChanged = false;
  floatingLaneCoverFinger = -1;
  floatingLaneCoverDragOffsetY = 0.0f;
  if (isReplayPlayback()) {
    const long long initialReplayTime =
        getGameplayTimeMicros(context.jukebox.getTimeMicros());
    processReplayLaneCoverEvents(initialReplayTime);
  }
  buildReplayNoteLookup();
  beginReplayRecording();
  updateGaugeStatusText();
}

void GamePlayScene::showPauseMenu(bool pausePlayback) {
  if (pausePlayback) {
    context.jukebox.pause();
  }
  if (renderer != nullptr) {
    renderer->clearLiveTouchPoints();
  }
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(true);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(false);
  }
  resetCoursePauseHold();
}

void GamePlayScene::closePauseMenu() {
  if (!isCoursePlayback() && context.jukebox.isPaused()) {
    context.jukebox.resume();
  }
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  resetCoursePauseHold();
}

void GamePlayScene::restartCurrentPattern() {
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  resetCoursePauseHold();
  context.jukebox.stop();
  defer(
      [this]() {
        reset();
        return true;
      },
      0, true);
}

bool GamePlayScene::restartCourseFromBeginning() {
  auto session = options.courseSession;
  if (session == nullptr || session->entries.empty()) {
    return false;
  }

  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  resetCoursePauseHold();

  session->currentIndex = 0;
  session->completedResults.clear();
  if (!session->courseReplayPlayback) {
    session->replayStages.clear();
  }
  session->carriedGauge.reset();
  session->carriedCombo = 0;
  session->maxCombo = 0;
  session->courseScoreSaved = false;
  session->playOption.reset();
  session->playOptionSeed.reset();
  session->playOption2.reset();
  session->playOption2Seed.reset();
  context.jukebox.stop();
  defer(
      [this]() {
        const bool started =
            options.courseSession != nullptr &&
                    options.courseSession->courseReplayPlayback
                ? startCourseReplayChartAtCurrentIndex()
                : startCourseChartAtCurrentIndex();
        if (!started) {
          SDL_Log("Failed to restart course from the first chart.");
          context.sceneManager->changeScene("MainMenu");
        }
        return false;
      },
      0, true);
  return true;
}

void GamePlayScene::retryWithNewPattern() {
  if (isReplayPlayback()) {
    restartCurrentPattern();
    return;
  }

  pauseLayout->setVisible(false);
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  resetCoursePauseHold();
  context.jukebox.stop();

  defer(
      [this]() {
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> retryChart;
        StartOptions retryOptions;
        if (!prepareRetryChart(chart->Meta, options, retryChart, retryOptions,
                               parseCancelled)) {
          SDL_Log("Failed to prepare retry chart for: %s",
                  chart->Meta.Title.c_str());
          reset();
          return true;
        }

        context.jukebox.stop();
        context.jukebox.reloadChartResources(*retryChart, true, parseCancelled);
        if (parseCancelled) {
          reset();
          return true;
        }

        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(context, std::move(retryChart),
                                            retryOptions),
            false);
        return false;
      },
      0, true);
}

bool GamePlayScene::isReplayPlayback() const {
  return options.replayData != nullptr;
}

bool GamePlayScene::isCoursePlayback() const {
  return options.courseSession != nullptr &&
         options.courseSession->validCurrentIndex();
}

bool GamePlayScene::courseNoSpeed() const {
  return isCoursePlayback() && options.courseConstraints.noSpeed;
}

int GamePlayScene::effectiveVisibleTimeGreenNumber() const {
  return courseNoSpeed() ? noSpeedGreenNumberForChart(chart)
                         : context.settings.visibleTimeGreenNumber;
}

int GamePlayScene::effectiveNoteStartPositionPercent() const {
  return courseNoSpeed() ? AppSettings::kDefaultNoteStartPositionPercent
                         : context.settings.noteStartPositionPercent;
}

bool GamePlayScene::shouldRecordReplay() const {
  return !options.autoPlay && !isReplayPlayback();
}

bool GamePlayScene::shouldPersistRecordedReplay() const {
  return shouldRecordReplay() && !options.practiceMode && !isCoursePlayback();
}

void GamePlayScene::configurePacemakerTarget() {
  activePacemakerTarget = {};
  if (renderer == nullptr) {
    return;
  }

  const std::string selected =
      pacemaker::normalizeTargetId(options.pacemakerTarget);
  if (chart == nullptr || options.autoPlay || options.practiceMode ||
      isCoursePlayback()) {
    renderer->setPacemakerTarget(activePacemakerTarget);
    return;
  }

  if (options.gbattleRecordData != nullptr) {
    activePacemakerTarget =
        gbattle::targetFromRecord(*chart, *options.gbattleRecordData);
    renderer->setPacemakerTarget(activePacemakerTarget);
    return;
  }

  if (selected == pacemaker::kTargetOff) {
    renderer->setPacemakerTarget(activePacemakerTarget);
    return;
  }

  if (isReplayPlayback()) {
    if (options.replayData == nullptr || options.replayData->autoPlay) {
      renderer->setPacemakerTarget(activePacemakerTarget);
      return;
    }

    activePacemakerTarget = result_presentation::pacemakerTargetForReplay(
        *chart, *options.replayData, selected,
        result_presentation::previousBestForReplayChart(chart->Meta,
                                                        *options.replayData));
    renderer->setPacemakerTarget(activePacemakerTarget);
    return;
  }

  std::optional<ScoreBestSnapshot> best;
  std::optional<ReplayData> bestReplay;
  if (selected == pacemaker::kTargetBest) {
    best = ScoreDBHelper::GetInstance().LoadBestScore(chart->Meta);
    if (best.has_value() && best->score > 0) {
      const auto summaries =
          ReplayDBHelper::GetInstance().ListReplays(chart->Meta, 100);
      for (const ReplaySummary &summary : summaries) {
        if (summary.courseReplay || summary.autoPlay ||
            summary.finalScore != best->score || summary.eventCount <= 0) {
          continue;
        }

        auto replay =
            ReplayDBHelper::GetInstance().LoadReplay(summary.id, chart->Meta);
        if (!replay.has_value() ||
            replay->finalScore != best->score) {
          continue;
        }

        const std::vector<int> progression =
            pacemaker::buildReplayScoreProgression(*chart, *replay);
        if (!progression.empty() && progression.back() == best->score) {
          bestReplay = std::move(*replay);
          break;
        }
      }
    }
  }

  activePacemakerTarget = pacemaker::targetFromSelection(
      *chart, selected, best,
      bestReplay.has_value() ? &bestReplay.value() : nullptr);
  renderer->setPacemakerTarget(activePacemakerTarget);
}

void GamePlayScene::updatePacemakerStatus() {
  if (renderer == nullptr || state == nullptr) {
    return;
  }
  renderer->setPacemakerStatus(
      pacemaker::snapshotForState(activePacemakerTarget, *state));
}

bool GamePlayScene::startCourseReplayChartAtCurrentIndex() {
  auto session = options.courseSession;
  if (session == nullptr ||
      !session->hasCourseReplayStage(session->currentIndex)) {
    return false;
  }

  auto stageReplay = session->currentCourseReplayStageReplay();
  if (stageReplay == nullptr) {
    return false;
  }
  session->applyReplayStagePlayOptions(*stageReplay);

  std::atomic_bool parseCancelled = false;
  auto replayChart = play_options::prepareReplayChart(
      stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  if (replayChart == nullptr || parseCancelled) {
    return false;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*replayChart, true, parseCancelled);
  if (parseCancelled) {
    return false;
  }

  StartOptions nextOptions =
      makeCourseReplayStageStartOptions(session, stageReplay);

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(replayChart),
                                      std::move(nextOptions)),
      false);
  return true;
}

bool GamePlayScene::startCourseChartAtCurrentIndex() {
  auto session = options.courseSession;
  if (session == nullptr || !session->validCurrentIndex()) {
    return false;
  }

  const bms_parser::ChartMeta *nextMeta = session->currentMeta();
  if (nextMeta == nullptr || nextMeta->BmsPath.empty()) {
    return false;
  }

  std::atomic_bool parseCancelled = false;
  std::unique_ptr<bms_parser::Chart> nextChart;
  try {
    nextChart = play_options::parseChart(nextMeta->BmsPath, parseCancelled,
                                         "course");
  } catch (const std::exception &e) {
    SDL_Log("Course parse failed %s: %s",
            fspath_to_utf8(nextMeta->BmsPath).c_str(), e.what());
    archive_file::appendDebugLogLine(
        "Course parse exception: " + fspath_to_utf8(nextMeta->BmsPath) + ": " +
        e.what());
    return false;
  }
  if (nextChart == nullptr || parseCancelled) {
    return false;
  }
  applyCourseConstraintsToChart(*nextChart, session->constraints);

  play_options::PlayOptionReplayInfo playInfo =
      play_options::applySelectedPlayOptions(*nextChart,
                                             session->requestedPlayOption);
  applyEffectiveLongNoteModeToChart(*nextChart, options.longNoteMode);
  session->playOption = playInfo.option;
  session->playOptionSeed = playInfo.seed;
  session->playOption2 = playInfo.option2;
  session->playOption2Seed = playInfo.seed2;

  context.jukebox.stop();
  context.jukebox.loadChart(*nextChart, true, parseCancelled);
  if (parseCancelled) {
    return false;
  }

  StartOptions nextOptions;
  nextOptions.startPosition = 0;
  nextOptions.autoKeySound = session->autoKeySound;
  nextOptions.autoPlay = false;
  nextOptions.gaugeType = session->gaugeType;
  nextOptions.gaugeProfile = session->gaugeProfile;
  nextOptions.gaugeAutoShift = session->gaugeAutoShift;
  nextOptions.playOption = playInfo.option;
  nextOptions.playOptionSeed = playInfo.seed;
  nextOptions.playOption2 = playInfo.option2;
  nextOptions.playOption2Seed = playInfo.seed2;
  nextOptions.longNoteMode = options.longNoteMode;
  nextOptions.assistOption = session->assistOption;
  nextOptions.courseSession = session;
  nextOptions.courseConstraints = session->constraints;
  nextOptions.ownsChart = true;

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(nextChart),
                                      std::move(nextOptions)),
      false);
  return true;
}

bool GamePlayScene::startNextCourseChart() {
  auto session = options.courseSession;
  if (session == nullptr || !session->hasNextChart()) {
    return false;
  }

  session->currentIndex++;
  return startCourseChartAtCurrentIndex();
}

void GamePlayScene::beginReplayRecording() {
  practiceGhostPublished = false;
  lastRecordedTouchSamples.clear();
  if (!shouldRecordReplay()) {
    recordedReplay = {};
    return;
  }

  recordedReplay = {};
  recordedReplay.chartMeta = chart->Meta;
  recordedReplay.randomSeed = chart->Meta.RandomSeed;
  recordedReplay.randomPrng = chart->Meta.RandomPrng;
  recordedReplay.randomValues = chart->Meta.RandomValues;
  recordedReplay.playOption = options.playOption;
  recordedReplay.playOptionSeed = options.playOptionSeed;
  recordedReplay.playOption2 = options.playOption2;
  recordedReplay.playOption2Seed = options.playOption2Seed;
  recordedReplay.assistOption = assist_options::normalize(options.assistOption);
  recordedReplay.initialGaugeType = options.gaugeType;
  recordedReplay.gaugeAutoShift = options.gaugeAutoShift;
  recordedReplay.finalScore = 0;
  recordedReplay.finalGauge = state != nullptr ? state->currentGauge : 0.0f;
  recordedReplay.clearType = kClearTypeFailedRank;
  recordedReplay.events.reserve(
      static_cast<size_t>(std::max(0, chart->Meta.TotalNotes)) * 2);
  recordedReplay.touchSamples.reserve(1024);
  recordedReplay.laneCoverEvents.reserve(128);
  appendReplayLaneCoverEvent(effectiveNoteStartPositionPercent(), 0,
                             false);
}

void GamePlayScene::finishReplayRecording() {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  recordedReplay.finalScore = state->getScore();
  recordedReplay.finalGauge = state->currentGauge;
  recordedReplay.clearType = state->getClearTypeRank();
}

void GamePlayScene::publishPracticeGhost() {
  if (!options.practiceMode || practiceGhostPublished ||
      !options.practiceGhostCallback || recordedReplay.events.empty()) {
    return;
  }

  finishReplayRecording();
  practiceGhostPublished = true;
  options.practiceGhostCallback(recordedReplay);
}

long long GamePlayScene::getAudioOffsetMicros() const {
  return static_cast<long long>(context.settings.audioOffsetMs) * 1000LL;
}

long long GamePlayScene::getStartPositionMicros() const {
  const long long requested =
      static_cast<long long>(std::min<unsigned long long>(
          options.startPosition,
          static_cast<unsigned long long>(
              std::max(0LL, chart != nullptr ? chart->Meta.TotalLength : 0LL))));
  return std::max(0LL, requested);
}

long long GamePlayScene::getAudioSeekPositionMicros() const {
  const long long startPosition = getStartPositionMicros();
  const long long leadIn =
      static_cast<long long>(std::min<unsigned long long>(
          options.practiceLeadInMicros,
          static_cast<unsigned long long>(std::max(0LL, startPosition))));
  return std::max(0LL, startPosition - leadIn);
}

void GamePlayScene::initializeStartPositionState() {
  if (state == nullptr || chart == nullptr) {
    return;
  }

  const long long startPosition = getStartPositionMicros();
  if (startPosition <= 0) {
    return;
  }

  bool foundStartTimeline = false;
  for (size_t measureIndex = 0; measureIndex < chart->Measures.size();
       ++measureIndex) {
    const auto *measure = chart->Measures[measureIndex];
    if (measure == nullptr) {
      continue;
    }

    for (size_t timelineIndex = 0; timelineIndex < measure->TimeLines.size();
         ++timelineIndex) {
      auto *timeline = measure->TimeLines[timelineIndex];
      if (timeline == nullptr) {
        continue;
      }

      if (timeline->Timing <= startPosition) {
        applyTimelineBpm(timeline);
      }

      if (timeline->Timing >= startPosition) {
        state->passedMeasureCount = measureIndex;
        state->passedTimelineCount = timelineIndex;
        foundStartTimeline = true;
        break;
      }

      for (auto *note : timeline->Notes) {
        markPracticeSkippedNote(note, startPosition);
      }
      for (auto *note : timeline->LandmineNotes) {
        markPracticeSkippedNote(note, startPosition);
      }
    }

    if (foundStartTimeline) {
      break;
    }
  }

  if (!foundStartTimeline) {
    state->passedMeasureCount = chart->Measures.size();
    state->passedTimelineCount = 0;
  }
}

void GamePlayScene::applyTimelineBpm(const bms_parser::TimeLine *timeline) {
  if (timeline == nullptr || !timeline->BpmChange ||
      !std::isfinite(timeline->Bpm) || timeline->Bpm <= 0.0) {
    return;
  }
  if (std::abs(currentGameplayBpm - timeline->Bpm) <= 0.0001) {
    return;
  }
  currentGameplayBpm = timeline->Bpm;
  if (renderer != nullptr) {
    renderer->setCurrentBpm(currentGameplayBpm);
  }
}

long long
GamePlayScene::getGameplayTimeMicros(long long rawSongTimeMicros) const {
  return rawSongTimeMicros + getAudioOffsetMicros();
}

long long GamePlayScene::getInputSongTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return songTimeMicros - static_cast<long long>(inputDelay * 1000000);
}

long long GamePlayScene::getJudgementTimeMicros(long long songTimeMicros,
                                                double inputDelay) const {
  return getInputSongTimeMicros(songTimeMicros, inputDelay);
}

long long GamePlayScene::getVisualOffsetMicros() const {
  return static_cast<long long>(context.settings.visualOffsetMs) * 1000LL;
}

long long GamePlayScene::getVisualTimeMicros(long long songTimeMicros) const {
  return gameplay_timing::visualTimeMicros(songTimeMicros,
                                           getVisualOffsetMicros());
}

void GamePlayScene::update(float dt) {
  (void)dt;
  if (inputHandler != nullptr) {
    inputHandler->pumpPendingTouchEvents();
  }
  updateCoursePauseHoldProgress(nowMicros());
  if (state == nullptr || !state->isPlaying || state->isEnding) {
    return;
  }

  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  const long long gameplayTimeMicros = getGameplayTimeMicros(rawSongTimeMicros);
  touchVisualizerLoaded = true;
  if (isReplayPlayback()) {
    processReplayKeySounds(rawSongTimeMicros);
    processReplayEvents(gameplayTimeMicros);
  }
  updateHellChargeGauge(gameplayTimeMicros);
  checkPassedTimeline(gameplayTimeMicros);
  if (isReplayPlayback()) {
    processReplayLaneCoverEvents(gameplayTimeMicros);
  }
  if (state->passedMeasureCount != chart->Measures.size()) {
    return;
  }

  SDL_Log("All measures passed");
  state->isEnding = true;
  finishReplayRecording();
  publishPracticeGhost();
  if (isCoursePlayback()) {
    options.courseSession->carriedGauge = state->gaugeSnapshot();
    options.courseSession->carriedCombo = state->combo;
    options.courseSession->maxCombo =
        std::max(options.courseSession->maxCombo, state->maxCombo);
    options.courseSession->recordResult(chart->Meta, *state);
    if (shouldRecordReplay()) {
      options.courseSession->recordReplayStage(recordedReplay);
    }
  }
  defer(
      [this]() {
        const ReplayData *replayToSave =
            shouldPersistRecordedReplay() ? &recordedReplay : nullptr;
        const ReplayData *retrySource =
            replayToSave != nullptr
                ? replayToSave
                : (options.replayData != nullptr ? options.replayData.get()
                                                 : nullptr);
        ResultPracticeOptions practiceResultOptions;
        if (options.practiceMode) {
          practiceResultOptions.enabled = true;
          practiceResultOptions.startPosition =
              static_cast<unsigned long long>(getStartPositionMicros());
          practiceResultOptions.autoKeySound = options.autoKeySound;
          practiceResultOptions.autoPlay = options.autoPlay;
          practiceResultOptions.gaugeType = options.gaugeType;
          practiceResultOptions.gaugeAutoShift = options.gaugeAutoShift;
          practiceResultOptions.playOption = options.playOption;
          practiceResultOptions.playOptionSeed = options.playOptionSeed;
          practiceResultOptions.playOption2 = options.playOption2;
          practiceResultOptions.playOption2Seed = options.playOption2Seed;
          practiceResultOptions.longNoteMode = options.longNoteMode;
          practiceResultOptions.assistOption = options.assistOption;
          practiceResultOptions.leadInMicros = options.practiceLeadInMicros;
          practiceResultOptions.returnScene = options.returnScene;
          practiceResultOptions.practiceGhostCallback =
              options.practiceGhostCallback;
        }
        ResultCourseOptions courseResultOptions;
        if (isCoursePlayback()) {
          courseResultOptions.mode = ResultCourseMode::Stage;
          courseResultOptions.session = options.courseSession;
        }
        const bool replayPacemakerResult =
            !options.autoPlay && !options.practiceMode && isReplayPlayback() &&
            !isCoursePlayback() && options.replayData != nullptr &&
            !options.replayData->autoPlay;
        const std::string resultPacemakerTarget =
            (!options.autoPlay && !options.practiceMode &&
             ((!isReplayPlayback() && !isCoursePlayback()) ||
              replayPacemakerResult))
                ? options.pacemakerTarget
                : pacemaker::kTargetOff;
        const bms_parser::ChartMeta resultMeta = chart->Meta;
        std::unique_ptr<bms_parser::Chart> ownedReusableRetryChart;
        bms_parser::Chart *reusableRetryChart = nullptr;
        if (!isCoursePlayback()) {
          if (ownedChart != nullptr) {
            ownedReusableRetryChart = std::move(ownedChart);
            reusableRetryChart = ownedReusableRetryChart.get();
            chart = reusableRetryChart;
          } else {
            reusableRetryChart = chart;
          }
        }
        context.sceneManager->changeScene(
            std::make_unique<ResultScene>(
                context, resultMeta, *state, replayToSave,
                !options.autoPlay && !options.practiceMode &&
                    !isReplayPlayback() && !isCoursePlayback(),
                retrySource, practiceResultOptions,
                options.autoPlay ||
                    (options.replayData != nullptr &&
                     options.replayData->autoPlay),
                courseResultOptions, resultPacemakerTarget,
                std::move(ownedReusableRetryChart), reusableRetryChart),
            false);
        return false;
      },
      2000, true);
}

void GamePlayScene::renderScene() {
  RenderContext renderContext;
  pauseLayout->setSize(rendering::window_width, rendering::window_height);
  if (pauseButton != nullptr) {
    pauseButton->setPositionNoLayout(rendering::window_width - 88, 38);
  }
  const long long gameplayTimeMicros =
      getGameplayTimeMicros(context.jukebox.getTimeMicros());
  renderer->render(renderContext, getVisualTimeMicros(gameplayTimeMicros),
                   gameplayTimeMicros);
  renderCoursePauseHoldRing();
  if (laneStateText != nullptr) {
    laneStateText->render(renderContext);
  }
}

bool GamePlayScene::renderViewBeforeScene(const View *view) const {
  return view != pauseLayout && view != pauseButton;
}

bool GamePlayScene::handleCoursePauseButtonEvent(SDL_Event &event) {
  if (!isCoursePlayback() || pauseButton == nullptr ||
      !pauseButton->getVisible()) {
    return false;
  }

  switch (event.type) {
  case SDL_MOUSEBUTTONDOWN: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    mouseEventToUi(event.button, uiX, uiY);
    if (!isInsideButton(*pauseButton, uiX, uiY)) {
      return false;
    }
    beginCoursePauseHold(false, -1);
    return true;
  }
  case SDL_MOUSEBUTTONUP: {
    if (event.button.button != SDL_BUTTON_LEFT ||
        event.button.which == SDL_TOUCH_MOUSEID || !coursePauseHoldActive ||
        coursePauseHoldTouch) {
      return false;
    }
    cancelCoursePauseHold();
    return true;
  }
  case SDL_MOUSEMOTION: {
    if (!coursePauseHoldActive || coursePauseHoldTouch) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    mouseMotionToUi(event.motion, uiX, uiY);
    if (!isInsideButton(*pauseButton, uiX, uiY)) {
      cancelCoursePauseHold();
    }
    return true;
  }
  case SDL_FINGERDOWN: {
    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (!isInsideButton(*pauseButton, uiX, uiY)) {
      return false;
    }
    if (!coursePauseHoldActive) {
      beginCoursePauseHold(true, event.tfinger.fingerId);
    }
    return true;
  }
  case SDL_FINGERUP: {
    if (!coursePauseHoldActive || !coursePauseHoldTouch ||
        event.tfinger.fingerId != coursePauseHoldFinger) {
      return false;
    }
    cancelCoursePauseHold();
    return true;
  }
  case SDL_FINGERMOTION: {
    if (!coursePauseHoldActive || !coursePauseHoldTouch ||
        event.tfinger.fingerId != coursePauseHoldFinger) {
      return false;
    }
    float uiX = 0.0f;
    float uiY = 0.0f;
    fingerEventToUi(event.tfinger, uiX, uiY);
    if (!isInsideButton(*pauseButton, uiX, uiY)) {
      cancelCoursePauseHold();
    }
    return true;
  }
  case SDL_WINDOWEVENT:
    if (event.window.event == SDL_WINDOWEVENT_LEAVE &&
        coursePauseHoldActive && !coursePauseHoldTouch) {
      cancelCoursePauseHold();
      return true;
    }
    break;
  default:
    break;
  }

  return false;
}

void GamePlayScene::beginCoursePauseHold(bool touch, SDL_FingerID fingerId) {
  coursePauseHoldActive = true;
  coursePauseHoldRewinding = false;
  coursePauseHoldTouch = touch;
  coursePauseHoldFinger = fingerId;
  coursePauseHoldStartMicros = nowMicros() -
                               static_cast<long long>(
                                   coursePauseHoldProgress *
                                   static_cast<float>(kCoursePauseHoldMicros));
}

void GamePlayScene::cancelCoursePauseHold() {
  if (!coursePauseHoldActive) {
    return;
  }

  updateCoursePauseHoldProgress(nowMicros());
  coursePauseHoldActive = false;
  coursePauseHoldTouch = false;
  coursePauseHoldFinger = -1;
  if (coursePauseHoldProgress <= 0.0f) {
    resetCoursePauseHold();
    return;
  }

  coursePauseHoldRewinding = true;
  coursePauseHoldRewindStartMicros = nowMicros();
  coursePauseHoldRewindStartProgress = coursePauseHoldProgress;
}

void GamePlayScene::resetCoursePauseHold() {
  coursePauseHoldActive = false;
  coursePauseHoldRewinding = false;
  coursePauseHoldTouch = false;
  coursePauseHoldFinger = -1;
  coursePauseHoldStartMicros = 0;
  coursePauseHoldRewindStartMicros = 0;
  coursePauseHoldProgress = 0.0f;
  coursePauseHoldRewindStartProgress = 0.0f;
}

void GamePlayScene::updateCoursePauseHoldProgress(long long currentMicros) {
  if (!isCoursePlayback()) {
    resetCoursePauseHold();
    return;
  }

  if (coursePauseHoldActive) {
    const float progress = static_cast<float>(
                               currentMicros - coursePauseHoldStartMicros) /
                           static_cast<float>(kCoursePauseHoldMicros);
    coursePauseHoldProgress = std::clamp(progress, 0.0f, 1.0f);
    if (coursePauseHoldProgress >= 1.0f) {
      showPauseMenu(false);
    }
    return;
  }

  if (!coursePauseHoldRewinding) {
    return;
  }

  const float rewindProgress =
      static_cast<float>(currentMicros - coursePauseHoldRewindStartMicros) /
      static_cast<float>(kCoursePauseRewindMicros);
  if (rewindProgress >= 1.0f) {
    resetCoursePauseHold();
    return;
  }
  coursePauseHoldProgress =
      coursePauseHoldRewindStartProgress *
      (1.0f - std::clamp(rewindProgress, 0.0f, 1.0f));
}

void GamePlayScene::renderCoursePauseHoldRing() {
  if (!isCoursePlayback() || pauseButton == nullptr ||
      !pauseButton->getVisible()) {
    return;
  }

  updateCoursePauseHoldProgress(nowMicros());
  if (pauseButton == nullptr || !pauseButton->getVisible()) {
    return;
  }

  const float cx = static_cast<float>(pauseButton->getX()) +
                   static_cast<float>(pauseButton->getWidth()) * 0.5f;
  const float cy = static_cast<float>(pauseButton->getY()) +
                   static_cast<float>(pauseButton->getHeight()) * 0.5f;
  const float radius =
      static_cast<float>(std::max(pauseButton->getWidth(),
                                  pauseButton->getHeight())) *
          0.5f +
      8.0f;
  constexpr float thickness = 4.0f;
  const uint32_t baseColor = Color(236, 253, 255, 72).toABGR();
  const uint32_t progressColor = ui_theme::amber().toABGR();

  rendering::SimpleBatchRenderer batch;
  batch.setSubmitView(rendering::ui_view);
  batch.begin();
  addRingArc(batch, cx, cy, radius, -kPi * 0.5f, kPi * 2.0f, thickness,
             baseColor);
  if (coursePauseHoldProgress > 0.001f) {
    addRingArc(batch, cx, cy, radius, -kPi * 0.5f,
               kPi * 2.0f * std::clamp(coursePauseHoldProgress, 0.0f, 1.0f),
               thickness + 1.0f, progressColor);
  }
  batch.end();
}

void GamePlayScene::cleanupScene() {
  SDL_Log("Cleaning up GamePlayScene");
  context.jukebox.removeOnTick();
  SDL_Log("Stopping input handler");
  if (inputHandler != nullptr) {
    inputHandler->stopListen();
  }
  ownedInputHandler.reset();
  inputHandler = nullptr;
  ownedLaneInputController.reset();
  laneInputController = nullptr;
  hellChargeGaugeBalanceMicros.clear();
  ownedRenderer.reset();
  renderer = nullptr;
  ownedState.reset();
  state = nullptr;
  ownedLaneStateText.reset();
  laneStateText = nullptr;
  ownedChart.reset();
  chart = nullptr;
  SDL_Log("Cleaned up GamePlayScene");
}
bms_parser::Note *GamePlayScene::pressLane(int lane, double inputDelay) {
  if (laneInputController == nullptr) {
    return nullptr;
  }
  return pressLane(lane, lane, inputDelay);
}
bms_parser::Note *GamePlayScene::pressLane(int mainLane, int compensateLane,
                                           double inputDelay) {
  if (context.jukebox.isPaused()) {
    return nullptr;
  }
  if (isGamePaused || state == nullptr || !state->isPlaying ||
      state->isEnding) {
    return nullptr;
  }
  if (laneInputController == nullptr) {
    return nullptr;
  }
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = getGameplayTimeMicros(context.jukebox.getTimeMicros()),
      .laneBeamTimeMicros = nowMicros(),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  auto result =
      laneInputController->pressLane(mainLane, compensateLane, inputContext);
  updateLaneStateText();
  if (result.keySoundNote != nullptr &&
      result.keySoundNote->Wav != bms_parser::Parser::NoWav &&
      !options.autoKeySound && !isReplayPlayback()) {
    context.jukebox.playKeySound(result.keySoundNote->Wav);
  }
  if (result.hasJudge) {
    onJudge(result.judge, !options.autoPlay || isReplayPlayback());
  }
  if (result.hasReplayEvent) {
    const auto &event = result.replayEvent;
    appendReplayEvent(event.action, event.lane, event.note,
                      event.songTimeMicros, event.judgeTimeMicros,
                      event.judge);
  }
  return result.note;
}
bms_parser::Note *GamePlayScene::releaseLane(int lane, double inputDelay,
                                             bool isBackSpin) {
  if (isGamePaused || state == nullptr || !state->isPlaying ||
      state->isEnding) {
    return nullptr;
  }
  if (laneInputController == nullptr) {
    return nullptr;
  }
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = getGameplayTimeMicros(context.jukebox.getTimeMicros()),
      .laneBeamTimeMicros = nowMicros(),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  auto result =
      laneInputController->releaseLane(lane, inputContext, isBackSpin);
  updateLaneStateText();
  if (result.hasJudge) {
    onJudge(result.judge, !options.autoPlay || isReplayPlayback());
  }
  if (result.hasReplayEvent) {
    const auto &event = result.replayEvent;
    appendReplayEvent(event.action, event.lane, event.note,
                      event.songTimeMicros, event.judgeTimeMicros,
                      event.judge);
  }
  return result.note;
}
void GamePlayScene::checkPassedTimeline(long long time) {
  const auto &measures = chart->Measures;
  if (state == nullptr) {
    return;
  }
  const long long visualNow = nowMicros();
  const long long judgedTime = getJudgementTimeMicros(time);
  const long long poorCutoff = judgedTime - latePoorTiming;
  const bool replayPlayback = isReplayPlayback();
  for (size_t i = state->passedMeasureCount; i < measures.size(); i++) {
    const bool isFirstMeasure = i == state->passedMeasureCount;
    const auto &measure = measures[i];
    for (size_t j = isFirstMeasure ? state->passedTimelineCount : 0;
         j < measure->TimeLines.size(); j++) {
      const auto &timeline = measure->TimeLines[j];
      if (timeline->Timing <= judgedTime) {
        applyTimelineBpm(timeline);
      }
      if (timeline->Timing < poorCutoff) {
        if (isFirstMeasure) {
          state->passedTimelineCount++;
        }
        if (replayPlayback) {
          for (const auto &note : timeline->Notes) {
            if (note != nullptr && note->IsLandmineNote()) {
              expireGimmickNote(note, judgedTime);
            }
          }
          for (const auto &note : timeline->LandmineNotes) {
            expireGimmickNote(note, judgedTime);
          }
          continue;
        }
        // make remaining notes POOR
        for (const auto &note : timeline->Notes) {
          if (note == nullptr) {
            continue;
          }
          if (note->IsPlayed) {
            continue;
          }
          if (note->IsLandmineNote()) {
            expireGimmickNote(note, judgedTime);
            continue;
          }
          if (note->IsLongNote()) {
            const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            if (effectiveLongNoteIsCharge(longNote, chart,
                                          options.longNoteMode)) {
              const auto poorResult =
                  JudgeResult(Poor, judgedTime - timeline->Timing);
              if (!longNote->IsTail()) {
                markLongNoteMissed(longNote, judgedTime);
                onJudge(poorResult, false);
                appendReplayEvent(ReplayEventAction::Miss, note->Lane, note,
                                  time, judgedTime, poorResult);
                if (longNote->Tail != nullptr && !longNote->Tail->IsPlayed) {
                  markLongNoteMissed(
                      longNote->Tail, judgedTime,
                      !longNoteTailJudgedBeforeTiming(longNote->Tail,
                                                      judgedTime));
                  onJudge(poorResult, false);
                  appendReplayEvent(ReplayEventAction::Miss,
                                    longNote->Tail->Lane, longNote->Tail, time,
                                    judgedTime, poorResult);
                }
                continue;
              }

              markLongNoteMissed(longNote, judgedTime);
              if (longNote->Head != nullptr) {
                longNote->Head->IsHolding = false;
              }
              onJudge(poorResult, false);
              appendReplayEvent(ReplayEventAction::Miss, note->Lane, note, time,
                                judgedTime, poorResult);
              continue;
            } else if (!longNote->IsTail()) {
              markLongNoteMissed(longNote, judgedTime);
              if (longNote->Tail != nullptr && !longNote->Tail->IsPlayed) {
                markLongNoteMissed(longNote->Tail, judgedTime, false);
              }
            }
          }
          const auto poorResult =
              JudgeResult(Poor, judgedTime - timeline->Timing);
          onJudge(poorResult, false);
          appendReplayEvent(ReplayEventAction::Miss, note->Lane, note, time,
                            judgedTime, poorResult);
        }
        for (const auto &note : timeline->LandmineNotes) {
          if (note == nullptr || note->IsDead) {
            continue;
          }
          expireGimmickNote(note, judgedTime);
        }
      } else if (timeline->Timing <= judgedTime) {
        // auto-release long notes
        for (const auto &note : timeline->Notes) {
          if (note == nullptr) {
            continue;
          }
          if (note->IsPlayed) {
            continue;
          }
          if (note->IsLandmineNote()) {
            auto *landmine = static_cast<bms_parser::LandmineNote *>(note);
            if (!replayPlayback && laneIsPressed(lanePressed, note->Lane)) {
              detonateLandmine(landmine, time, judgedTime);
            } else {
              expireGimmickNote(landmine, judgedTime);
            }
            continue;
          }
          if (note->IsLongNote()) {
            const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            if (longNote->IsTail()) {
              if (!longNote->IsHolding) {
                continue;
              }
              const bool chargeLongNote = effectiveLongNoteIsCharge(
                  longNote, chart, options.longNoteMode);
              if (chargeLongNote && !options.autoPlay) {
                continue;
              }
              longNote->Release(judgedTime);
              const auto judgeResult =
                  chargeLongNote
                      ? normalizeLongNoteReleaseJudge(
                            judge.judgeNow(longNote, judgedTime))
                      : judgeClassicLongNoteRelease(judge, longNote,
                                                    judgedTime);
              onJudge(judgeResult, false);
              appendReplayEvent(ReplayEventAction::Release, note->Lane, note,
                                time, judgedTime, judgeResult);
              if (options.autoPlay) {
                renderer->onLaneReleased(note->Lane, visualNow);
              }
              continue;
            }
          }
          if (replayPlayback) {
            continue;
          }
          if (options.autoPlay) // NormalNote or LongNote's head
          {
            const JudgeResult judgeResult =
                pressNote(note, judgedTime, nullptr, time);
            renderer->onLanePressed(note->Lane, judgeResult, visualNow);
            if (!note->IsLongNote()) {
              renderer->onLaneReleased(note->Lane, visualNow);
            }
          }
        }
        for (const auto &note : timeline->LandmineNotes) {
          if (note == nullptr || note->IsDead) {
            continue;
          }
          if (!replayPlayback && laneIsPressed(lanePressed, note->Lane)) {
            detonateLandmine(note, time, judgedTime);
          } else {
            expireGimmickNote(note, judgedTime);
          }
        }
      } else {
        return;
      }
    }
    if (state->passedTimelineCount == measure->TimeLines.size() &&
        isFirstMeasure) {
      state->passedMeasureCount++;
      state->passedTimelineCount = 0;
    }
  }
}

void GamePlayScene::buildReplayNoteLookup() {
  replayNoteLookup.clear();
  if (!isReplayPlayback()) {
    return;
  }

  for (const auto &measure : chart->Measures) {
    for (const auto &timeline : measure->TimeLines) {
      for (const auto &note : timeline->Notes) {
        if (note == nullptr) {
          continue;
        }
        replayNoteLookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
      for (const auto &note : timeline->LandmineNotes) {
        if (note == nullptr) {
          continue;
        }
        replayNoteLookup[replayNoteKey(note->Lane, timeline->Timing)] = note;
      }
    }
  }
}

bms_parser::Note *
GamePlayScene::findReplayNote(const ReplayEvent &event) const {
  if (event.noteTimeMicros < 0) {
    return nullptr;
  }
  const auto it =
      replayNoteLookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == replayNoteLookup.end() ? nullptr : it->second;
}

void GamePlayScene::processReplayKeySounds(long long rawSongTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr ||
      options.autoKeySound) {
    return;
  }

  const auto &events = options.replayData->events;
  while (replayKeySoundCursor < events.size()) {
    const auto &event = events[replayKeySoundCursor];
    if (event.songTimeMicros > rawSongTimeMicros) {
      break;
    }

    if (event.action == ReplayEventAction::Press) {
      if (auto *note = findReplayNote(event);
          note != nullptr && note->Wav != bms_parser::Parser::NoWav) {
        context.jukebox.playKeySound(note->Wav);
      }
    }
    replayKeySoundCursor++;
  }
}

void GamePlayScene::processReplayEvents(long long gameplayTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr) {
    return;
  }

  const auto &events = options.replayData->events;
  const long long visualNow = nowMicros();
  while (replayEventCursor < events.size() &&
         events[replayEventCursor].songTimeMicros <= gameplayTimeMicros) {
    applyReplayEvent(events[replayEventCursor], visualNow);
    replayEventCursor++;
  }
}

void GamePlayScene::processReplayLaneCoverEvents(long long gameplayTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr ||
      renderer == nullptr) {
    return;
  }

  const auto &events = options.replayData->laneCoverEvents;
  while (replayLaneCoverCursor < events.size() &&
         events[replayLaneCoverCursor].songTimeMicros <= gameplayTimeMicros) {
    applyReplayLaneCoverEvent(events[replayLaneCoverCursor]);
    replayLaneCoverCursor++;
  }
}

void GamePlayScene::applyReplayLaneCoverEvent(
    const ReplayLaneCoverEvent &event) {
  if (renderer == nullptr) {
    return;
  }
  renderer->applyLaneCoverState(event.noteStartPositionPercent,
                                event.resetVisibleTimeReference);
}

void GamePlayScene::applyReplayEvent(const ReplayEvent &event,
                                     long long visualTimeMicros) {
  if (state == nullptr || !state->isPlaying || state->isEnding) {
    return;
  }

  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  switch (event.action) {
  case ReplayEventAction::Press: {
    if (auto pressedIt = lanePressed.find(event.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    updateLaneStateText();

    if (auto *note = findReplayNote(event);
        note != nullptr && event.judgement != None) {
      pressNote(note, event.judgeTimeMicros, &recordedJudge,
                event.songTimeMicros, false);
      applyReplayGauge(event);
    }
    renderer->onLanePressed(event.lane, recordedJudge, visualTimeMicros);
    break;
  }
  case ReplayEventAction::Release: {
    if (auto pressedIt = lanePressed.find(event.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = false;
    }
    updateLaneStateText();
    renderer->onLaneReleased(event.lane, visualTimeMicros);

    if (auto *note = findReplayNote(event);
        note != nullptr && event.judgement != None) {
      releaseNote(note, event.judgeTimeMicros, &recordedJudge,
                  event.songTimeMicros, false);
      applyReplayGauge(event);
    }
    break;
  }
  case ReplayEventAction::Miss:
    if (event.judgement != None) {
      markReplayMissedNote(findReplayNote(event), event.judgeTimeMicros);
      onJudge(recordedJudge, false);
      applyReplayGauge(event);
    }
    break;
  case ReplayEventAction::Mine:
    if (auto *note = findReplayNote(event); note != nullptr) {
      note->IsPlayed = true;
      expireGimmickNote(note, event.judgeTimeMicros);
    }
    applyReplayGauge(event);
    break;
  }
}

void GamePlayScene::applyReplayGauge(const ReplayEvent &event) {
  if (!isReplayPlayback() || state == nullptr) {
    return;
  }

  state->gaugeType = event.gaugeType;
  state->currentGauge = event.gauge;
  const int gaugeIndex = gaugeTypeIndex(event.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(state->gaugeValues.size())) {
    state->gaugeValues[gaugeIndex] = event.gauge;
  }
  if (!state->gaugeHistory.empty()) {
    state->gaugeHistory.back() = event.gauge;
  }
  updateGaugeStatusText();
}

void GamePlayScene::resetHellChargeGaugeTracking(long long gameplayTimeMicros) {
  hellChargeGaugeBalanceMicros.clear();
  lastHellChargeGaugeUpdateMicros = gameplayTimeMicros;
}

void GamePlayScene::updateHellChargeGauge(long long gameplayTimeMicros) {
  if (state == nullptr || chart == nullptr || isReplayPlayback()) {
    lastHellChargeGaugeUpdateMicros = gameplayTimeMicros;
    return;
  }

  const long long previousTime = lastHellChargeGaugeUpdateMicros;
  lastHellChargeGaugeUpdateMicros = gameplayTimeMicros;
  if (gameplayTimeMicros <= previousTime) {
    return;
  }

  bool gaugeUpdated = false;
  std::vector<bms_parser::LongNote *> activeHellChargeNotes;
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || !note->IsLongNote()) {
          continue;
        }
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        if (longNote->IsTail() || longNote->Tail == nullptr ||
            longNote->Timeline == nullptr || longNote->Tail->Timeline == nullptr ||
            !effectiveLongNoteIsHellCharge(longNote, chart,
                                           options.longNoteMode)) {
          continue;
        }

        const long long headTime = longNote->Timeline->Timing;
        const long long tailTime = longNote->Tail->Timeline->Timing;
        const bool tailJudgedBeforeTiming =
            longNote->Tail->IsPlayed && longNote->Tail->PlayedTime < tailTime;
        if (tailTime <= headTime || gameplayTimeMicros <= headTime ||
            previousTime >= tailTime ||
            (longNote->Tail->IsDead && !tailJudgedBeforeTiming)) {
          continue;
        }

        const long long activeStart = std::max(previousTime, headTime);
        const long long activeEnd = std::min(gameplayTimeMicros, tailTime);
        const long long activeDelta = activeEnd - activeStart;
        if (activeDelta <= 0) {
          continue;
        }

        activeHellChargeNotes.push_back(longNote);
        long long &balance = hellChargeGaugeBalanceMicros[longNote];
        const bool gaining = longNote->IsHolding ||
                             laneIsPressed(lanePressed, longNote->Lane) ||
                             options.autoPlay;
        balance += gaining ? activeDelta : -activeDelta;
        while (balance > kHellChargeGaugeTickMicros) {
          state->applyGaugeJudgementRate(Great, 0.5f);
          balance -= kHellChargeGaugeTickMicros;
          gaugeUpdated = true;
        }
        while (balance < -kHellChargeGaugeTickMicros) {
          state->applyGaugeJudgementRate(Bad, 0.5f);
          balance += kHellChargeGaugeTickMicros;
          gaugeUpdated = true;
        }
      }
    }
  }

  for (auto it = hellChargeGaugeBalanceMicros.begin();
       it != hellChargeGaugeBalanceMicros.end();) {
    if (std::find(activeHellChargeNotes.begin(), activeHellChargeNotes.end(),
                  it->first) == activeHellChargeNotes.end()) {
      it = hellChargeGaugeBalanceMicros.erase(it);
    } else {
      ++it;
    }
  }

  if (gaugeUpdated) {
    updateGaugeStatusText();
  }
}

void GamePlayScene::detonateLandmine(bms_parser::LandmineNote *note,
                                     long long songTimeMicros,
                                     long long judgeTimeMicros) {
  if (note == nullptr || note->IsDead) {
    return;
  }

  note->IsPlayed = true;
  note->IsDead = true;
  note->PlayedTime = judgeTimeMicros;

  if (state != nullptr) {
    state->applyGaugeDelta(-note->Damage);
    updateGaugeStatusText();
  }
  appendReplayEvent(ReplayEventAction::Mine, note->Lane, note, songTimeMicros,
                    judgeTimeMicros, JudgeResult(None, 0));
}

void GamePlayScene::expireGimmickNote(bms_parser::Note *note,
                                      long long judgeTimeMicros) {
  if (note == nullptr || note->IsDead) {
    return;
  }

  note->IsDead = true;
  note->PlayedTime = judgeTimeMicros;
}

void GamePlayScene::onJudge(const JudgeResult &judgeResult,
                            bool recordTimingSample) {
  const int judgementCount = ++state->judgeCount[judgeResult.judgement];
  if (judgeResult.isComboBreak()) {
    state->combo = 0;
    state->comboBreak++;
  } else if (judgeResult.judgement != Kpoor) {
    state->combo++;
    if (state->combo > state->maxCombo) {
      state->maxCombo = state->combo;
    }
  }
  renderer->onJudge(judgeResult, state->combo, state->getScore(),
                    getVisualTimeMicros(
                        getGameplayTimeMicros(context.jukebox.getTimeMicros())),
                    recordTimingSample);
  renderer->setJudgementCounter(judgeResult.judgement, judgementCount,
                                state->comboBreak);
  // CurrentRhythmHUD->OnJudge(state);
  // UE_LOG(LogTemp, Warning, TEXT("Judge: %s, Combo: %d, Diff: %lld"),
  // *JudgeResult.ToString(), state->Combo, JudgeResult.Diff);

  state->recordFastSlow(judgeResult);

  state->applyGaugeJudgement(judgeResult.judgement);
  updateGaugeStatusText();
  updatePacemakerStatus();
}

void GamePlayScene::appendReplayEvent(ReplayEventAction action, int lane,
                                      const bms_parser::Note *note,
                                      long long songTimeMicros,
                                      long long judgeTimeMicros,
                                      const JudgeResult &judgeResult) {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  ReplayEvent event;
  event.action = action;
  event.lane = lane;
  event.noteTimeMicros = note != nullptr && note->Timeline != nullptr
                             ? note->Timeline->Timing
                             : -1;
  event.songTimeMicros = songTimeMicros;
  event.judgeTimeMicros = judgeTimeMicros;
  event.judgement = judgeResult.judgement;
  event.diffMicros = judgeResult.Diff;
  event.gauge = state->currentGauge;
  event.gaugeType = state->gaugeType;
  event.combo = state->combo;
  event.score = state->getScore();
  recordedReplay.events.push_back(event);
}

void GamePlayScene::appendReplayLaneCoverEvent(int noteStartPositionPercent,
                                               long long songTimeMicros,
                                               bool resetVisibleTimeReference) {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  ReplayLaneCoverEvent event;
  event.songTimeMicros = std::max(0LL, songTimeMicros);
  event.noteStartPositionPercent =
      std::clamp(noteStartPositionPercent,
                 AppSettings::kMinNoteStartPositionPercent,
                 AppSettings::kMaxNoteStartPositionPercent);
  event.resetVisibleTimeReference = resetVisibleTimeReference;
  recordedReplay.laneCoverEvents.push_back(event);
}

bool GamePlayScene::handleTouchInput(SDL_FingerID fingerIndex,
                                     ReplayTouchAction action,
                                     Vector3 normalizedLocation) {
  const bool activeFloatingDrag =
      floatingLaneCoverDragActive && fingerIndex == floatingLaneCoverFinger;
  if (state == nullptr || !state->isPlaying || state->isEnding ||
      context.jukebox.isPaused()) {
    if (activeFloatingDrag &&
        (action == ReplayTouchAction::Up ||
         action == ReplayTouchAction::Cancel)) {
      floatingLaneCoverDragActive = false;
      floatingLaneCoverDragChanged = false;
      floatingLaneCoverFinger = -1;
      floatingLaneCoverDragOffsetY = 0.0f;
      persistFloatingLaneCoverSettings();
    }
    return activeFloatingDrag;
  }

  const long long gameplayTimeMicros =
      getGameplayTimeMicros(context.jukebox.getTimeMicros());
  if (renderer != nullptr && touchVisualizerLoaded) {
    renderer->setLiveTouchPoint(static_cast<long long>(fingerIndex), action,
                                normalizedLocation.x, normalizedLocation.y,
                                gameplayTimeMicros);
  }
  appendReplayTouchSample(fingerIndex, action, normalizedLocation,
                          gameplayTimeMicros);
  return handleFloatingLaneCoverInput(fingerIndex, action, normalizedLocation,
                                      gameplayTimeMicros);
}

bool GamePlayScene::handleFloatingLaneCoverInput(
    SDL_FingerID fingerIndex, ReplayTouchAction action,
    Vector3 normalizedLocation, long long songTimeMicros) {
  if (renderer == nullptr || !context.settings.floatingLaneCoverEnabled) {
    return false;
  }
  if (courseNoSpeed()) {
    return false;
  }

  const float renderX = normalizedLocation.x * rendering::render_width;
  const float renderY = normalizedLocation.y * rendering::render_height;
  const bool activeFinger =
      floatingLaneCoverDragActive && fingerIndex == floatingLaneCoverFinger;

  auto applyDrag = [&]() -> bool {
    const int previous = context.settings.noteStartPositionPercent;
    const int next = renderer->dragLaneCoverHandleTo(
        renderX, renderY, floatingLaneCoverDragOffsetY);
    context.settings.noteStartPositionPercent = next;
    if (next == previous) {
      return false;
    }
    renderer->applyLaneCoverState(next, true);
    floatingLaneCoverDragChanged = true;
    floatingLaneCoverSettingsDirty = true;
    appendReplayLaneCoverEvent(next, songTimeMicros, true);
    return true;
  };

  switch (action) {
  case ReplayTouchAction::Down:
    if (const auto grabOffset =
            renderer->laneCoverHandleGrabOffset(renderX, renderY);
        grabOffset.has_value()) {
      floatingLaneCoverDragActive = true;
      floatingLaneCoverDragChanged = false;
      floatingLaneCoverFinger = fingerIndex;
      floatingLaneCoverDragOffsetY = *grabOffset;
      return true;
    }
    return false;
  case ReplayTouchAction::Move:
    if (!activeFinger) {
      return false;
    }
    (void)applyDrag();
    return true;
  case ReplayTouchAction::Up:
  case ReplayTouchAction::Cancel:
    if (!activeFinger) {
      return false;
    }
    if (action == ReplayTouchAction::Up && floatingLaneCoverDragChanged) {
      (void)applyDrag();
    }
    floatingLaneCoverDragActive = false;
    floatingLaneCoverDragChanged = false;
    floatingLaneCoverFinger = -1;
    floatingLaneCoverDragOffsetY = 0.0f;
    persistFloatingLaneCoverSettings();
    return true;
  }

  return false;
}

void GamePlayScene::persistFloatingLaneCoverSettings() {
  if (courseNoSpeed()) {
    floatingLaneCoverSettingsDirty = false;
    return;
  }
  if (!floatingLaneCoverSettingsDirty) {
    return;
  }
  floatingLaneCoverSettingsDirty = false;
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save floating lane cover settings");
  }
}

void GamePlayScene::appendReplayTouchSample(SDL_FingerID fingerIndex,
                                            ReplayTouchAction action,
                                            Vector3 normalizedLocation,
                                            long long songTimeMicros) {
  if (!shouldRecordReplay() || state == nullptr || !state->isPlaying ||
      state->isEnding || context.jukebox.isPaused()) {
    return;
  }

  ReplayTouchSample sample;
  sample.action = action;
  sample.fingerId = static_cast<long long>(fingerIndex);
  sample.songTimeMicros = songTimeMicros;
  sample.x = std::clamp(normalizedLocation.x, 0.0f, 1.0f);
  sample.y = std::clamp(normalizedLocation.y, 0.0f, 1.0f);

  const auto lastIt = lastRecordedTouchSamples.find(sample.fingerId);
  if (action == ReplayTouchAction::Move &&
      lastIt != lastRecordedTouchSamples.end()) {
    const ReplayTouchSample &last = lastIt->second;
    const long long deltaMicros = sample.songTimeMicros - last.songTimeMicros;
    const float dx = sample.x - last.x;
    const float dy = sample.y - last.y;
    if (deltaMicros >= 0 &&
        deltaMicros < kReplayTouchMoveMinIntervalMicros &&
        dx * dx + dy * dy <
            kReplayTouchMoveMinDistance * kReplayTouchMoveMinDistance) {
      return;
    }
  }

  recordedReplay.touchSamples.push_back(sample);
  if (action == ReplayTouchAction::Up ||
      action == ReplayTouchAction::Cancel) {
    lastRecordedTouchSamples.erase(sample.fingerId);
  } else {
    lastRecordedTouchSamples[sample.fingerId] = sample;
  }
}

JudgeResult GamePlayScene::pressNote(bms_parser::Note *note,
                                     long long pressedTime,
                                     const JudgeResult *precomputedJudge,
                                     long long songTimeMicros,
                                     bool recordEvent) {
  if (note->Wav != bms_parser::Parser::NoWav && !options.autoKeySound &&
      !isReplayPlayback()) {
    context.jukebox.playKeySound(note->Wav);
  }
  const JudgeResult judgeResult = precomputedJudge != nullptr
                                      ? *precomputedJudge
                                      : judge.judgeNow(note, pressedTime);
  if (judgeResult.judgement != None) {
    if (judgeResult.isNotePlayed()) {
      // TODO: play keybomb
      if (note->IsLongNote()) {
        if (const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            !longNote->IsTail()) {
          longNote->Press(pressedTime);
          const bool chargeLongNote = effectiveLongNoteIsCharge(
              longNote, chart, options.longNoteMode);
          if (chargeLongNote) {
            onJudge(judgeResult, !options.autoPlay || isReplayPlayback());
          }
          if (recordEvent) {
            appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                              songTimeMicros >= 0 ? songTimeMicros
                                                  : pressedTime,
                              pressedTime, judgeResult);
          }
        }
        return judgeResult;
      }
      note->Press(pressedTime);
    }
    onJudge(judgeResult, !options.autoPlay || isReplayPlayback());
    if (recordEvent) {
      appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                        songTimeMicros >= 0 ? songTimeMicros : pressedTime,
                        pressedTime, judgeResult);
    }
  }
  return judgeResult;
}

JudgeResult GamePlayScene::releaseNote(bms_parser::Note *Note,
                                       long long ReleasedTime,
                                       const JudgeResult *precomputedJudge,
                                       long long songTimeMicros,
                                       bool recordEvent) {
  if (!Note->IsLongNote()) {
    return JudgeResult(None, 0);
  }
  const auto &LongNote = static_cast<bms_parser::LongNote *>(Note);
  if (!LongNote->IsTail()) {
    return JudgeResult(None, 0);
  }
  if (!LongNote->IsHolding) {
    return JudgeResult(None, 0);
  }
  LongNote->Release(ReleasedTime);
  const auto judgeResult = precomputedJudge != nullptr
                               ? *precomputedJudge
                               : judge.judgeNow(LongNote, ReleasedTime);
  JudgeResult appliedJudge(None, 0);
  const bool chargeLongNote =
      effectiveLongNoteIsCharge(LongNote, chart, options.longNoteMode);
  if (precomputedJudge != nullptr) {
    appliedJudge = *precomputedJudge;
  } else {
    appliedJudge = chargeLongNote
                       ? normalizeLongNoteReleaseJudge(judgeResult)
                       : judgeClassicLongNoteRelease(judge, LongNote,
                                                     ReleasedTime);
  }
  onJudge(appliedJudge, !options.autoPlay || isReplayPlayback());
  if (recordEvent) {
    appendReplayEvent(ReplayEventAction::Release, Note->Lane, Note,
                      songTimeMicros >= 0 ? songTimeMicros : ReleasedTime,
                      ReleasedTime, appliedJudge);
  }
  return appliedJudge;
}

EventHandleResult GamePlayScene::handleEvents(SDL_Event &event) {
  if (handleCoursePauseButtonEvent(event)) {
    return {};
  }

  Scene::handleEvents(event);
  if (event.type == SDL_KEYDOWN) {
    if (event.key.keysym.sym == SDLK_ESCAPE) {
      if (isCoursePlayback()) {
        if (pauseLayout != nullptr && pauseLayout->getVisible()) {
          closePauseMenu();
        } else {
          showPauseMenu(false);
        }
      } else if (context.jukebox.isPaused()) {
        closePauseMenu();
      } else {
        showPauseMenu(true);
      }
    }
  }
  return {};
}
void GamePlayScene::updateLaneStateText() {
  if (laneStateText == nullptr) {
    return;
  }
  std::string str;
  for (auto &[lane, pressed] : lanePressed) {
    str += std::to_string(pressed) + "\n";
  }
  laneStateText->setText(str);
}

void GamePlayScene::updateGaugeStatusText() {
  if (renderer == nullptr || state == nullptr) {
    return;
  }

  renderer->setGaugeStatus(state->gaugeType, state->gaugeAutoShift,
                           state->currentGauge, state->gaugeProfile);
}
