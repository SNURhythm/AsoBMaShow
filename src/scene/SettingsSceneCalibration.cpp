#include "SettingsSceneShared.h"
#include "../view/ScrollView.h"
#include "../view/DropdownView.h"
#include "../view/CheckboxButtonContent.h"
#include <numbers>

using namespace settings_scene;

namespace {
const path_t kCalibrationKeysound = PATH("@offset_calibration_keysound");
std::vector<short> calibrationKeysoundPcm() {
  constexpr int sampleRate = 48000;
  std::vector<short> pcm(sampleRate / 16 * 2);
  for (std::size_t frame = 0; frame < pcm.size() / 2; ++frame) {
    const double time = static_cast<double>(frame) / sampleRate;
    const double envelope = std::min(time / 0.001, 1.0) * std::exp(-time * 100);
    const auto value = static_cast<short>(10000 * envelope *
        (std::sin(2 * std::numbers::pi * 440 * time) +
         0.3 * std::sin(2 * std::numbers::pi * 880 * time)));
    pcm[frame * 2] = pcm[frame * 2 + 1] = value;
  }
  return pcm;
}
} // namespace

void SettingsScene::startCalibration() {
  calibrationAudioOffsetMs = context.settings.audioOffsetMs;
  calibrationVisualOffsetMs = context.settings.visualOffsetMs;
  calibrationSavedAutoPlay = previewAutoPlay;
  calibrationSavedRandomTiming = previewRandomTiming;
  calibrationSavedRate = context.jukebox.playbackRate();
  calibrationSamples.configure(calibrationChartOptions.bpm, calibrationChartOptions.beatsPerMeasure);
  calibrationError.clear();
  calibrationDisplay = false;
  calibrationMeasuring = false;
  calibrationActive = true;
  previewAutoPlay = false;
  previewRandomTiming = false;
  previewPanelFolded = false;
  startLanePreview();
  activeTab = SettingsTab::Timing;
}

void SettingsScene::finishCalibration(bool apply) {
  if (!calibrationActive) return;
  stopCalibrationAudio();
  destroyPreviewRenderer();
  calibrationActive = false;
  calibrationMeasuring = false;
  previewActive = false;
  previewAutoPlay = calibrationSavedAutoPlay;
  previewRandomTiming = calibrationSavedRandomTiming;
  previewPaused = false;
  std::string error;
  if (!context.jukebox.setPlaybackRate(calibrationSavedRate, error))
    SDL_Log("Unable to restore playback rate after calibration: %s", error.c_str());
  if (apply) {
    context.settings.audioOffsetMs = calibrationAudioOffsetMs;
    context.settings.visualOffsetMs = calibrationVisualOffsetMs;
    persistSettings();
  }
  activeTab = SettingsTab::Timing;
  lastLayoutWidth = -1;
}

void SettingsScene::startCalibrationAudio() {
  calibrationError.clear();
  calibrationSamples.beginLoop();
  const auto stopped = context.jukebox.stop();
  if (!stopped.success) {
    calibrationError = stopped.diagnostic;
  } else {
    std::string error;
    if (!context.jukebox.setPlaybackRate(audio::PlaybackRate{}, error)) {
      calibrationError = error;
    } else {
      // Use the same generated clicks, mixer, scheduling and clock as gameplay.
      prep_metronome::PrepMetronomePlan plan;
      plan.enabled = !calibrationMuted;
      const auto timing = calibrationTiming(calibrationChartOptions.bpm, calibrationChartOptions.beatsPerMeasure);
      int beat = 0;
      for (auto time = timing.firstClickMicros; time < timing.loopMicros; time += timing.beatMicros, ++beat)
        plan.clicks.push_back({time, beat % calibrationChartOptions.beatsPerMeasure == 0});
      if (!context.jukebox.loadGeneratedKeySound(
              kCalibrationKeysoundWav, kCalibrationKeysound, calibrationKeysoundPcm(), 2, 48000)) {
        calibrationError = "Unable to load calibration keysound";
        previewPaused = true;
        calibrationMeasuring = false;
        previewPanelFolded = false;
        lastLayoutWidth = -1;
        refreshCalibrationText();
        return;
      }
      std::atomic_bool cancelled{false};
      context.jukebox.schedule(*previewChart, !calibrationMuted && !context.settings.inputKeysoundEnabled, cancelled, std::nullopt, &plan);
      const auto started = context.jukebox.play();
      calibrationAudioRunning = started.success;
      if (!started.success) calibrationError = started.diagnostic;
    }
  }
  previewPaused = !calibrationError.empty();
  if (previewPaused) {
    calibrationMeasuring = false;
    previewPanelFolded = false;
    lastLayoutWidth = -1;
  }
  refreshCalibrationText();
}

void SettingsScene::stopCalibrationAudio() {
  if (!calibrationAudioRunning) return;
  const auto stopped = context.jukebox.stop();
  if (!stopped.success) {
    calibrationError = stopped.diagnostic;
    SDL_Log("Unable to stop calibration: %s", stopped.diagnostic.c_str());
  }
  calibrationAudioRunning = false;
}

void SettingsScene::updateCalibration() {
  if (!calibrationAudioRunning) return;
  const auto raw = context.jukebox.getTimeMicros();
  if (raw >= calibrationTiming(calibrationChartOptions.bpm, calibrationChartOptions.beatsPerMeasure).loopMicros) {
    // Loop the renderer without replacing controls or closing an open dropdown.
    const auto layoutWidth = lastLayoutWidth;
    resetPreviewSimulation();
    if (previewError.empty() && calibrationError.empty()) lastLayoutWidth = layoutWidth;
    return;
  }
  previewElapsedMicros = raw + calibrationAudioOffsetMs * 1000LL;
  // Display calibration leaves notes unjudged so they cross the line at their
  // visual time even when the trial offset is outside the judgement window.
  if (!calibrationDisplay) advancePreviewSimulation();
  refreshCalibrationText();
}

long long SettingsScene::previewInputTimeMicros(double inputDelay) const {
  if (!calibrationActive) return previewElapsedMicros;
  return context.jukebox.getTimeMicros() + calibrationAudioOffsetMs * 1000LL -
         static_cast<long long>(inputDelay * 1'000'000);
}

bms_parser::Note *SettingsScene::pressLaneAt(int lane, std::int64_t timestampMicros) {
  if (!calibrationActive) return pressLane(lane);
  const auto raw = context.jukebox.audioRuntime().songTimeMicrosAtSteadyMicros(
      previewEventSteadyMicros.value_or(timestampMicros));
  if (!raw) return nullptr;
  const auto delay = (context.jukebox.getTimeMicros() - *raw) / 1'000'000.0;
  return pressLane(lane, delay);
}

bms_parser::Note *SettingsScene::releaseLaneAt(int lane, std::int64_t timestampMicros,
                                              bool backSpin) {
  if (!calibrationActive) return releaseLane(lane, 0, backSpin);
  const auto raw = context.jukebox.audioRuntime().songTimeMicrosAtSteadyMicros(
      previewEventSteadyMicros.value_or(timestampMicros));
  const auto delay = raw ? (context.jukebox.getTimeMicros() - *raw) / 1'000'000.0 : 0;
  return releaseLane(lane, delay, backSpin);
}

void SettingsScene::playCalibrationKeysound() {
  if (calibrationAudioRunning && !previewPaused && !calibrationMuted && context.settings.inputKeysoundEnabled)
    context.jukebox.playKeySound(kCalibrationKeysoundWav);
}

void SettingsScene::recordCalibrationTap(long long rawTimeMicros) {
  if (!calibrationMeasuring || !calibrationAudioRunning || previewPaused ||
      (calibrationDisplay && (calibrationNotesHidden || calibrationChartOptions.subdivision != 4))) return;
  const bool accepted = calibrationDisplay
      ? calibrationSamples.tapDisplay(rawTimeMicros, calibrationAudioOffsetMs,
          calibrationVisualOffsetMs, context.settings.notesDisplayTimingMilliseconds)
      : calibrationSamples.tap(rawTimeMicros);
  if (!accepted) return;
  const auto estimate = calibrationDisplay
      ? calibrationSamples.displayEstimate(context.settings.notesDisplayTimingMilliseconds)
      : calibrationSamples.estimate();
  if (estimate) {
    if (calibrationDisplay) calibrationVisualOffsetMs = clampVisualOffset(estimate->offsetMs);
    else calibrationAudioOffsetMs = clampOffset(estimate->offsetMs);
    calibrationMeasuring = false;
    previewPanelFolded = false;
    lastLayoutWidth = -1;
    // Restart on the next frame, after the input callback has returned.
    previewRendererDirty = true;
  }
  refreshCalibrationText();
}

void SettingsScene::refreshCalibrationText() {
  if (!calibrationActive) return;
  if (calibrationValueText) calibrationValueText->setText(
      std::to_string(calibrationDisplay ? calibrationVisualOffsetMs : calibrationAudioOffsetMs) + " ms");
  if (calibrationSummaryText) calibrationSummaryText->setLocalizedText(i18n::message(
      "settings.calibration.values", {{"audio", std::to_string(calibrationAudioOffsetMs)},
                                      {"visual", std::to_string(calibrationVisualOffsetMs)}}));
  if (!calibrationStatusText) return;
  auto status = i18n::message("settings.calibration.progress",
      {{"count", std::to_string(calibrationSamples.count())}});
  if (!calibrationMeasuring) status = i18n::message("settings.calibration.ready");
  if (const auto estimate = calibrationSamples.estimate())
    status = i18n::message(calibrationDisplay ? "settings.calibration.display_result" : "settings.calibration.result",
        {{"offset", std::to_string(calibrationDisplay ? calibrationVisualOffsetMs : calibrationAudioOffsetMs)},
         {"spread", std::to_string(estimate->spreadMs)}});
  if (calibrationDisplay && calibrationChartOptions.subdivision != 4)
    status = i18n::message("settings.calibration.display_spacing");
  if (calibrationDisplay && calibrationNotesHidden)
    status = i18n::message("settings.calibration.display_hidden");
  if (previewPaused) status = i18n::message("settings.calibration.paused");
  if (!calibrationError.empty()) status = i18n::message("settings.calibration.error",
                                                       {{"error", calibrationError}});
  calibrationStatusText->setLocalizedText(std::move(status));
}

void SettingsScene::buildCalibrationPanel(View *panel, const LayoutMetrics &metrics, View *selectionRow) {
  const int controlHeight = std::max(44, metrics.actionButtonHeight * 2 / 3);
  const int stepWidth = metrics.compact ? 44 : 52;
  auto *scroll = new ScrollView();
  scroll->setFlexGrow(1);
  scroll->setFlexShrink(1);
  scroll->setMinHeight(0);
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column);
  body->setGap(12);
  const auto addButton = [&](View *parent, const i18n::Text &label,
                             std::function<void()> action, bool selected = false, bool primary = false) {
    auto *labelView = makeText(label, metrics.smallTextSize, app_theme::textPrimary(),
                               TextView::CENTER, TextView::MIDDLE);
    auto *button = makeControlButton(kFitContentWidth, controlHeight, labelView);
    button->setSelected(selected);
    if (selected || primary) {
      button->setThemedBackgroundColors(app_theme::action, app_theme::actionHover,
                                        app_theme::actionPressed);
      labelView->setThemedColor(app_theme::onAction);
    }
    button->setOnClickListener(std::move(action));
    parent->addView(button);
    return button;
  };
  const auto restart = [this] {
    calibrationMeasuring = false;
    calibrationSamples.configure(calibrationChartOptions.bpm, calibrationChartOptions.beatsPerMeasure);
    previewPaused = false;
    previewRendererDirty = true;
    lastLayoutWidth = -1;
  };
  const auto addDropdown = [&](View *row, const i18n::Text &label, std::string selected,
                               std::vector<DropdownView::Option> options,
                               std::function<void(const std::string &)> choose) {
    auto *dropdown = new DropdownView({.onOptionSelected = std::move(choose)}, overlayPortal);
    dropdown->setTriggerWidth(metrics.compact ? 160 : 210);
    dropdown->refresh({.label = label, .selectedId = std::move(selected),
                       .options = std::move(options), .maxVisibleItems = 8});
    row->addView(dropdown);
  };
  auto *modes = new View();
  modes->setFlexDirection(FlexDirection::Row);
  modes->setFlexWrap(YGWrapWrap);
  modes->setGap(8);
  for (const bool display : {false, true}) {
    auto *modeButton = addButton(modes, i18n::message(display ? "settings.calibration.display" : "settings.calibration.audio"),
        [this, display] {
          if (calibrationDisplay == display) return;
          calibrationDisplay = display;
          calibrationMeasuring = false;
          calibrationSamples.configure(calibrationChartOptions.bpm, calibrationChartOptions.beatsPerMeasure);
          previewPaused = false;
          resetPreviewSimulation();
          lastLayoutWidth = -1;
        }, calibrationDisplay == display);
    modeButton->setFlexGrow(1);
    modeButton->setFlexBasis(0);
  }
  body->addView(modes);
  body->addView(makeWrappedText(i18n::message(calibrationDisplay
      ? "settings.calibration.display_help" : "settings.calibration.audio_help",
      {{"beats", std::to_string(calibrationChartOptions.beatsPerMeasure)}}),
      metrics.smallTextSize, app_theme::textSecondary()));
  auto *adjustments = new View();
  adjustments->setFlexDirection(FlexDirection::Row);
  adjustments->setAlignItems(YGAlignCenter);
  adjustments->setGap(8);
  for (const int delta : {-10, -1, 1, 10}) {
    if (delta == 1) {
      calibrationValueText = makeText({}, metrics.bodyTextSize, app_theme::textPrimary(),
                                      TextView::CENTER, TextView::MIDDLE);
      calibrationValueText->setFlexGrow(1);
      calibrationValueText->setMinWidth(80);
      adjustments->addView(calibrationValueText);
    }
    auto *step = addButton(adjustments, i18n::Text((delta > 0 ? "+" : "") + std::to_string(delta)),
        [this, delta] {
          calibrationMeasuring = false;
          calibrationSamples.reset();
          if (calibrationDisplay) calibrationVisualOffsetMs = clampVisualOffset(calibrationVisualOffsetMs + delta);
          else {
            calibrationAudioOffsetMs = clampOffset(calibrationAudioOffsetMs + delta);
            previewRendererDirty = true;
          }
          refreshCalibrationText();
        });
    step->setWidth(stepWidth);
    step->setPadding(Edge::Left, 4);
    step->setPadding(Edge::Right, 4);
  }
  body->addView(adjustments);
  calibrationStatusText = makeWrappedText({}, metrics.smallTextSize, app_theme::textSecondary());
  body->addView(calibrationStatusText);
  auto *startButton = addButton(body, i18n::message("settings.calibration.start"), [this] {
    calibrationSamples.reset();
    calibrationMeasuring = true;
    previewPanelFolded = true;
    previewPaused = false;
    resetPreviewSimulation();
    if (!previewError.empty()) {
      calibrationMeasuring = false;
      previewPanelFolded = false;
    }
    lastLayoutWidth = -1;
  }, false, true);
  startButton->setEnabled(!calibrationDisplay ||
      (!calibrationNotesHidden && calibrationChartOptions.subdivision == 4));
  auto *previewHeading = makeText(i18n::message("settings.preview_layout.preview.label"),
                                  metrics.bodyTextSize, app_theme::textPrimary());
  previewHeading->setMargin(Edge::Top, 12);
  body->addView(previewHeading);
  body->addView(selectionRow);
  auto *chartControls = new View();
  chartControls->setFlexDirection(FlexDirection::Row);
  chartControls->setFlexWrap(YGWrapWrap);
  chartControls->setGap(8);
  std::vector<DropdownView::Option> patterns;
  for (const auto &[pattern, key] : std::array{
      std::pair{CalibrationPattern::SingleLane, "settings.calibration.single"},
      std::pair{CalibrationPattern::Stairs, "settings.calibration.stairs"},
      std::pair{CalibrationPattern::ReverseStairs, "settings.calibration.reverse_stairs"},
      std::pair{CalibrationPattern::Denim, "settings.calibration.denim"}})
    patterns.push_back({.id = std::to_string(static_cast<int>(pattern)), .label = i18n::message(key)});
  addDropdown(chartControls, i18n::message("settings.calibration.chart"),
      std::to_string(static_cast<int>(calibrationChartOptions.pattern)), std::move(patterns),
      [this, restart](const std::string &id) {
        calibrationChartOptions.pattern = static_cast<CalibrationPattern>(std::stoi(id));
        restart();
      });
  if (calibrationChartOptions.pattern == CalibrationPattern::SingleLane && previewChart) {
    const auto keys = previewChart->Meta.GetKeyLaneIndices();
    const auto scratches = previewChart->Meta.GetScratchLaneIndices();
    const auto lanes = previewKeyMode < 0 ? keys : previewChart->Meta.GetTotalLaneIndices();
    if (std::ranges::find(lanes, calibrationChartOptions.lane) == lanes.end())
      calibrationChartOptions.lane = keys[keys.size() / 2];
    std::vector<DropdownView::Option> options;
    for (const int lane : lanes) {
      const auto scratch = std::ranges::find(scratches, lane);
      const auto key = std::ranges::find(keys, lane);
      const auto label = scratch != scratches.end()
          ? (scratches.size() == 1 ? "S" : scratch == scratches.begin() ? "LS" : "RS")
          : std::to_string(key - keys.begin() + 1);
      options.push_back({.id = std::to_string(lane), .label = label});
    }
    addDropdown(chartControls, i18n::message("settings.preview_layout.lane.label"),
        std::to_string(calibrationChartOptions.lane), std::move(options),
        [this, restart](const std::string &id) { calibrationChartOptions.lane = std::stoi(id); restart(); });
  }
  body->addView(chartControls);
  auto *tempo = new View();
  tempo->setFlexDirection(FlexDirection::Row);
  tempo->setAlignItems(YGAlignCenter);
  tempo->setGap(8);
  for (const int delta : {-10, -1, 1, 10}) {
    if (delta == 1) {
      auto *value = makeText(std::to_string(calibrationChartOptions.bpm) + " BPM",
                             metrics.bodyTextSize, app_theme::textPrimary(),
                             TextView::CENTER, TextView::MIDDLE);
      value->setFlexGrow(1);
      value->setMinWidth(80);
      tempo->addView(value);
    }
    auto *step = addButton(tempo, i18n::Text((delta > 0 ? "+" : "") + std::to_string(delta)), [this, restart, delta] {
      calibrationChartOptions.bpm = std::clamp(calibrationChartOptions.bpm + delta, 40, 300);
      restart();
    });
    step->setWidth(stepWidth);
    step->setPadding(Edge::Left, 4);
    step->setPadding(Edge::Right, 4);
  }
  body->addView(tempo);
  auto *rhythm = new View();
  rhythm->setFlexDirection(FlexDirection::Row);
  rhythm->setFlexWrap(YGWrapWrap);
  rhythm->setGap(8);
  std::vector<DropdownView::Option> subdivisions;
  for (int denominator : {4, 8, 16, 24, 32})
    subdivisions.push_back({.id = std::to_string(denominator), .label = "1/" + std::to_string(denominator)});
  addDropdown(rhythm, i18n::message("settings.calibration.spacing"),
      std::to_string(calibrationChartOptions.subdivision), std::move(subdivisions),
      [this, restart](const std::string &id) { calibrationChartOptions.subdivision = std::stoi(id); restart(); });
  addDropdown(rhythm, i18n::message("settings.calibration.meter"),
      std::to_string(calibrationChartOptions.beatsPerMeasure), {{"4", "4/4"}, {"3", "3/4"}},
      [this, restart](const std::string &id) { calibrationChartOptions.beatsPerMeasure = std::stoi(id); restart(); });
  body->addView(rhythm);
  auto *sound = new View();
  sound->setFlexDirection(FlexDirection::Row);
  sound->setFlexWrap(YGWrapWrap);
  sound->setAlignItems(YGAlignFlexEnd);
  sound->setGap(8);
  addDropdown(sound, i18n::message("records.keysound.label"),
      context.settings.inputKeysoundEnabled ? "input" : "auto",
      {{"input", i18n::message("settings.controls.keysound.input_trigger.label")},
       {"auto", i18n::message("settings.controls.keysound.auto_timed.label")}},
      [this, restart](const std::string &id) {
        context.settings.inputKeysoundEnabled = id == "input";
        persistSettings();
        restart();
      });
  auto *aids = new View();
  aids->setFlexDirection(FlexDirection::Row);
  aids->setFlexWrap(YGWrapWrap);
  aids->setGap(8);
  const auto addToggle = [&](View *parent, const char *key, bool &enabled, bool restartAudio) {
    auto *content = new CheckboxButtonContent(i18n::tr(key), metrics.smallTextSize, 20);
    content->setThemedColor(app_theme::textPrimary);
    content->setChecked(enabled);
    auto *button = new Button();
    button->setHeight(controlHeight);
    button->setPadding(Edge::Left, 12);
    button->setPadding(Edge::Right, 12);
    button->setAlignItems(YGAlignCenter);
    button->setJustifyContent(YGJustifyCenter);
    button->setFlexShrink(0);
    button->setCornerRadius(app_theme::controlRadius());
    button->setThemedBackgroundColors(app_theme::control, app_theme::controlHover, app_theme::controlPressed);
    button->addView(content);
    button->setOnClickListener([this, value = &enabled, content, restartAudio] {
      *value = !*value;
      content->setChecked(*value);
      if (calibrationDisplay && value == &calibrationNotesHidden) {
        calibrationMeasuring = false;
        calibrationSamples.reset();
        lastLayoutWidth = -1;
      }
      // Start a fresh count-in with the changed sound schedule; taps survive.
      if (restartAudio) previewRendererDirty = true;
      ui::invalidate();
    });
    parent->addView(button);
  };
  addToggle(sound, "settings.calibration.mute", calibrationMuted, true);
  body->addView(sound);
  addToggle(aids, "settings.calibration.hide_notes", calibrationNotesHidden, false);
  addToggle(aids, "settings.calibration.logical_line", calibrationLogicalJudgementLine, false);
  body->addView(aids);
  scroll->setContentView(body);
  panel->addView(scroll);
  calibrationSummaryText = makeWrappedText({}, metrics.smallTextSize, app_theme::textSecondary());
  panel->addView(calibrationSummaryText);
  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row);
  actions->setJustifyContent(YGJustifySpaceBetween);
  actions->setGap(8);
  addButton(actions, i18n::message("settings.input.cancel.label"), [this] { finishCalibration(false); });
  addButton(actions, i18n::message("settings.audio.apply.label"), [this] { finishCalibration(true); }, false, true);
  panel->addView(actions);
}
