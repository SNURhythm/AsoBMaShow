//
// Created by XF on 8/25/2024.
//

#include "GamePlayScene.h"
#include "GamePlayStartup.h"
#include "GamePlayTiming.h"
#include "PracticeNoteFinalizer.h"
#include "../../GBattleMode.h"
#include "../../CourseConstraintUtils.h"
#include "../../PlayOptionUtils.h"
#include "../../PrepMetronome.h"
#include "../../repositories/ReplayRepository.h"
#include "../../replay/ChartReplayCapture.h"
#include "../../replay/ReplaySetupProvenance.h"
#include "../../ResultPresentationUtils.h"
#include "../../Uuid.h"
#include "../../practice/PracticeResultFlow.h"
#include "../../rendering/SimpleBatchRenderer.h"
#include "../../view/TextView.h"
#include "../../view/IconText.h"
#include "BuiltInPlayfieldPresentation.h"
#include "GameplayNoteJudgeRole.h"
#include "RealtimeGameplayAuthorityPolicy.h"
#include "RhythmLaneInputController.h"
#include "RealtimeGameplayInputBridge.h"
#include "RealtimeGameplayWorker.h"
#include "ReplayKeysoundSchedule.h"
#include "RealtimeTouchInputRouter.h"
#include "RealtimeTouchPresentation.h"
#include "VirtualControllerLayout.h"
#include "../../input/RhythmInputHandler.h"
#include "../../input/RealtimePhysicalInputRouter.h"
#include "../../input/InputTimestamp.h"
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include "../../input/AppleInputTimestamp.h"
#include "../../input/NativeCallbackLifetime.h"
#endif
#include "../../targets.h"
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include "PlayfieldPresentationCoordinator.h"
#include "../../skin/beatoraja/BgfxSkinTextureDevice.h"
#include "../../skin/beatoraja/PlaySkinSession.h"
#endif
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../../iOSNatives.hpp"
#endif
#include "../../view/Button.h"
#include "../../view/UiTheme.h"
#include "../../scene/MainMenuScene.h"
#include "../ResultScene.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include <SDL_uikit_rawtouch.h>
#include <dispatch/dispatch.h>
#endif

namespace {
constexpr uint32_t kIconPause = 0xf04c;
constexpr uint32_t kIconRestart = 0xf2f9;
constexpr long long kReplayTouchMoveMinIntervalMicros = 8000LL;
constexpr float kReplayTouchMoveMinDistance = 0.002f;
constexpr long long kHellChargeGaugeTickMicros = 200000LL;
constexpr long long kCoursePauseHoldMicros = 650000LL;
constexpr long long kCoursePauseRewindMicros = 260000LL;
constexpr float kPi = 3.14159265358979323846f;

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
skin::UiLogicalRect gameplaySkinSafeUiBounds() noexcept {
  double top = 0.0;
  double left = 0.0;
  double bottom = 0.0;
  double right = 0.0;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  if (std::isfinite(normalized.top) && std::isfinite(normalized.left) &&
      std::isfinite(normalized.bottom) && std::isfinite(normalized.right)) {
    top = std::clamp(static_cast<double>(normalized.top), 0.0, 1.0) *
          static_cast<double>(rendering::window_height);
    left = std::clamp(static_cast<double>(normalized.left), 0.0, 1.0) *
           static_cast<double>(rendering::window_width);
    bottom = std::clamp(static_cast<double>(normalized.bottom), 0.0, 1.0) *
             static_cast<double>(rendering::window_height);
    right = std::clamp(static_cast<double>(normalized.right), 0.0, 1.0) *
            static_cast<double>(rendering::window_width);
  }
#endif
  const double width = static_cast<double>(rendering::window_width) - left - right;
  const double height =
      static_cast<double>(rendering::window_height) - top - bottom;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
      height <= 0.0) {
    return {.x = 0.0,
            .y = 0.0,
            .width = static_cast<double>(rendering::window_width),
            .height = static_cast<double>(rendering::window_height)};
  }
  return {.x = left, .y = top, .width = width, .height = height};
}

void appendGameplaySkinDiagnostic(
    ApplicationContext &context, const skin::SkinEntryId &entry,
    std::string revisionDigest, std::string configurationDigest,
    skin::SkinDiagnosticPhase phase, skin::SkinDiagnostic diagnostic,
    std::optional<std::uint64_t> frameSerial = std::nullopt) noexcept {
  if (!context.skinDiagnosticHistory) {
    return;
  }
  try {
    const std::optional<std::uint32_t> luaLine =
        diagnostic.source && diagnostic.source->line != 0
            ? std::optional<std::uint32_t>(diagnostic.source->line)
            : std::nullopt;
    context.skinDiagnosticHistory->append({
        .entry = entry,
        .revisionDigest = std::move(revisionDigest),
        .configurationDigest = std::move(configurationDigest),
        .phase = phase,
        .diagnostic = std::move(diagnostic),
        .luaLine = luaLine,
        .frameSerial = frameSerial,
    });
  } catch (...) {
  }
}

std::string gameplaySkinFailureMessage(const skin::SkinDiagnostic &diagnostic) {
  std::string message = "The selected gameplay skin could not be used.";
  if (!diagnostic.message.empty()) {
    message.append("\n\n");
    message.append(diagnostic.message);
  }
  if (!diagnostic.code.empty()) {
    message.append("\n\n[");
    message.append(diagnostic.code);
    message.push_back(']');
  }
  return message;
}
#endif

const char *
resultPersistenceStateName(result_persistence::SaveState state) noexcept {
  switch (state) {
  case result_persistence::SaveState::Saved:
    return "Saved";
  case result_persistence::SaveState::InvalidAttempt:
    return "InvalidAttempt";
  case result_persistence::SaveState::Unstaged:
    return "Unstaged";
  case result_persistence::SaveState::PendingScore:
    return "PendingScore";
  case result_persistence::SaveState::PendingAcknowledgement:
    return "PendingAcknowledgement";
  case result_persistence::SaveState::UnstagedConflict:
    return "UnstagedConflict";
  case result_persistence::SaveState::PendingConflict:
    return "PendingConflict";
  }
  return "Unknown";
}

const char *chartReplayPersistenceStateName(
    replay::ChartReplayPersistenceState state) noexcept {
  switch (state) {
  case replay::ChartReplayPersistenceState::SavedWithReplay:
    return "SavedWithReplay";
  case replay::ChartReplayPersistenceState::SavedWithoutReplay:
    return "SavedWithoutReplay";
  case replay::ChartReplayPersistenceState::PendingScore:
    return "PendingScore";
  case replay::ChartReplayPersistenceState::PendingAcknowledgement:
    return "PendingAcknowledgement";
  case replay::ChartReplayPersistenceState::Retryable:
    return "Retryable";
  case replay::ChartReplayPersistenceState::InvalidAttempt:
    return "InvalidAttempt";
  case replay::ChartReplayPersistenceState::IntegrityConflict:
    return "IntegrityConflict";
  }
  return "Unknown";
}

replay::ReplayTouchAction modernTouchAction(ReplayTouchAction action) noexcept {
  switch (action) {
  case ReplayTouchAction::Down:
    return replay::ReplayTouchAction::Down;
  case ReplayTouchAction::Move:
    return replay::ReplayTouchAction::Move;
  case ReplayTouchAction::Up:
    return replay::ReplayTouchAction::Up;
  case ReplayTouchAction::Cancel:
    return replay::ReplayTouchAction::Cancel;
  }
  return replay::ReplayTouchAction::Cancel;
}
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR || TARGET_OS_ANDROID
constexpr auto kPlayStartInputPlatform = PlayStartInputPlatform::Mobile;
#else
constexpr auto kPlayStartInputPlatform = PlayStartInputPlatform::Desktop;
#endif

StartOptions resolvePlayStartInputDevices(StartOptions options,
                                          const InputProfile &profile,
                                          int keyMode) {
  if (options.practiceSession != nullptr) {
    const auto &configuration = options.practiceSession->configuration();
    applyPracticeConfigurationToStartOptions(options, configuration);
  }
  if (!options.inputDeviceCategories.empty()) {
    return options;
  }
  InputBindingResolver resolver(profile, makeGameplayInputScopes(keyMode), {});
  const auto activeDeviceClasses = resolver.activeDeviceClasses();
  const std::vector<input::DeviceClass> resolverDeviceClasses(
      activeDeviceClasses.begin(), activeDeviceClasses.end());
  options.inputDeviceCategories = collectPlayStartInputDeviceCategories(
      resolverDeviceClasses, kPlayStartInputPlatform);
  return options;
}

std::optional<NoteTimeRange>
practiceAllowedNoteRange(const StartOptions &options) {
  if (options.practiceSession == nullptr) {
    return std::nullopt;
  }
  const auto &configuration = options.practiceSession->configuration();
  return NoteTimeRange{
      .startMicros = configuration.startMicros,
      .endMicros = configuration.endMicros,
  };
}

constexpr std::int32_t kBeatorajaPlaytimeMarginMillis = 5'000;

std::int32_t javaLongToInt(std::int64_t value) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::int32_t javaIntAdd(std::int32_t left, std::int32_t right) noexcept {
  const auto bits = static_cast<std::uint32_t>(left) +
                    static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(bits);
}

std::optional<std::int32_t>
beatorajaPlaytimeMillis(const bms_parser::Chart *chart,
                        const StartOptions &options) noexcept {
  if (chart == nullptr) {
    return std::nullopt;
  }
  if (options.practiceSession != nullptr) {
    // Pinned practice also preloads TIMER_PLAY by a frequency-adjusted
    // starttimeoffset. Aso has only chart-time playback here, so its elapsed
    // origin cannot yet reproduce the paired upstream formula exactly.
    return std::nullopt;
  }
  if (options.practiceMode) {
    // A legacy practice launch lacks the upstream range/frequency authority.
    return std::nullopt;
  }
  const long long terminalMicros =
      options.autoPlay ? chart->Meta.TotalLength : chart->Meta.PlayLength;
  return javaIntAdd(javaLongToInt(terminalMicros / 1'000),
                    kBeatorajaPlaytimeMarginMillis);
}

long long nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t nowUnixMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string formatPracticeTime(long long micros) {
  const long long totalMillis = std::max(0LL, micros) / 1000LL;
  const long long minutes = totalMillis / 60000LL;
  const long long seconds = (totalMillis / 1000LL) % 60LL;
  const long long millis = totalMillis % 1000LL;
  std::ostringstream stream;
  stream << minutes << ':' << std::setfill('0') << std::setw(2) << seconds
         << '.' << std::setw(3) << millis;
  return stream.str();
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

bool laneIsPressed(const std::unordered_map<int, bool> &lanePressed, int lane) {
  const auto it = lanePressed.find(lane);
  return it != lanePressed.end() && it->second;
}

bool isInsideButton(const Button &button, float uiX, float uiY) {
  return uiX >= button.getX() && uiX <= button.getX() + button.getWidth() &&
         uiY >= button.getY() && uiY <= button.getY() + button.getHeight();
}

gameplay::RealtimeTouchNativeOverlayRegion
realtimeTouchOverlayRegion(const Button *button) noexcept {
  if (button == nullptr || !button->getVisible() || button->getWidth() <= 0 ||
      button->getHeight() <= 0) {
    return {};
  }
  const float left = static_cast<float>(button->getX());
  const float top = static_cast<float>(button->getY());
  return {.visible = true,
          .left = left,
          .top = top,
          .right = left + static_cast<float>(button->getWidth()),
          .bottom = top + static_cast<float>(button->getHeight())};
}

gameplay::RealtimeTouchUiTransform realtimeTouchUiTransform() noexcept {
  return {.renderWidth = rendering::render_width,
          .renderHeight = rendering::render_height,
          .uiScaleX = rendering::ui_scale_x,
          .uiScaleY = rendering::ui_scale_y,
          .uiOffsetX = rendering::ui_offset_x,
          .uiOffsetY = rendering::ui_offset_y,
          .uiWidth = rendering::window_width,
          .uiHeight = rendering::window_height};
}

std::uint64_t effectiveRealtimeTouchLayoutRevision(
    std::uint64_t presentationRevision,
    const input::VirtualControllerConfig &virtualController,
    int keyMode) noexcept {
  if (virtualController.enabled &&
      gameplay::supportsVirtualControllerKeyMode(keyMode)) {
    return presentationRevision == 0 ? 1 : presentationRevision;
  }
  return presentationRevision;
}

gameplay::VirtualControllerLayout currentVirtualControllerLayout(
    const input::VirtualControllerConfig &config, int keyMode,
    const gameplay::RealtimeTouchUiTransform &transform) {
  return gameplay::makeVirtualControllerLayout(
      config, keyMode,
      {.x = 0.0F,
       .y = 0.0F,
       .width = static_cast<float>(transform.uiWidth),
       .height = static_cast<float>(transform.uiHeight)});
}

void appendVirtualControllerHitRegions(
    std::vector<PresentationUiHitRegion> &regions,
    const gameplay::VirtualControllerLayout &layout,
    std::uint64_t layoutRevision) {
  if (!layout.valid()) {
    return;
  }
  regions.reserve(regions.size() + layout.elements.size());
  for (const auto &element : layout.elements) {
    const auto &bounds = element.bounds;
    std::optional<PresentationUiCircle> circle;
    if (element.shape == gameplay::VirtualControllerShape::Circle) {
      circle = PresentationUiCircle{
          .center = {.x = bounds.centerX(), .y = bounds.centerY()},
          .radius = std::min(bounds.width, bounds.height) * 0.5F};
    }
    regions.push_back(
        {.hit = {.kind = PresentationUiControlKind::VirtualController,
                 .layoutRevision = layoutRevision},
         .boundary = {{{bounds.x, bounds.y},
                       {bounds.x + bounds.width, bounds.y},
                       {bounds.x + bounds.width, bounds.y + bounds.height},
                       {bounds.x, bounds.y + bounds.height}}},
         .circle = circle});
  }
}

void renderVirtualControllerOverlay(const gameplay::VirtualControllerLayout &layout) {
  if (!layout.valid()) {
    return;
  }
  constexpr float kBorder = 3.0F;
  const uint32_t border = Color(177, 243, 255, 112).toABGR();
  const uint32_t fill = Color(28, 77, 90, 54).toABGR();
  rendering::SimpleBatchRenderer batch;
  batch.setSubmitView(rendering::ui_view);
  batch.begin();
  for (const auto &element : layout.elements) {
    const auto &bounds = element.bounds;
    if (element.shape == gameplay::VirtualControllerShape::Circle) {
      const float radius = std::min(bounds.width, bounds.height) * 0.5F;
      batch.addCircle(bounds.centerX(), bounds.centerY(), radius, border);
      batch.addCircle(bounds.centerX(), bounds.centerY(),
                      std::max(0.0F, radius - kBorder), fill);
      continue;
    }
    const float radius = std::min(bounds.height * 0.22F, 12.0F);
    batch.addRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height,
                         radius, border);
    batch.addRoundedRect(bounds.x + kBorder, bounds.y + kBorder,
                         std::max(0.0F, bounds.width - kBorder * 2.0F),
                         std::max(0.0F, bounds.height - kBorder * 2.0F),
                         std::max(0.0F, radius - kBorder), fill);
  }
  batch.end();
}

gameplay::RealtimeTouchLayoutRefreshKey makeRealtimeTouchLayoutRefreshKey(
    std::uint64_t layoutRevision, std::uint64_t hitRegionRevision,
    const Button *pauseButton, const Button *practiceRestartButton,
    const Button *skinResetLayoutButton) noexcept {
  return {.layoutRevision = layoutRevision,
          .hitRegionRevision = hitRegionRevision,
          .uiTransform = realtimeTouchUiTransform(),
          .nativeOverlays = {
              realtimeTouchOverlayRegion(pauseButton),
              realtimeTouchOverlayRegion(practiceRestartButton),
              realtimeTouchOverlayRegion(skinResetLayoutButton)}};
}

void mouseEventToUi(const SDL_MouseButtonEvent &event, float &uiX, float &uiY) {
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

JudgeResult judgeClassicLongNoteRelease(
    const gameplay::CompiledGameplayJudge &judge,
    const bms_parser::ChartMeta &chartMeta, int longNoteModeOverride,
    bms_parser::LongNote *tail, long long releasedTime) {
  if (tail == nullptr || !tail->IsTail() || tail->Head == nullptr) {
    return JudgeResult(None, 0);
  }

  const auto noteTiming = [](const bms_parser::Note *note) {
    return note != nullptr && note->Timeline != nullptr ? note->Timeline->Timing
               : 0LL;
  };
  const JudgeResult headJudge = judge.judgeAt(
      gameplay::judgeRoleFor(tail->Head, chartMeta, longNoteModeOverride),
      noteTiming(tail->Head), tail->Head->PlayedTime);
  const long long tailDiff = releasedTime - noteTiming(tail);
  if (judge.rules().ruleset == GameplayRuleset::LR2) {
    const JudgeResult head = normalizeLongNoteReleaseJudge(headJudge);
    if (std::llabs(tailDiff) <= 120'000) {
      return head;
    }
    return head.judgement == Bad && std::llabs(head.Diff) > std::llabs(tailDiff)
               ? head
               : JudgeResult(Bad, tailDiff);
  }
  const JudgeResult tailJudge = judge.judgeAt(
      gameplay::judgeRoleFor(tail, chartMeta, longNoteModeOverride),
      noteTiming(tail), releasedTime);
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

Judge presentationJudgeForPolicy(
    const gameplay::GameplayPolicyBuildOutcome &outcome, int sourceRank) {
  Judge result(sourceRank);
  if (!outcome.policy.has_value()) {
    return result;
  }
  result.timingWindows.clear();
  for (const Judgement judgement : {PGreat, Great, Good, Bad, Kpoor}) {
    const auto window = outcome.policy->judge.window(
        gameplay::JudgeWindowContext::Normal, judgement);
    if (window.has_value()) {
      result.timingWindows[judgement] = {window->earlyMicros,
                                         window->lateMicros};
    }
  }
  return result;
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

  applyEffectiveLongNoteModeToChart(*retryChart, retryOptions.longNoteMode);
  return true;
}

#if defined(DEBUG) || defined(_DEBUG)
constexpr bool kShowLaneStateOverlay = true;
#else
constexpr bool kShowLaneStateOverlay = false;
#endif

std::vector<bms_parser::Note *>
buildRealtimeGameplayNoteLookup(const bms_parser::Chart &chart) {
  std::vector<bms_parser::Note *> result;
  std::unordered_set<bms_parser::Note *> seen;
  const auto append = [&](bms_parser::Note *note) {
    if (note != nullptr && seen.insert(note).second) {
      result.push_back(note);
    }
  };
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        append(note);
      }
      for (auto *note : timeline->LandmineNotes) {
        append(note);
      }
    }
  }
  return result;
}

std::optional<gameplay::RealtimeTouchLayout>
buildRealtimeTouchLayout(const PlayfieldPresentation &presentation,
                         bool dragMode, const bms_parser::ChartMeta &chartMeta,
                         const input::VirtualControllerConfig &virtualController,
                         const gameplay::RealtimeTouchUiTransform &transform) {
  auto layout = presentation.touchLayout();
  layout.dragMode = dragMode;
  const auto controller = currentVirtualControllerLayout(
      virtualController, chartMeta.KeyMode, transform);
  auto controllerRegions =
      gameplay::makeVirtualControllerTouchRegions(controller, transform);
  if (!controllerRegions.empty()) {
    layout.laneRegions.insert(
        layout.laneRegions.begin(),
        std::make_move_iterator(controllerRegions.begin()),
        std::make_move_iterator(controllerRegions.end()));
    if (layout.revision == 0) {
      layout.revision = 1;
    }
  }
  if (layout.revision == 0 ||
      (layout.laneRegions.empty() &&
      (layout.laneCount == 0 || layout.lanes.size() < layout.laneCount ||
       layout.scratch.size() < layout.laneCount))) {
    return std::nullopt;
  }
  return layout;
}

ReplayEventAction
replayActionFromRealtime(gameplay::GameplayReplayAction action) {
  switch (action) {
  case gameplay::GameplayReplayAction::Release:
    return ReplayEventAction::Release;
  case gameplay::GameplayReplayAction::Miss:
    return ReplayEventAction::Miss;
  case gameplay::GameplayReplayAction::Mine:
    return ReplayEventAction::Mine;
  case gameplay::GameplayReplayAction::Gauge:
    return ReplayEventAction::Gauge;
  case gameplay::GameplayReplayAction::MultiBad:
    return ReplayEventAction::MultiBad;
  case gameplay::GameplayReplayAction::Press:
  default:
    return ReplayEventAction::Press;
  }
}
} // namespace

struct GamePlayScene::RealtimeGameplaySession {
  static constexpr std::size_t kAuxiliaryTouchCapacity = 4096;
  static constexpr std::size_t kInputCommandCapacity = 256;
  static constexpr std::size_t kStartSelectInputCapacity = 512;

  GamePlayScene *scene = nullptr;
  AudioWrapper *audio = nullptr;
  long long audioOffsetMicros = 0;
  std::uint64_t epoch = 0;
  std::atomic_bool acceptingTouch{false};
  std::atomic_bool acceptingNativeInput{false};
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  input::apple::HostToSteadyTimestampSession touchTimestampSession;
#endif
  InputDeviceRegistry *inputRegistry = nullptr;
  std::vector<std::optional<audio::RealtimeSoundHandle>> soundHandles;
  std::vector<bms_parser::Note *> notes;
  std::unique_ptr<gameplay::RealtimeGameplayWorker> worker;
  std::unique_ptr<gameplay::RealtimeGameplayInputBridge> legacyInputBridge;
  std::unique_ptr<gameplay::RealtimeTouchInputRouter> touchRouter;
  std::mutex touchRouterMutex;
  std::unique_ptr<input::RealtimePhysicalInputRouter> physicalInputRouter;
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::unique_ptr<NativeCallbackLifetime> touchCallbackLifetime;
#endif
  gameplay::BoundedMpscQueue<input::LogicalInputTransition,
                             kInputCommandCapacity>
      inputCommands;
  std::atomic_bool inputCommandOverflow{false};
  gameplay::BoundedMpscQueue<gameplay::StartSelectControlInput,
                             kStartSelectInputCapacity>
      startSelectInputs;
  std::atomic_bool startSelectInputOverflow{false};
  bool sdlInputWatchRegistered = false;
  std::uint64_t realtimeInputSubscription = 0;
  std::uint64_t realtimeDeviceSubscription = 0;
  std::array<bool, 6> registryRealtimeClasses{};
  std::array<bool, 6> claimedRealtimeClasses{};
  gameplay::BoundedMpscQueue<gameplay::RealtimeTouchSample,
                             kAuxiliaryTouchCapacity>
      auxiliaryTouches;
  std::atomic_bool auxiliaryTouchOverflow{false};
  std::atomic_bool touchRoutingRecoveryRequested{false};
  gameplay::RealtimeTouchPresentationDispatcher presentationTouches;
  gameplay::RealtimeTouchHitSnapshotPublication touchHitSnapshots;
  gameplay::RealtimeTouchHitCaptureTracker rawHitCaptures;
  std::atomic<std::uint64_t> requestedHitCaptureReset{0};
  std::uint64_t appliedRawHitCaptureReset = 0;
  std::uint64_t appliedSnapshotGeneration = 0;
  std::uint64_t appliedTransactionSequence = 0;
  std::size_t visualMeasureIndex = 0;
  std::size_t visualTimelineIndex = 0;
  gameplay::RealtimeTouchLayoutRefreshKey layoutRefreshKey;
  bool touchHitSnapshotDirty = true;
  bool touchIngressDesired = false;

  void enqueueStartSelectInput(
      const gameplay::RealtimeGameplayInput &input) noexcept {
    if (!input.hasReplayControl) {
      return;
    }
    const auto kind = input.replayControl.kind;
    const bool systemControl =
        kind == replay::LogicalControlKind::Start ||
        kind == replay::LogicalControlKind::Select;
    const bool gameplayControl =
        kind == replay::LogicalControlKind::Lane ||
        kind == replay::LogicalControlKind::ScratchClockwise ||
        kind == replay::LogicalControlKind::ScratchCounterClockwise;
    if (!systemControl && (!gameplayControl || input.replayOnly)) {
      return;
    }
    if (!startSelectInputs.tryPush(
            {.control = input.replayControl,
             .pressed = input.type == gameplay::RealtimeGameplayInputType::Press,
             .timestampMicros = input.steadyTimestampMicros})) {
      startSelectInputOverflow.store(true, std::memory_order_release);
    }
  }

  void populateImmutableHit(gameplay::RealtimeTouchSample &sample) {
    const auto requested =
        requestedHitCaptureReset.load(std::memory_order_acquire);
    if (appliedRawHitCaptureReset != requested) {
      rawHitCaptures.reset();
      appliedRawHitCaptureReset = requested;
    }
    const auto snapshot = touchHitSnapshots.acquire();
    static const gameplay::RealtimeTouchHitSnapshot emptySnapshot;
    const auto &published = snapshot ? *snapshot : emptySnapshot;
    gameplay::populateRealtimeTouchPresentationMetadata(
        sample, published, rawHitCaptures);
  }

  void requestHitCaptureReset() noexcept {
    requestedHitCaptureReset.fetch_add(1, std::memory_order_release);
  }

  [[nodiscard]] bool
  registryRealtimeEnabled(input::DeviceClass deviceClass) const noexcept {
    const auto index = static_cast<std::size_t>(deviceClass);
    return index < registryRealtimeClasses.size() &&
           registryRealtimeClasses[index];
  }

  static std::optional<std::int64_t>
  mapSteadyToSong(void *context, std::int64_t steadyMicros) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (session.audio == nullptr) {
      return std::nullopt;
    }
    const auto rawSongTime =
        session.audio->songTimeMicrosAtSteadyMicros(steadyMicros);
    if (!rawSongTime.has_value()) {
      return std::nullopt;
    }
    return *rawSongTime + session.audioOffsetMicros;
  }

  static std::optional<std::int64_t> currentSongTime(void *context) {
    return mapSteadyToSong(context, nowMicros());
  }

  static bool
  reserveAudio(void *context, gameplay::NoteId noteId,
               gameplay::RealtimeGameplayAudioReservation &reservation) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (noteId >= session.soundHandles.size()) {
      return false;
    }
    if (!session.soundHandles[noteId].has_value()) {
      reservation.value = 0;
      reservation.requiresCommit = false;
      return true;
    }
    const auto reserved = session.audio->tryReserveRealtimeSoundCommand();
    if (!reserved.has_value()) {
      return false;
    }
    reservation.value = reserved->cursor;
    reservation.requiresCommit = true;
    return true;
  }

  static bool
  commitAudio(void *context,
              gameplay::RealtimeGameplayAudioReservation reservation,
              gameplay::NoteId noteId) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (noteId >= session.soundHandles.size()) {
      return false;
    }
    const auto &handle = session.soundHandles[noteId];
    if (!handle.has_value()) {
      return true;
    }
    return session.audio->commitRealtimeKeysound(
        {.cursor = static_cast<std::uint32_t>(reservation.value)}, *handle);
  }

  static void
  cancelAudio(void *context,
              gameplay::RealtimeGameplayAudioReservation reservation,
              gameplay::NoteId noteId) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!reservation.requiresCommit || noteId >= session.soundHandles.size() ||
        !session.soundHandles[noteId].has_value()) {
      return;
    }
    session.audio->cancelRealtimeSoundCommand(
        {.cursor = static_cast<std::uint32_t>(reservation.value)});
  }

  static bool emitTouchInput(void *context,
                             const gameplay::RealtimeGameplayInput &input) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!gameplay::realtimeTouchRouterTransitionCanReachWorker(
            session.acceptingTouch.load(std::memory_order_acquire),
            session.worker != nullptr)) {
      return false;
    }
    auto owned = input;
    owned.source = gameplay::RealtimeGameplayInputSource::Touch;
    const bool accepted = session.worker->enqueueInput(owned);
    if (accepted) {
      session.enqueueStartSelectInput(owned);
    }
    return accepted;
  }

  static bool emitLegacyInput(void *context,
                              const gameplay::RealtimeGameplayInput &input) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    return session.worker != nullptr && session.worker->enqueueInput(input);
  }

  bool prepareLegacyInput(gameplay::RealtimeGameplayInputType type, int lane,
                          int compensateLane, bool backSpin,
                          std::int64_t inputDelayMicros) {
    return legacyInputBridge != nullptr &&
           legacyInputBridge->prepare(type, lane, compensateLane, backSpin,
                                      nowMicros(), inputDelayMicros);
  }

  bool emitLegacyApplied(int physicalLane, replay::LogicalControl control,
                         bool hasReplayControl, bool pressed,
                         bool replayOnly) {
    return legacyInputBridge != nullptr &&
           legacyInputBridge->emitApplied(physicalLane, control,
                                          hasReplayControl, pressed,
                                          replayOnly, nowMicros());
  }

  static bool scratchLongNoteHeld(void *context, int lane) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (session.worker == nullptr || lane < 0) {
      return false;
    }
    auto snapshot = session.worker->acquireLatestSnapshot();
    return snapshot && static_cast<std::size_t>(lane) <
                           snapshot->longNoteHoldingByLane.size() &&
           snapshot->longNoteHoldingByLane[static_cast<std::size_t>(lane)];
  }

  static bool cancelTouchLifecycle(
      void *context, const gameplay::RealtimeTouchSample &sample) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (session.auxiliaryTouches.tryPush(sample)) {
      return true;
    }
    session.auxiliaryTouchOverflow.store(true, std::memory_order_release);
    return false;
  }

  static PresentationTouchResult beginPresentationTouch(
      void *context, const PresentationTouchEvent &event) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    return session.scene != nullptr && session.scene->presentation != nullptr
               ? session.scene->presentation->beginPresentationTouch(event)
               : PresentationTouchResult{};
  }

  static PresentationTouchResult updatePresentationTouch(
      void *context, const PresentationTouchEvent &event) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    return session.scene != nullptr && session.scene->presentation != nullptr
               ? session.scene->presentation->updatePresentationTouch(event)
               : PresentationTouchResult{};
  }

  static PresentationTouchResult endPresentationTouch(
      void *context, const PresentationTouchEvent &event, bool cancelled) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    return session.scene != nullptr && session.scene->presentation != nullptr
               ? session.scene->presentation->endPresentationTouch(event,
                                                                    cancelled)
               : PresentationTouchResult{};
  }

  static void cancelPresentationTouches(void *context,
                                        long long eventMicros) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (session.scene != nullptr && session.scene->presentation != nullptr) {
      session.scene->presentation->cancelPresentationTouches(eventMicros);
    }
  }

  static bool
  emitPhysicalInput(void *context,
                    const input::RealtimePhysicalInputTransition &transition) {
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (transition.type ==
        input::RealtimePhysicalInputTransitionType::Command) {
      if (!session.inputCommands.tryPush(transition.command)) {
        session.inputCommandOverflow.store(true, std::memory_order_release);
        return false;
      }
      return true;
    }
    if (session.worker == nullptr) {
      return false;
    }
    const gameplay::RealtimeGameplayInput input{
        .epoch = session.epoch,
         .type = transition.type ==
                         input::RealtimePhysicalInputTransitionType::Press
                     ? gameplay::RealtimeGameplayInputType::Press
                     : gameplay::RealtimeGameplayInputType::Release,
         .source = gameplay::RealtimeGameplayInputSource::Physical,
         .lane = transition.lane,
         .compensateLane = transition.lane,
         .backSpin = transition.backSpin,
         .steadyTimestampMicros = transition.steadyTimestampMicros,
         .hasReplayControl = transition.hasReplayControl,
        .replayControl = transition.replayControl,
        .replayOnly = transition.replayOnly};
    const bool accepted = session.worker->enqueueInput(input);
    if (accepted) {
      session.enqueueStartSelectInput(input);
    }
    return accepted;
  }

  static int SDLCALL sdlInputWatch(void *context, SDL_Event *event) {
    if (context == nullptr || event == nullptr) {
      return 0;
    }
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!session.acceptingNativeInput.load(std::memory_order_acquire) ||
        session.inputRegistry == nullptr ||
        session.physicalInputRouter == nullptr) {
      return 0;
    }
    if (const auto disconnected =
            session.inputRegistry->realtimeDisconnectedSdlDevice(*event);
        disconnected.has_value()) {
      session.physicalInputRouter->disconnectDevice(*disconnected, nowMicros());
      return 0;
    }
    std::array<input::PhysicalInputEvent, 4> physicalInputs{};
    const std::size_t inputCount =
        session.inputRegistry->translateRealtimeSdlInputs(*event,
                                                          physicalInputs);
    const std::int64_t timestamp = nowMicros();
    for (std::size_t index = 0; index < inputCount; ++index) {
      const auto &physical = physicalInputs[index];
      if (physical.control.deviceClass != input::DeviceClass::Keyboard &&
          physical.control.deviceClass != input::DeviceClass::GameController &&
          physical.control.deviceClass != input::DeviceClass::Joystick) {
        continue;
      }
      session.physicalInputRouter->consume(physical, timestamp);
    }
    return 0;
  }

  static void registryRealtimeInput(void *context,
                                    const input::PhysicalInputEvent &event) {
    if (context == nullptr) {
      return;
    }
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!session.acceptingNativeInput.load(std::memory_order_acquire) ||
        session.physicalInputRouter == nullptr ||
        !session.registryRealtimeEnabled(event.control.deviceClass)) {
      return;
    }
    std::int64_t timestamp = nowMicros();
    if (event.timestampMicros != 0) {
      if (event.timestampDomain ==
          input::InputTimestampDomain::SdlMilliseconds) {
        timestamp = input::rebaseWrappingTimestampMillis(
            static_cast<std::uint32_t>(event.timestampMicros / 1000U),
            SDL_GetTicks(), timestamp);
      } else {
        timestamp = event.timestampMicros >
                            static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max())
                        ? std::numeric_limits<std::int64_t>::max()
                        : static_cast<std::int64_t>(event.timestampMicros);
      }
    }
    session.physicalInputRouter->consume(event, timestamp);
  }

  static void registryRealtimeDevice(void *context,
                                     const input::InputDeviceSnapshot &device) {
    if (context == nullptr || device.connected) {
      return;
    }
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!session.acceptingNativeInput.load(std::memory_order_acquire) ||
        session.physicalInputRouter == nullptr ||
        !session.registryRealtimeEnabled(device.deviceClass)) {
      return;
    }
    session.physicalInputRouter->disconnectDevice(device.stableId, nowMicros());
  }

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  struct CancelledTouchExpiry {
    void *lifetimeToken = nullptr;
    gameplay::RealtimeTouchSample sample;
  };

  static void expireCancelledTouch(void *context) {
    std::unique_ptr<CancelledTouchExpiry> expiry(
        static_cast<CancelledTouchExpiry *>(context));
    if (!expiry) {
      return;
    }
    auto lease = NativeCallbackLifetime::acquire(expiry->lifetimeToken);
    auto *session = lease.ownerAs<RealtimeGameplaySession>();
    if (session == nullptr) {
      return;
    }
    gameplay::RealtimeTouchRoutingDisposition disposition =
        gameplay::RealtimeTouchRoutingDisposition::Inert;
    {
      std::lock_guard lock(session->touchRouterMutex);
      if (session->touchRouter == nullptr) {
        return;
      }
      disposition =
          session->touchRouter->consumeForPublication(expiry->sample);
    }
    if (gameplay::realtimeTouchRoutingRequiresRecovery(disposition)) {
      session->acceptingTouch.store(false, std::memory_order_release);
      session->touchRoutingRecoveryRequested.store(true,
                                                    std::memory_order_release);
    }
  }

  static void scheduleCancelledTouchExpiry(
      RealtimeGameplaySession &session,
      const gameplay::RealtimeTouchSample &cancelSample) {
    if (session.touchCallbackLifetime == nullptr) {
      return;
    }
    auto *expiry = new (std::nothrow) CancelledTouchExpiry{
        .lifetimeToken = session.touchCallbackLifetime->token(),
        .sample = cancelSample,
    };
    if (expiry == nullptr) {
      SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                  "Could not schedule cancelled-touch expiry");
      return;
    }
    expiry->sample.phase = gameplay::RealtimeTouchPhase::CancelExpired;
    expiry->sample.steadyTimestampMicros =
        cancelSample.steadyTimestampMicros >
                std::numeric_limits<std::int64_t>::max() - 50'000
            ? std::numeric_limits<std::int64_t>::max()
            : cancelSample.steadyTimestampMicros + 50'000;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                     dispatch_get_main_queue(), expiry,
                     &RealtimeGameplaySession::expireCancelledTouch);
  }

  static void SDLCALL rawTouchSink(const IOSRawTouchEvent *event,
                                   void *context) {
    if (event == nullptr || context == nullptr) {
      return;
    }
    auto &session = *static_cast<RealtimeGameplaySession *>(context);
    if (!session.acceptingTouch.load(std::memory_order_acquire) ||
        session.touchRouter == nullptr) {
      return;
    }
    gameplay::RealtimeTouchPhase phase;
    switch (event->phase) {
    case IOSRawTouchPhaseBegan:
      phase = gameplay::RealtimeTouchPhase::Down;
      break;
    case IOSRawTouchPhaseMoved:
      phase = gameplay::RealtimeTouchPhase::Move;
      break;
    case IOSRawTouchPhaseEnded:
      phase = gameplay::RealtimeTouchPhase::Up;
      break;
    case IOSRawTouchPhaseCancelled:
      phase = gameplay::RealtimeTouchPhase::Cancel;
      break;
    default:
      return;
    }
    gameplay::RealtimeTouchSample sample{
        .fingerId = event->fingerId,
        .phase = phase,
        .normalizedX = event->normalizedX,
        .normalizedY = event->normalizedY,
        .steadyTimestampMicros =
            session.touchTimestampSession.toSteadyMicros(
                event->timestampMicros),
    };
    session.populateImmutableHit(sample);
    sample.excludedFromGameplay =
        sample.presentationHit.kind != PresentationUiControlKind::None &&
        sample.presentationHit.kind !=
            PresentationUiControlKind::VirtualController;
    gameplay::RealtimeTouchRoutingDisposition disposition =
        gameplay::RealtimeTouchRoutingDisposition::RetryRequired;
    {
      std::lock_guard lock(session.touchRouterMutex);
      if (session.touchRouter != nullptr) {
        disposition = session.touchRouter->consumeForPublication(sample);
      }
    }
    bool auxiliaryPublished = false;
    if (gameplay::realtimeTouchRoutingPublishesAuxiliary(disposition)) {
      if (!session.auxiliaryTouches.tryPush(sample)) {
        // Stop admitting later callbacks until the game thread drains and
        // transactionally cancels every ownership domain.
        session.acceptingTouch.store(false, std::memory_order_release);
        session.auxiliaryTouchOverflow.store(true, std::memory_order_release);
      } else {
        auxiliaryPublished = true;
      }
    } else if (gameplay::realtimeTouchRoutingRequiresRecovery(disposition)) {
      // The sample was not accepted by the router, so publishing it to
      // presentation/replay would create mismatched ownership. Fail closed;
      // the normal overflow recovery releases the old contact and republishes
      // a clean snapshot before ingress resumes.
      session.acceptingTouch.store(false, std::memory_order_release);
      session.touchRoutingRecoveryRequested.store(true,
                                                  std::memory_order_release);
    }
    bool cancellationAcknowledged = false;
    if (phase == gameplay::RealtimeTouchPhase::Cancel &&
        auxiliaryPublished) {
      std::lock_guard lock(session.touchRouterMutex);
      cancellationAcknowledged =
          session.touchRouter != nullptr &&
          session.touchRouter->acknowledgePublishedCancellation(
              sample.fingerId);
      if (!cancellationAcknowledged) {
        session.acceptingTouch.store(false, std::memory_order_release);
        session.touchRoutingRecoveryRequested.store(true,
                                                    std::memory_order_release);
      }
    }
    if (phase == gameplay::RealtimeTouchPhase::Cancel &&
        gameplay::realtimeTouchShouldScheduleCancelExpiry(
            disposition, auxiliaryPublished, cancellationAcknowledged)) {
      scheduleCancelledTouchExpiry(session, sample);
    }
  }
#endif
};

bool GamePlayScene::realtimeGameplayAuthorityActive() const noexcept {
  return realtimeGameplaySession != nullptr &&
         realtimeGameplaySession->worker != nullptr;
}

void GamePlayScene::acquireGameplaySkinForAttempt() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  auto *coordinator =
      dynamic_cast<PlayfieldPresentationCoordinator *>(presentation);
  if (coordinator == nullptr || chart == nullptr ||
      !context.acquireGameplaySkinForNextChart) {
    return;
  }

  if (!context.skinStorageRoots || !context.skinResourcePreparationService ||
      !context.skinConfigurationWriteQueue ||
      !context.skinDiagnosticHistory || !context.skinLiveResourceCounters) {
    showPlaybackInitializationFailure(
        "Gameplay skin services are unavailable. Return to Settings, then "
        "try the selected skin again.");
    return;
  }

  skin::GameplaySkinAcquisition acquisition =
      context.acquireGameplaySkinForNextChart(chart->Meta.KeyMode);
  if (acquisition.disposition ==
      skin::GameplaySkinAcquisitionDisposition::Failed) {
    const skin::SkinDiagnostic diagnostic =
        acquisition.failure
            ? acquisition.failure->diagnostic
            : skin::SkinDiagnostic{
                  .code = "skin.lifecycle.acquisition_invalid",
                  .message =
                      "The selected gameplay skin returned no activation",
                  .severity = skin::DiagnosticSeverity::Error};
    showPlaybackInitializationFailure(gameplaySkinFailureMessage(diagnostic));
    return;
  }
  if (acquisition.disposition !=
          skin::GameplaySkinAcquisitionDisposition::Ready ||
      !acquisition.request) {
    return;
  }

  skin::GameplaySkinActivationRequest request =
      std::move(*acquisition.request);
  const skin::SkinEntryId capturedEntry = request.activation.entry;
  const std::string capturedRevisionDigest =
      request.activation.revision.revision().lowercaseSha256;
  const std::string capturedConfigurationDigest =
      request.activation.configurationDigest;
  const skin::UiLogicalRect safeUiBounds = gameplaySkinSafeUiBounds();
  try {
    auto created = skin::PlaySkinSession::create(
        std::move(request.activation),
        {.sessionSerial = request.sessionSerial,
         .profileId = std::move(request.profileId),
         .chartModel = playfieldChartVisualModel,
         .initialState = &capturedPlayfieldVisualState,
         .initialProjection = &capturedPlayfieldProjection,
         .viewport = request.viewport,
         .safeUiBounds = safeUiBounds,
         .storageRoots = *context.skinStorageRoots,
         .resourcePreparation = *context.skinResourcePreparationService,
         .textureDevice = std::make_shared<skin::BgfxSkinTextureDevice>(),
         .liveResourceCounters = context.skinLiveResourceCounters,
         .configurationWrites = *context.skinConfigurationWriteQueue,
         .stop = {}});
    std::optional<skin::SkinDiagnostic> failureDiagnostic;
    for (auto &diagnostic : created.diagnostics) {
      if (!failureDiagnostic &&
          diagnostic.severity == skin::DiagnosticSeverity::Error) {
        failureDiagnostic = diagnostic;
      }
      appendGameplaySkinDiagnostic(context, capturedEntry,
                                  capturedRevisionDigest,
                                  capturedConfigurationDigest,
                                  skin::SkinDiagnosticPhase::Session,
                                  std::move(diagnostic));
    }
    if (!created.session) {
      const skin::SkinDiagnostic diagnostic = failureDiagnostic.value_or(
          skin::SkinDiagnostic{
              .code = "skin.session.construction_failed",
              .message = "The selected gameplay skin could not start a "
                         "chart-lifetime session",
              .severity = skin::DiagnosticSeverity::Error});
      if (!failureDiagnostic) {
        appendGameplaySkinDiagnostic(
            context, capturedEntry, capturedRevisionDigest,
            capturedConfigurationDigest, skin::SkinDiagnosticPhase::Session,
            diagnostic);
      }
      showPlaybackInitializationFailure(gameplaySkinFailureMessage(diagnostic));
      return;
    }
    coordinator->installSkinSession(std::move(created.session));
    gameplaySkinSafeBoundsInitialized = true;
    gameplaySkinSafeBoundsX = safeUiBounds.x;
    gameplaySkinSafeBoundsY = safeUiBounds.y;
    gameplaySkinSafeBoundsWidth = safeUiBounds.width;
    gameplaySkinSafeBoundsHeight = safeUiBounds.height;
  } catch (...) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "Gameplay skin session construction failed");
    const skin::SkinDiagnostic diagnostic{
        .code = "skin.session.construction_exception",
        .message = "The selected gameplay skin threw while starting. Built-in "
                   "gameplay was not used as a replacement.",
        .severity = skin::DiagnosticSeverity::Error};
    appendGameplaySkinDiagnostic(context, capturedEntry, capturedRevisionDigest,
                                capturedConfigurationDigest,
                                skin::SkinDiagnosticPhase::Session,
                                diagnostic);
    showPlaybackInitializationFailure(gameplaySkinFailureMessage(diagnostic));
  }
#endif
}

void GamePlayScene::refreshGameplayPresentationGeometry() {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  auto *coordinator =
      dynamic_cast<PlayfieldPresentationCoordinator *>(presentation);
  if (coordinator == nullptr ||
      coordinator->activeMode() != PresentationMode::Skin) {
    return;
  }
  const skin::UiLogicalRect safeUiBounds = gameplaySkinSafeUiBounds();
  if (gameplaySkinSafeBoundsInitialized &&
      gameplaySkinSafeBoundsX == safeUiBounds.x &&
      gameplaySkinSafeBoundsY == safeUiBounds.y &&
      gameplaySkinSafeBoundsWidth == safeUiBounds.width &&
      gameplaySkinSafeBoundsHeight == safeUiBounds.height) {
    return;
  }
  coordinator->updateSkinViewportGeometry(safeUiBounds);
  gameplaySkinSafeBoundsInitialized = true;
  gameplaySkinSafeBoundsX = safeUiBounds.x;
  gameplaySkinSafeBoundsY = safeUiBounds.y;
  gameplaySkinSafeBoundsWidth = safeUiBounds.width;
  gameplaySkinSafeBoundsHeight = safeUiBounds.height;
#endif
}

void GamePlayScene::updateSkinResetLayoutVisibility() {
  if (skinResetLayoutButton == nullptr) {
    return;
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinResetLayoutButton->setVisible(
      presentation != nullptr &&
      presentation->activeMode() == PresentationMode::Skin &&
      (pauseLayout == nullptr || !pauseLayout->getVisible()));
#else
  skinResetLayoutButton->setVisible(false);
#endif
}

bool GamePlayScene::startRealtimeGameplayAuthority() {
  if (realtimeGameplayAuthorityActive() || chart == nullptr ||
      state == nullptr || presentation == nullptr) {
    return false;
  }

  std::optional<gameplay::GameplayTimeRange> realtimePracticeRange;
  if (const auto range = practiceNoteRange(); range.has_value()) {
    realtimePracticeRange = gameplay::GameplayTimeRange{
        .startMicros = range->startMicros,
        .endMicros = range->endMicros,
    };
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR || TARGET_OS_WINDOWS
  constexpr bool nativeManualInputAvailable = true;
#else
  constexpr bool nativeManualInputAvailable = false;
#endif
  const auto policy = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = nativeManualInputAvailable,
      .autoPlay = options.autoPlay,
      .inputHandlerAvailable = inputHandler != nullptr,
      .replayPlayback = isReplayPlayback(),
      .practiceMode = options.practiceMode,
      .practiceRange = realtimePracticeRange,
      .startPositionMicros = getStartPositionMicros(),
      .preparationActivationSongTimeMicros =
          preparationPlan.laneIndicator.enabled()
              ? std::optional<std::int64_t>(getGameplayTimeMicros(
                    preparationPlan.laneIndicator.endTimeMicros))
              : std::nullopt,
  });
  if (!policy.eligible) {
    return false;
  }

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::optional<gameplay::RealtimeTouchLayout> touchLayout;
  if (!options.autoPlay) {
    presentation->refreshGeometry();
    touchLayout = buildRealtimeTouchLayout(
        *presentation, assist_options::isDragMode(options.assistOption),
        chart->Meta, context.inputProfile.virtualController,
        realtimeTouchUiTransform());
    if (!touchLayout.has_value()) {
      realtimeGameplayAuthorityWaitingForSkinGeometry =
          presentation->activeMode() == PresentationMode::Skin;
      SDL_Log("Realtime iOS gameplay input unavailable: invalid touch layout");
      return false;
    }
  }
#endif

  auto definition =
      gameplay::buildGameplayDefinition(*chart, options.longNoteMode);
  auto session = std::make_unique<RealtimeGameplaySession>();
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  session->touchCallbackLifetime =
      std::make_unique<NativeCallbackLifetime>(session.get());
#endif
  session->scene = this;
  session->presentationTouches.setSink(
      {.context = session.get(),
       .begin = &RealtimeGameplaySession::beginPresentationTouch,
       .update = &RealtimeGameplaySession::updatePresentationTouch,
       .end = &RealtimeGameplaySession::endPresentationTouch,
       .cancelAll = &RealtimeGameplaySession::cancelPresentationTouches});
  session->audio = &context.jukebox.audioRuntime();
  session->inputRegistry = &context.inputDeviceRegistry;
  session->audioOffsetMicros = getAudioOffsetMicros();
  session->epoch = ++realtimeGameplayEpoch;
  session->notes = buildRealtimeGameplayNoteLookup(*chart);
  if (session->notes.size() != definition.noteCount()) {
    SDL_Log("Realtime gameplay input unavailable: note identity mismatch");
    return false;
  }
  session->soundHandles.resize(definition.keysoundSourceCount());
  if (!options.autoKeySound) {
    for (gameplay::NoteId id = 0; id < definition.keysoundSourceCount(); ++id) {
      const int wav = definition.keysoundSource(id).wav;
      if (wav != bms_parser::Parser::NoWav) {
        session->soundHandles[id] =
            context.jukebox.resolveRealtimeKeySound(wav);
      }
    }
  }

  const std::size_t automaticCapacity =
      std::max<std::size_t>(4096, definition.noteCount() * 3 + 1024);
  const std::size_t replayCapacity = gameplay::realtimeGameplayReplayCapacity(
      definition.noteCount(), definition.metadata().finalTimelineTimeMicros);
  gameplay::GameplaySimulationConfig simulationConfig{
      .judge = rulesetPolicyBuild.policy->judge,
      .gaugeRules = rulesetPolicyBuild.policy->gauge,
      .notePriorityMode = context.settings.notePriorityMode,
      .attempt =
          {
              .initialGaugeType = state->selectedGaugeType,
              .gaugeAutoShift = state->gaugeAutoShift,
              .gaugeProfile = state->gaugeProfile,
              .gaugeAutoShiftLowerBound = state->gaugeAutoShiftLowerBound,
              .carriedGauge = state->gaugeSnapshot(),
              .carriedCombo = state->combo,
              .carriedMaxCombo = state->maxCombo,
              .assistClearMark = state->assistClearMark,
              .autoPlay = options.autoPlay,
              .replayCapacity = replayCapacity,
              .automaticResultCapacity = automaticCapacity,
              .gaugeHistoryCapacity = automaticCapacity,
          },
  };
  simulationConfig.allowedNoteRange = policy.allowedNoteRange;
  gameplay::RealtimeGameplayWorkerConfig workerConfig{
      .epoch = session->epoch,
      .simulation = std::move(simulationConfig),
      .clock = {.context = session.get(),
                .mapSteadyToSong = &RealtimeGameplaySession::mapSteadyToSong,
                .currentSongTime = &RealtimeGameplaySession::currentSongTime},
      .audio = {.context = session.get(),
                .reserve = &RealtimeGameplaySession::reserveAudio,
                .commit = &RealtimeGameplaySession::commitAudio,
                .cancel = &RealtimeGameplaySession::cancelAudio},
      .inputTriggeredKeysounds = !options.autoKeySound,
      .activationSongTimeMicros = policy.activationSongTimeMicros,
      .practiceCompletionSongTimeMicros =
          policy.practiceCompletionSongTimeMicros,
  };
  session->worker = std::make_unique<gameplay::RealtimeGameplayWorker>(
      std::move(definition), std::move(workerConfig));
  session->legacyInputBridge =
      std::make_unique<gameplay::RealtimeGameplayInputBridge>(
          session->epoch,
          gameplay::RealtimeGameplayInputBridgeSink{
              .context = session.get(),
              .emit = &RealtimeGameplaySession::emitLegacyInput});
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  if (touchLayout.has_value()) {
    session->touchRouter = std::make_unique<gameplay::RealtimeTouchInputRouter>(
        session->epoch, *touchLayout,
        gameplay::RealtimeTouchInputSink{
            .context = session.get(),
            .emit = &RealtimeGameplaySession::emitTouchInput,
            .scratchLongNoteHeld =
                &RealtimeGameplaySession::scratchLongNoteHeld,
            .cancelTouchLifecycle =
                &RealtimeGameplaySession::cancelTouchLifecycle});
  }
#endif
  if (!options.autoPlay) {
    const auto activeInputScopes = makeGameplayInputScopes(chart->Meta.KeyMode);
    const auto realtimeInputProfile =
        makeGameplayInputProfileWithEscapeFallback(context.inputProfile,
                                                   activeInputScopes);
    session->physicalInputRouter =
        std::make_unique<input::RealtimePhysicalInputRouter>(
            realtimeInputProfile, activeInputScopes,
            [context = session.get()](const auto &transition) {
              return RealtimeGameplaySession::emitPhysicalInput(context,
                                                                transition);
            });
  }
  session->visualMeasureIndex = state->passedMeasureCount;
  session->visualTimelineIndex = state->passedTimelineCount;
  session->layoutRefreshKey = makeRealtimeTouchLayoutRefreshKey(
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
      effectiveRealtimeTouchLayoutRevision(
          presentation->touchLayoutRevision(),
          context.inputProfile.virtualController, chart->Meta.KeyMode),
      presentation->touchHitRegionsRevision(),
#else
      0, 0,
#endif
      pauseButton, practiceRestartButton, skinResetLayoutButton);
  if (!session->worker->start()) {
    SDL_Log("Realtime gameplay worker failed to start");
    return false;
  }

  if (inputHandler != nullptr) {
    inputHandler->discardPendingTouchEvents();
  }
  realtimeGameplaySession = std::move(session);
  realtimeGameplayAuthorityWaitingForSkinGeometry = false;
  auto &activeSession = *realtimeGameplaySession;
  if (options.autoPlay) {
    SDL_Log("Realtime autoplay authority active (epoch %llu)",
            static_cast<unsigned long long>(realtimeGameplayEpoch));
    return true;
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  for (const auto deviceClass :
       {input::DeviceClass::Keyboard, input::DeviceClass::GameController,
        input::DeviceClass::Joystick, input::DeviceClass::Midi,
        input::DeviceClass::Gyroscope}) {
    activeSession
        .claimedRealtimeClasses[static_cast<std::size_t>(deviceClass)] = true;
  }
  activeSession.registryRealtimeClasses[static_cast<std::size_t>(
      input::DeviceClass::Midi)] = true;
  activeSession.registryRealtimeClasses[static_cast<std::size_t>(
      input::DeviceClass::Gyroscope)] = true;
#elif TARGET_OS_WINDOWS
  for (const auto deviceClass :
       {input::DeviceClass::Keyboard, input::DeviceClass::GameController,
        input::DeviceClass::Midi}) {
    activeSession
        .claimedRealtimeClasses[static_cast<std::size_t>(deviceClass)] = true;
    activeSession
        .registryRealtimeClasses[static_cast<std::size_t>(deviceClass)] = true;
  }
#endif
  activeSession.realtimeInputSubscription =
      context.inputDeviceRegistry.subscribeRealtimeInput(
          [session = &activeSession](const auto &event) {
            RealtimeGameplaySession::registryRealtimeInput(session, event);
          });
  activeSession.realtimeDeviceSubscription =
      context.inputDeviceRegistry.subscribeRealtimeDevices(
          [session = &activeSession](const auto &device) {
            RealtimeGameplaySession::registryRealtimeDevice(session, device);
          });
  for (std::size_t index = 0;
       index < activeSession.claimedRealtimeClasses.size(); ++index) {
    if (!activeSession.claimedRealtimeClasses[index]) {
      continue;
    }
    const auto deviceClass = static_cast<input::DeviceClass>(index);
    inputHandler->setRegistryDeviceClassEnabled(deviceClass, false);
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  SDL_AddEventWatch(&RealtimeGameplaySession::sdlInputWatch, &activeSession);
  activeSession.sdlInputWatchRegistered = true;
#endif
  setRealtimeGameplayIngressEnabled(true);
  activeSession.acceptingNativeInput.store(true, std::memory_order_release);
  for (std::size_t index = 0;
       index < activeSession.claimedRealtimeClasses.size(); ++index) {
    if (activeSession.claimedRealtimeClasses[index]) {
      context.inputDeviceRegistry.setRealtimeInputClaimed(
          static_cast<input::DeviceClass>(index), true);
    }
  }
  SDL_Log("Realtime gameplay native input authority active (epoch %llu)",
          static_cast<unsigned long long>(realtimeGameplayEpoch));
  return true;
}

void GamePlayScene::setRealtimeGameplayIngressEnabled(bool enabled) {
  if (!realtimeGameplayAuthorityActive()) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    IOSSetRawTouchEventSink(nullptr, nullptr);
#endif
    return;
  }
  auto &session = *realtimeGameplaySession;
  const auto timestampMicros = nowMicros();
  if (session.physicalInputRouter != nullptr) {
    session.physicalInputRouter->setGameplayEnabled(enabled, timestampMicros);
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  session.touchIngressDesired = enabled;
  if (enabled) {
    if (session.acceptingTouch.load(std::memory_order_acquire)) {
      return;
    }
    // SDL's UIKit bridge holds its callback spinlock until an in-flight raw
    // callback returns. Detach before mutating any raw-thread-owned state.
    IOSSetRawTouchEventSink(nullptr, nullptr);
    session.acceptingTouch.store(false, std::memory_order_release);
    const auto expectedKey = makeRealtimeTouchLayoutRefreshKey(
        presentation != nullptr
            ? effectiveRealtimeTouchLayoutRevision(
                  presentation->touchLayoutRevision(),
                  context.inputProfile.virtualController,
                  chart != nullptr ? chart->Meta.KeyMode : 0)
            : 0,
        presentation != nullptr ? presentation->touchHitRegionsRevision() : 0,
        pauseButton, practiceRestartButton, skinResetLayoutButton);
    if (presentation == nullptr || session.layoutRefreshKey != expectedKey) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                   "Realtime touch ingress remains disabled until current layout publication succeeds");
      session.touchHitSnapshotDirty = true;
      return;
    }
    if ((session.touchHitSnapshotDirty ||
         !session.touchHitSnapshots.acquire()) &&
        !publishRealtimeTouchHitSnapshot()) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                   "Realtime touch ingress remains disabled without an immutable hit snapshot");
      return;
    }
    session.touchTimestampSession.reanchor();
    bool routerEnabled = true;
    {
      std::lock_guard lock(session.touchRouterMutex);
      routerEnabled = session.touchRouter == nullptr ||
                      session.touchRouter->setGameplayEnabled(
                          true, timestampMicros);
    }
    if (!routerEnabled) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                   "Realtime touch ingress remains disabled until held lanes release");
      return;
    }
    session.acceptingTouch.store(true, std::memory_order_release);
    IOSSetRawTouchEventSink(&RealtimeGameplaySession::rawTouchSink, &session);
    return;
  }
  IOSSetRawTouchEventSink(nullptr, nullptr);
  session.acceptingTouch.store(false, std::memory_order_release);
  bool routerDisabled = true;
  {
    std::lock_guard lock(session.touchRouterMutex);
    routerDisabled = session.touchRouter == nullptr ||
                     session.touchRouter->setGameplayEnabled(
                         false, timestampMicros);
  }
  if (!routerDisabled) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime touch release failed while closing ingress");
  }
  drainRealtimeTouchSamples(timestampMicros);
  cancelLegacyFloatingLaneCoverTouch();
  session.requestHitCaptureReset();
#else
  (void)enabled;
#endif
}

bool GamePlayScene::publishRealtimeTouchHitSnapshot() {
  if (!realtimeGameplayAuthorityActive() || presentation == nullptr) {
    return false;
  }
  auto &session = *realtimeGameplaySession;
  gameplay::RealtimeTouchHitSnapshot snapshot{
      .layoutRevision = session.layoutRefreshKey.layoutRevision,
      .uiTransform = session.layoutRefreshKey.uiTransform};
  const auto appendNativeOverlay = [&](const auto &overlay) {
    if (!overlay.visible) {
      return;
    }
    snapshot.regionsTopmostFirst.push_back(
        {.hit = {.kind = PresentationUiControlKind::NativeOverlay},
         .boundary = {{{overlay.left, overlay.top},
                       {overlay.right, overlay.top},
                       {overlay.right, overlay.bottom},
                       {overlay.left, overlay.bottom}}}});
  };
  try {
    for (const auto &overlay : session.layoutRefreshKey.nativeOverlays) {
      appendNativeOverlay(overlay);
    }
    if (chart != nullptr) {
      appendVirtualControllerHitRegions(
          snapshot.regionsTopmostFirst,
          currentVirtualControllerLayout(context.inputProfile.virtualController,
                                         chart->Meta.KeyMode,
                                         session.layoutRefreshKey.uiTransform),
          session.layoutRefreshKey.layoutRevision);
    }
    auto presentationRegions = presentation->touchHitRegions();
    snapshot.regionsTopmostFirst.insert(
        snapshot.regionsTopmostFirst.end(),
        std::make_move_iterator(presentationRegions.begin()),
        std::make_move_iterator(presentationRegions.end()));
    if (session.touchHitSnapshots.publish(std::move(snapshot))) {
      session.touchHitSnapshotDirty = false;
      return true;
    }
  } catch (...) {
  }
  SDL_LogError(SDL_LOG_CATEGORY_INPUT,
               "Realtime touch hit snapshot publication failed; preserving the last good snapshot");
  session.touchHitSnapshotDirty = true;
  return false;
}

void GamePlayScene::drainRealtimeTouchSamples(
    std::optional<long long> cancelPresentationAtSteadyMicros) {
  if (!realtimeGameplayAuthorityActive()) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  const long long visualGameplayTimeMicros =
      getGameplayTimeMicros(context.jukebox.getTimeMicros());
  const auto consumeSample = [&](const gameplay::RealtimeTouchSample &sample) {
    const auto gameplayTime = RealtimeGameplaySession::mapSteadyToSong(
        &session, sample.steadyTimestampMicros);
    (void)session.presentationTouches.consume(
        sample, gameplayTime.value_or(sample.steadyTimestampMicros));
    if (!gameplayTime.has_value()) {
      return;
    }
    ReplayTouchAction action = ReplayTouchAction::Move;
    switch (sample.phase) {
    case gameplay::RealtimeTouchPhase::Down:
      action = ReplayTouchAction::Down;
      break;
    case gameplay::RealtimeTouchPhase::Move:
      action = ReplayTouchAction::Move;
      break;
    case gameplay::RealtimeTouchPhase::Up:
      action = ReplayTouchAction::Up;
      break;
    case gameplay::RealtimeTouchPhase::Cancel:
      action = ReplayTouchAction::Cancel;
      break;
    case gameplay::RealtimeTouchPhase::CancelExpired:
      return;
    }
    const auto presentationPoint = sample.presentationPoint.value_or(
        gameplay::realtimeTouchPresentationPoint(sample.normalizedX,
                                                  sample.normalizedY));
    (void)handleTouchInputAtGameplayTime(
        static_cast<SDL_FingerID>(sample.fingerId), action,
        Vector3(presentationPoint.x, presentationPoint.y, 0.0F), *gameplayTime,
        visualGameplayTimeMicros,
        gameplay::realtimeTouchAllowsLegacyBuiltInControl(
            sample.presentationHit));
  };
  gameplay::RealtimeTouchSample sample;
  while (session.auxiliaryTouches.tryPop(sample)) {
    consumeSample(sample);
  }
  const bool metadataOverflow = session.auxiliaryTouchOverflow.exchange(
      false, std::memory_order_acq_rel);
  const bool routingRecovery = session.touchRoutingRecoveryRequested.exchange(
      false, std::memory_order_acq_rel);
  if (metadataOverflow || routingRecovery) {
    SDL_LogWarn(
        SDL_LOG_CATEGORY_INPUT,
        metadataOverflow
            ? "Realtime touch metadata queue overflowed; cancelling active touch ownership"
            : "Realtime touch routing rejected a sample; cancelling active touch ownership");
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    IOSSetRawTouchEventSink(nullptr, nullptr);
#endif
    session.acceptingTouch.store(false, std::memory_order_release);
    const auto recoverySteadyMicros = nowMicros();
    bool routerCancelled = true;
    {
      std::lock_guard lock(session.touchRouterMutex);
      if (session.touchRouter != nullptr) {
        routerCancelled =
            session.touchRouter->cancelAll(recoverySteadyMicros);
      }
    }
    while (session.auxiliaryTouches.tryPop(sample)) {
      consumeSample(sample);
    }
    const auto recoveryEventMicros = RealtimeGameplaySession::mapSteadyToSong(
        &session, recoverySteadyMicros);
    session.presentationTouches.reconcileMetadataOverflow(
        recoveryEventMicros.value_or(recoverySteadyMicros));
    cancelLegacyFloatingLaneCoverTouch();
    session.requestHitCaptureReset();
    if (session.touchIngressDesired && routerCancelled) {
      setRealtimeGameplayIngressEnabled(true);
    }
  }
  if (cancelPresentationAtSteadyMicros) {
    const auto eventMicros = RealtimeGameplaySession::mapSteadyToSong(
        &session, *cancelPresentationAtSteadyMicros);
    session.presentationTouches.cancelAll(
        eventMicros.value_or(*cancelPresentationAtSteadyMicros));
  }
}

void GamePlayScene::drainRealtimeInputCommands() {
  if (!realtimeGameplayAuthorityActive()) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  input::LogicalInputTransition transition;
  while (session.inputCommands.tryPop(transition)) {
    handleLogicalInputCommand(transition);
  }
  if (session.inputCommandOverflow.exchange(false, std::memory_order_acq_rel)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Realtime input command queue overflowed");
  }
}

void GamePlayScene::drainRealtimeStartSelectInputs() {
  if (!realtimeGameplayAuthorityActive()) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  gameplay::StartSelectControlInput input;
  while (session.startSelectInputs.tryPop(input)) {
    consumeStartSelectInput(input);
    if (!realtimeGameplayAuthorityActive()) {
      return;
    }
  }
  if (session.startSelectInputOverflow.exchange(false,
                                                std::memory_order_acq_rel)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Start/Select input queue overflowed; a control edge was dropped");
  }
}

void GamePlayScene::refreshRealtimeTouchLayout() {
  if (!realtimeGameplayAuthorityActive() || chart == nullptr ||
      presentation == nullptr || realtimeGameplaySession->worker == nullptr ||
      !realtimeGameplaySession->worker->running() ||
      realtimeGameplaySession->touchRouter == nullptr) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  if (pauseButton != nullptr) {
    pauseButton->setPositionNoLayout(rendering::window_width - 88, 38);
  }
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setPositionNoLayout(rendering::window_width - 88,
                                               98);
  }
  refreshGameplayPresentationGeometry();
  const auto currentKey = makeRealtimeTouchLayoutRefreshKey(
      effectiveRealtimeTouchLayoutRevision(
          presentation->touchLayoutRevision(),
          context.inputProfile.virtualController, chart->Meta.KeyMode),
      presentation->touchHitRegionsRevision(), pauseButton,
      practiceRestartButton, skinResetLayoutButton);
  if (session.layoutRefreshKey == currentKey &&
      !session.touchHitSnapshotDirty) {
    if (session.touchIngressDesired &&
        !session.acceptingTouch.load(std::memory_order_acquire)) {
      setRealtimeGameplayIngressEnabled(true);
    }
    return;
  }
  const bool presentationLayoutChanged =
      session.layoutRefreshKey.layoutRevision != currentKey.layoutRevision ||
      session.layoutRefreshKey.uiTransform != currentKey.uiTransform;
  if (!presentationLayoutChanged) {
    session.layoutRefreshKey = currentKey;
    if (publishRealtimeTouchHitSnapshot() && session.touchIngressDesired &&
        !session.acceptingTouch.load(std::memory_order_acquire)) {
      setRealtimeGameplayIngressEnabled(true);
    }
    return;
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  // A changed topology or transform invalidates the currently published raw
  // routing immediately. Detach before asking the presentation to rebuild so
  // an invalid/transient resize cannot keep accepting touches through stale
  // geometry.
  IOSSetRawTouchEventSink(nullptr, nullptr);
#endif
  session.acceptingTouch.store(false, std::memory_order_release);
  const auto switchMicros = nowMicros();
  presentation->refreshGeometry();
  const auto layout = buildRealtimeTouchLayout(
      *presentation, assist_options::isDragMode(options.assistOption),
      chart->Meta, context.inputProfile.virtualController,
      currentKey.uiTransform);
  if (!layout.has_value()) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime touch layout refresh failed");
    bool cancelled = true;
    {
      std::lock_guard lock(session.touchRouterMutex);
      cancelled = session.touchRouter == nullptr ||
                  session.touchRouter->cancelAll(switchMicros);
    }
    drainRealtimeTouchSamples(switchMicros);
    cancelLegacyFloatingLaneCoverTouch();
    session.requestHitCaptureReset();
    session.touchHitSnapshotDirty = true;
    if (!cancelled) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                   "Realtime touch cancellation will retry while layout remains invalid");
    }
    return;
  }
  bool updated = false;
  {
    std::lock_guard lock(session.touchRouterMutex);
    updated = session.touchRouter != nullptr &&
              session.touchRouter->updateLayout(*layout, switchMicros);
  }
  drainRealtimeTouchSamples(switchMicros);
  if (!updated) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime touch layout refresh failed");
    cancelLegacyFloatingLaneCoverTouch();
    session.requestHitCaptureReset();
    return;
  }
  cancelLegacyFloatingLaneCoverTouch();
  session.requestHitCaptureReset();
  session.layoutRefreshKey = currentKey;
  session.layoutRefreshKey.layoutRevision = layout->revision;
  session.layoutRefreshKey.hitRegionRevision =
      presentation->touchHitRegionsRevision();
  const bool published = publishRealtimeTouchHitSnapshot();
  if (published && session.touchIngressDesired) {
    setRealtimeGameplayIngressEnabled(true);
  }
}

void GamePlayScene::updateRealtimeVisualTimeline(long long gameplayTimeMicros) {
  if (!realtimeGameplayAuthorityActive() || chart == nullptr) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  while (session.visualMeasureIndex < chart->Measures.size()) {
    const auto *measure = chart->Measures[session.visualMeasureIndex];
    if (measure == nullptr ||
        session.visualTimelineIndex >= measure->TimeLines.size()) {
      ++session.visualMeasureIndex;
      session.visualTimelineIndex = 0;
      continue;
    }
    const auto *timeline = measure->TimeLines[session.visualTimelineIndex];
    if (timeline == nullptr) {
      ++session.visualTimelineIndex;
      continue;
    }
    if (timeline->Timing > gameplayTimeMicros) {
      return;
    }
    applyTimelineBpm(timeline);
    ++session.visualTimelineIndex;
  }
}

void GamePlayScene::syncRealtimeGameplaySnapshot() {
  if (!realtimeGameplayAuthorityActive() || state == nullptr) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  refreshRealtimeTouchLayout();
  auto snapshot = session.worker->acquireLatestSnapshot();
  if (!snapshot || snapshot->generation == session.appliedSnapshotGeneration) {
    return;
  }
  session.appliedSnapshotGeneration = snapshot->generation;

  const std::size_t noteCount =
      std::min(session.notes.size(), snapshot->noteStates.size());
  for (std::size_t index = 0; index < noteCount; ++index) {
    auto *note = session.notes[index];
    if (note == nullptr) {
      continue;
    }
    const auto &runtime = snapshot->noteStates[index];
    note->IsPlayed = runtime.played;
    note->IsDead = runtime.dead;
    note->PlayedTime = runtime.playedTimeMicros;
    if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
        longNote != nullptr) {
      longNote->IsHolding = runtime.holding;
      longNote->ReleaseTime = runtime.releaseTimeMicros;
    }
  }

  auto gaugeHistory = std::move(state->gaugeHistory);
  auto gaugeHistories = std::move(state->gaugeHistories);
  state->restoreGaugeState(snapshot->gaugeState);
  state->gaugeHistory = std::move(gaugeHistory);
  state->gaugeHistories = std::move(gaugeHistories);
  state->combo = snapshot->attempt.combo;
  state->maxCombo = snapshot->attempt.maxCombo;
  state->comboBreak = snapshot->attempt.comboBreak;
  state->fastCount = snapshot->fastCount;
  state->slowCount = snapshot->slowCount;
  for (int judgement = 0; judgement < JudgementCount; ++judgement) {
    const auto value = static_cast<Judgement>(judgement);
    state->judgeCount[value] = snapshot->attempt.judgeCounts[judgement];
    state->judgementFastSlowCount[value] = snapshot->fastSlowCounts[judgement];
  }

  std::array<int, gameplay::kRealtimeGameplayTransactionHistorySize>
      lanesWithNewVisual{};
  std::size_t lanesWithNewVisualCount = 0;
  const long long visualCatchUpMicros = playfieldVisualEventTimeMicros(
      getGameplayTimeMicros(context.jukebox.getTimeMicros()),
      getVisualOffsetMicros());
  for (std::size_t index = 0; index < snapshot->transactionCount; ++index) {
    const auto &transaction = snapshot->transactions[index];
    if (transaction.sequence <= session.appliedTransactionSequence) {
      continue;
    }
    const auto &result = transaction.result;
    if (result.hasLaneVisual) {
      const auto &visual = result.laneVisual;
      const long long eventVisualTimeMicros =
          playfieldVisualEventTimeMicros(
              visual.songTimeMicros, getVisualOffsetMicros());
      if (visual.lane >= 0 &&
          lanesWithNewVisualCount < lanesWithNewVisual.size()) {
        lanesWithNewVisual[lanesWithNewVisualCount++] = visual.lane;
      }
      if (visual.action == gameplay::LaneVisualAction::Press) {
        presentationEventFanout->onLanePressed(
            visual.lane, visual.judge, eventVisualTimeMicros);
      } else {
        presentationEventFanout->onLaneReleased(visual.lane,
                                                eventVisualTimeMicros);
      }
    }
    if (result.hasJudge) {
      const long long eventSongTimeMicros = result.replayEvent.songTimeMicros;
      const int eventCombo = result.replayEvent.combo;
      const int eventScore = result.replayEvent.score;
      presentationEventFanout->onJudge(
          result.judge, eventCombo, eventScore,
          judgeEventClock(eventSongTimeMicros), true);
    }
    session.appliedTransactionSequence = transaction.sequence;
  }

  const auto lanes = chart->Meta.GetTotalLaneIndices();
  for (const int lane : lanes) {
    if (lane < 0 ||
        static_cast<std::size_t>(lane) >= snapshot->lanePressed.size()) {
      continue;
    }
    const bool pressed = snapshot->lanePressed[lane];
    const bool previous = lanePressed[lane];
    lanePressed[lane] = pressed;
    if (pressed == previous ||
        std::find(lanesWithNewVisual.begin(),
                  lanesWithNewVisual.begin() + lanesWithNewVisualCount,
                  lane) != lanesWithNewVisual.begin() +
                               lanesWithNewVisualCount) {
      continue;
    }
    if (pressed) {
      presentationEventFanout->onLanePressed(
          lane, JudgeResult(None, 0), visualCatchUpMicros);
    } else {
      presentationEventFanout->onLaneReleased(lane, visualCatchUpMicros);
    }
  }
  session.appliedTransactionSequence = std::max(
      session.appliedTransactionSequence, snapshot->transactionSequence);
  updatePacemakerStatus();
  updateLaneStateText();
}

void GamePlayScene::stopRealtimeGameplayAuthority(bool transferReplay) {
  if (!realtimeGameplayAuthorityActive()) {
    return;
  }
  auto &session = *realtimeGameplaySession;
  setRealtimeGameplayIngressEnabled(false);
  session.acceptingNativeInput.store(false, std::memory_order_release);
  if (session.sdlInputWatchRegistered) {
    SDL_DelEventWatch(&RealtimeGameplaySession::sdlInputWatch, &session);
    session.sdlInputWatchRegistered = false;
  }
  if (session.realtimeInputSubscription != 0) {
    context.inputDeviceRegistry.unsubscribe(session.realtimeInputSubscription);
    session.realtimeInputSubscription = 0;
  }
  if (session.realtimeDeviceSubscription != 0) {
    context.inputDeviceRegistry.unsubscribe(session.realtimeDeviceSubscription);
    session.realtimeDeviceSubscription = 0;
  }
  for (std::size_t index = 0; index < session.claimedRealtimeClasses.size();
       ++index) {
    if (!session.claimedRealtimeClasses[index]) {
      continue;
    }
    const auto deviceClass = static_cast<input::DeviceClass>(index);
    context.inputDeviceRegistry.setRealtimeInputClaimed(deviceClass, false);
    if (inputHandler != nullptr) {
      inputHandler->setRegistryDeviceClassEnabled(deviceClass, true);
    }
  }
  drainRealtimeTouchSamples();
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  // A first cancellation can fail transactionally when either bounded queue
  // is full. Draining above makes the normal recovery path available; retry
  // before stopping the worker so every accepted Press can still acquire its
  // matching Release. If the retry also fails, do not publish an incomplete
  // replay as valid evidence.
  bool touchCancellationComplete = true;
  const auto finalTouchMicros = nowMicros();
  {
    std::lock_guard lock(session.touchRouterMutex);
    touchCancellationComplete = session.touchRouter == nullptr ||
                                session.touchRouter->setGameplayEnabled(
                                    false, finalTouchMicros);
  }
  drainRealtimeTouchSamples(finalTouchMicros);
  if (!touchCancellationComplete) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime touch cancellation failed during final shutdown; replay transfer is invalid");
    transferReplay = false;
  }
  // No raw callback can enter after sink detachment. Closing the delayed
  // callback lifetime here also prevents a queued cancellation-expiry callback
  // from racing worker shutdown; destruction remains an idempotent fallback.
  if (session.touchCallbackLifetime != nullptr) {
    session.touchCallbackLifetime->closeAndWait();
  }
#endif
  session.worker->stop();
  syncRealtimeGameplaySnapshot();
  if (state != nullptr) {
    state->gaugeHistory = session.worker->copyGaugeHistoryAfterStop();
    state->gaugeHistories = session.worker->copyGaugeHistoriesAfterStop();
  }

  if (transferReplay) {
    if (modernReplayInputRecorder != nullptr) {
      completedModernReplayInput =
          session.worker->copyAcceptedReplayInputAfterStop();
      if (!completedModernReplayInput.has_value()) {
        modernReplayCaptureDiagnostic =
            "Realtime raw replay input capture was unavailable.";
      }
      modernReplayInputRecorder.reset();
    }
    const auto capturePolicy = resultCapturePolicy();
    const auto workerEvents = session.worker->copyReplayEventsAfterStop();
    for (const auto &source : workerEvents) {
      ReplayEvent event{
          .action = replayActionFromRealtime(source.action),
          .lane = source.lane,
          .noteTimeMicros = source.noteTimeMicros,
          .songTimeMicros = source.songTimeMicros,
          .judgeTimeMicros = source.judgeTimeMicros,
          .judgement = source.judgement,
          .diffMicros = source.diffMicros,
          .gauge = source.gauge,
          .gaugeType = source.gaugeType,
          .combo = source.combo,
          .score = source.score,
      };
      if (capturePolicy.captureAnalytics) {
        analyticsReplay.events.push_back(event);
      }
      if (capturePolicy.recordReplay) {
        recordedReplay.events.push_back(event);
      }
    }
  }
  if (inputHandler != nullptr) {
    inputHandler->discardPendingTouchEvents();
  }
  realtimeGameplaySession.reset();
}

GamePlayScene::GamePlayScene(ApplicationContext &context,
                             bms_parser::Chart *chart, StartOptions options)
    : Scene(context), ownedChart(options.ownsChart ? chart : nullptr),
      chart(options.ownsChart ? ownedChart.get() : chart),
      options(enforceCoursePlaybackRules(resolvePlayStartInputDevices(
          std::move(options), context.inputProfile, chart->Meta.KeyMode))),
      rulesetPolicyBuild(buildGameplayRulesetPolicyAtPlayStart(
          this->options, this->chart->Meta, context.settings.notePriorityMode)),
      judge(presentationJudgeForPolicy(rulesetPolicyBuild,
                                       this->chart->Meta.Rank)) {
  judge.setAllowedNoteRange(practiceAllowedNoteRange(this->options));
  latePoorTiming =
      rulesetPolicyBuild.policy.has_value()
          ? rulesetPolicyBuild.policy->judge.automaticPoorLateMicros()
                       : judge.timingWindows[Bad].second;
  if (rulesetPolicyBuild.policy.has_value()) {
    attemptProvenance = captureScoreProvenanceAtPlayStart(
        this->options, this->chart->Meta, *rulesetPolicyBuild.policy);
  }
}

GamePlayScene::GamePlayScene(ApplicationContext &context,
                             std::unique_ptr<bms_parser::Chart> chart,
                             StartOptions options)
    : Scene(context), ownedChart(std::move(chart)), chart(ownedChart.get()),
      options(enforceCoursePlaybackRules(
          resolvePlayStartInputDevices(std::move(options), context.inputProfile,
                                       this->chart->Meta.KeyMode))),
      rulesetPolicyBuild(buildGameplayRulesetPolicyAtPlayStart(
          this->options, this->chart->Meta, context.settings.notePriorityMode)),
      judge(presentationJudgeForPolicy(rulesetPolicyBuild,
                                       this->chart->Meta.Rank)) {
  this->options.ownsChart = true;
  judge.setAllowedNoteRange(practiceAllowedNoteRange(this->options));
  latePoorTiming =
      rulesetPolicyBuild.policy.has_value()
          ? rulesetPolicyBuild.policy->judge.automaticPoorLateMicros()
                       : judge.timingWindows[Bad].second;
  if (rulesetPolicyBuild.policy.has_value()) {
    attemptProvenance = captureScoreProvenanceAtPlayStart(
        this->options, this->chart->Meta, *rulesetPolicyBuild.policy);
  }
}

GamePlayScene::~GamePlayScene() {
  stopBestReplayLoad();
  stopRealtimeGameplayAuthority(false);
  if (profileGameplayBlockerActive) {
    context.profileGameplayActive.store(false, std::memory_order_release);
  }
}

void GamePlayScene::init() {
  context.profileGameplayActive.store(true, std::memory_order_release);
  profileGameplayBlockerActive = true;
  if (!rulesetPolicyBuild.built()) {
    showPlaybackInitializationFailure(
        rulesetPolicyBuild.diagnostic.empty()
            ? "The selected gameplay ruleset could not be started."
            : rulesetPolicyBuild.diagnostic);
    return;
  }
  if (chart != nullptr) {
    const int replayLongNoteMode =
        options.replayData != nullptr
            ? options.replayData->chartMeta.LnMode
            : (options.gbattleRecordData != nullptr
                   ? options.gbattleRecordData->chartMeta.LnMode
                   : 0);
    applyEffectiveLongNoteModeToChart(*chart, replayLongNoteMode > 0
                                                  ? replayLongNoteMode
                                                  : options.longNoteMode);
  }
  startSelectControl.emplace(
      gameplay::StartSelectControl::Configuration{.keyMode = chart->Meta.KeyMode});
  playfieldHispeedMultiplier = context.settings.gameplayHispeedMultiplier;
  playfieldLaneCoverEnabled = context.settings.laneCoverEnabled;
  playfieldLaneCoverPercent = effectiveNoteStartPositionPercent();
  playfieldLaneCoverPercentExact =
      static_cast<float>(playfieldLaneCoverPercent);
  refreshLaneCoverHispeedFactor();
  playfieldChartVisualModel =
      buildPlayfieldChartVisualModel(*chart, options.longNoteMode);
  initializePlayfieldVisualNoteSources();
  ownedPlayfieldVisualStateStore =
      std::make_unique<PlayfieldVisualStateStore>(playfieldChartVisualModel);
  playfieldVisualStateStore = ownedPlayfieldVisualStateStore.get();
  auto builtIn = createBuiltInPlayfieldPresentation({
      .chart = *chart,
      .timingWindows = judge.timingWindows,
      .visibleTimeGreenNumber = effectiveVisibleTimeGreenNumber(),
      .renderHud = true,
      .playbackRate = options.playback,
      .replayData = options.replayData.get(),
  });
  builtInPresentation = builtIn.get();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  auto coordinator = std::make_unique<PlayfieldPresentationCoordinator>(
      PlayfieldPresentationCoordinatorDependencies{
          .builtIn = std::move(builtIn),
          .skin = {},
          .bga = context.jukebox,
          .persistViewport =
              [](const skin::PlaySkinSessionIdentity &,
                 skin::ViewportSettings) {
                return GameplayViewportPersistenceResult{
                    .disposition =
                        GameplayViewportPersistenceDisposition::Rejected,
                    .diagnostic = skin::SkinDiagnostic{
                        .code = "skin.presentation.viewport_lifecycle_pending",
                        .message =
                            "The Fit viewport is active for this chart; "
                            "durable lifecycle persistence is not connected.",
                        .severity = skin::DiagnosticSeverity::Warning,
                    }};
              },
          .recordFailure =
              [this](const PresentationFailure &failure) {
                appendGameplaySkinDiagnostic(
                    context, failure.entry, failure.revisionDigest,
                    failure.configurationDigest,
                    skin::SkinDiagnosticPhase::FrameFallback,
                    failure.diagnostic,
                    failure.frameSerial == 0
                        ? std::nullopt
                        : std::optional<std::uint64_t>(failure.frameSerial));
              },
      });
  ownedPresentation = std::move(coordinator);
#else
  ownedPresentation = std::move(builtIn);
#endif
  presentation = ownedPresentation.get();
  playfieldPresentationConfiguration = {
      .visibleTimeGreenNumber = effectiveVisibleTimeGreenNumber(),
      .hispeedMultiplier = playfieldHispeedMultiplier,
      .visibleTimeUseMilliseconds =
          !courseNoSpeed() && context.settings.visibleTimeUseMilliseconds,
      .visibleTimeBpmStrategy =
          courseNoSpeed() ? AppSettings::VisibleTimeBpmStrategy::Chart
                          : context.settings.visibleTimeBpmStrategy,
      .playAreaWidth =
          context.settings.playAreaWidthForKeyMode(chart->Meta.KeyMode),
      .laneBeamsEnabled = true,
      .laneCoverHispeedFactor = playfieldLaneCoverHispeedFactor,
      .laneBeamLengthPercent = context.settings.laneBeamLengthPercent,
      .noteStartPositionPercent = effectiveNoteStartPositionPercent(),
      .laneBeamClockUsesRenderTime = true,
      .showInvisibleNotes = context.settings.showInvisibleNotes,
      .markProcessedNotes = context.settings.markProcessedNotes,
      .judgementIndicatorEnabled =
          context.settings.judgementIndicatorEnabled,
      .judgementIndicatorY = context.settings.judgementIndicatorY,
      .judgementIndicatorWidthScale =
          context.settings.judgementIndicatorWidthScale,
      .judgementIndicatorHudMode =
          context.settings.judgementIndicatorRenderMode ==
          AppSettings::JudgementIndicatorRenderMode::Hud2D,
      .judgementIndicatorRangeMilliseconds =
          context.settings.judgementIndicatorRangeMilliseconds,
      .judgementTextY = context.settings.judgementTextY,
      .judgementCounterEnabled =
          context.settings.judgementCounterEnabled,
      .judgementCounterPosition =
          context.settings.judgementCounterPosition,
      .fastSlowCriteria =
          context.settings.judgementTimingFastSlowCriteria,
      .millisecondsCriteria =
          context.settings.judgementTimingMillisecondsCriteria,
      .gaugeBarPosition = context.settings.gaugeBarPosition,
      .touchVisualizationEnabled =
          options.touchVisualizationEnabled.value_or(
              context.settings.touchVisualizationEnabled),
      .replayGhostRenderingEnabled =
          options.replayGhostRenderingEnabled.value_or(true),
  };
  playfieldVisualStateStore->setConfiguration(playfieldPresentationConfiguration);
  presentation->configure(playfieldPresentationConfiguration);
  ownedPresentationEventFanout =
      std::make_unique<PlayfieldPresentationEventFanout>(
          *playfieldVisualStateStore, *presentation);
  presentationEventFanout = ownedPresentationEventFanout.get();
  std::string musicStopError;
  context.musicPlayer.Stop(musicStopError);
  context.jukebox.stop();
  if (!reset()) {
    return;
  }
  if (!isReplayPlayback() && !options.autoPlay) {
    const auto activeInputScopes = makeGameplayInputScopes(chart->Meta.KeyMode);
    const auto gameplayInputProfile =
        makeGameplayInputProfileWithEscapeFallback(context.inputProfile,
                                                   activeInputScopes);
    escapeHandledByInputPipeline = true;
    ownedInputHandler = std::make_unique<RhythmInputHandler>(
        this, chart->Meta, context.inputDeviceRegistry, gameplayInputProfile,
        activeInputScopes,
        [this](const input::LogicalInputTransition &transition) {
          handleLogicalInputCommand(transition);
        },
        context.settings.playAreaWidthForKeyMode(chart->Meta.KeyMode),
        LogicalGameplayRegistryPolicy{},
        [this](const auto &transition) {
          captureModernReplayInput(
              transition.physicalLane, transition.control,
              transition.hasReplayControl, transition.pressed,
              transition.replayOnly);
          if (transition.hasReplayControl &&
              (!transition.replayOnly ||
               transition.control.kind == replay::LogicalControlKind::Start ||
               transition.control.kind == replay::LogicalControlKind::Select)) {
            consumeStartSelectInput(
                {.control = transition.control,
                 .pressed = transition.pressed,
                 .timestampMicros = nowMicros()});
          }
        });
    inputHandler = ownedInputHandler.get();
    inputHandler->setDragModeEnabled(
        assist_options::isDragMode(options.assistOption));
    inputHandler->setTouchEventCallback([this](SDL_FingerID fingerIndex,
                                               ReplayTouchAction action,
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

  ownedLaneInputController = std::make_unique<RhythmLaneInputController>(
      chart, presentationEventFanout, lanePressed,
      rulesetPolicyBuild.policy->judge,
      options.longNoteMode, practiceNoteRange());
  laneInputController = ownedLaneInputController.get();

  (void)startRealtimeGameplayAuthority();

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
    pauseScreen->setHeight(options.practiceSession != nullptr ? 500 : 430);
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
          Color(28, 151, 144, 248), Color(40, 173, 164, 255),
          ui_theme::accentBorderStrong(), [this]() { closePauseMenu(); }));
      if (options.practiceSession != nullptr) {
        pauseScreen->addView(makePauseButton(
            "Restart Section", Color(57, 105, 42, 238), Color(72, 127, 51, 248),
            Color(91, 153, 61, 255), ui_theme::lime(),
            [this]() { restartCurrentPattern(); }));
        pauseScreen->addView(makePauseButton(
            "Finish Practice", ui_theme::primaryAction(),
            ui_theme::primaryActionHover(), ui_theme::primaryActionPressed(),
            ui_theme::cyan(), [this]() { finishPractice(); }));
        pauseScreen->addView(makePauseButton(
            "Exit Without Summary", Color(119, 45, 46, 238),
            Color(145, 53, 51, 248), Color(174, 64, 57, 255), ui_theme::coral(),
            [this]() { exitPracticeWithoutSummary(); }));
      } else {
        const bool canRetrySame =
            !coursePlayback && !isReplayPlayback() && !options.practiceMode &&
            chart != nullptr &&
            gameplayHasSamePatternRandomization(*chart, options);
        pauseScreen->addView(makePauseButton(
            coursePlayback ? "Restart Course"
                           : (isReplayPlayback() ? "Replay" : "Retry"),
            Color(57, 105, 42, 238), Color(72, 127, 51, 248),
            Color(91, 153, 61, 255), ui_theme::lime(), [this, canRetrySame]() {
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
        pauseScreen->addView(makePauseButton(
            "Exit", Color(119, 45, 46, 238), Color(145, 53, 51, 248),
            Color(174, 64, 57, 255), ui_theme::coral(), [this]() {
              context.jukebox.stop();
              defer(
                  [this]() {
                    if (options.practiceMode &&
                        options.returnScene != nullptr) {
                      context.sceneManager->changeScene(options.returnScene,
                                                        false);
                    } else {
                      context.sceneManager->changeScene("MainMenu");
                    }
                    return false;
                  },
                  0, true);
            }));
      }
    }

    pauseLayout->addView(pauseScreen);
  }
  pauseLayout->setVisible(false);

  /* pause button */
  pauseButton = new Button(rendering::window_width - 70, 50, 40, 40);
  addView(pauseButton);
  auto pauseText = new TextView(ui_icons::kFontAwesomeSolidPath, 24);
  pauseText->setText(ui_icons::textForCodepoint(kIconPause));
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

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  skinResetLayoutButton =
      new Button(rendering::window_width - 250, 38, 162, 52);
  addView(skinResetLayoutButton);
  auto resetLayoutText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  resetLayoutText->setText("Reset Layout");
  resetLayoutText->setAlign(TextView::CENTER);
  resetLayoutText->setVAlign(TextView::MIDDLE);
  resetLayoutText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  skinResetLayoutButton->setContentView(resetLayoutText);
  skinResetLayoutButton->setSize(162, 52);
  skinResetLayoutButton->setCornerRadius(ui_theme::controlRadius());
  skinResetLayoutButton->setBackgroundColors(
      Color(236, 253, 255, 42), Color(70, 230, 224, 88),
      Color(255, 204, 81, 120));
  skinResetLayoutButton->setBorderColors(
      ui_theme::hairlineSubtle(), ui_theme::accentBorder(),
      ui_theme::withAlpha(ui_theme::amber(), 190));
  skinResetLayoutButton->setStyledBorderWidth(1);
  skinResetLayoutButton->setOnClickListener([this]() {
    auto *coordinator =
        dynamic_cast<PlayfieldPresentationCoordinator *>(presentation);
    if (coordinator != nullptr && coordinator->resetLayoutToFit()) {
      refreshRealtimeTouchLayout();
      updateSkinResetLayoutVisibility();
    }
  });
  skinResetLayoutButton->setVisible(false);
#endif

  if (options.practiceSession != nullptr) {
    practiceRestartButton =
        new Button(rendering::window_width - 70, 110, 52, 52);
    addView(practiceRestartButton);
    auto restartText = new TextView(ui_icons::kFontAwesomeSolidPath, 24);
    restartText->setText(ui_icons::textForCodepoint(kIconRestart));
    restartText->setAlign(TextView::CENTER);
    restartText->setVAlign(TextView::MIDDLE);
    restartText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    practiceRestartButton->setContentView(restartText);
    practiceRestartButton->setCornerRadius(ui_theme::controlRadius());
    practiceRestartButton->setBackgroundColors(Color(57, 105, 42, 238),
                                               Color(72, 127, 51, 248),
                                               Color(91, 153, 61, 255));
    practiceRestartButton->setBorderColors(
        ui_theme::withAlpha(ui_theme::lime(), 150),
        ui_theme::withAlpha(ui_theme::lime(), 190),
        ui_theme::withAlpha(ui_theme::lime(), 220));
    practiceRestartButton->setStyledBorderWidth(1);
    practiceRestartButton->setOnClickListener(
        [this]() { restartCurrentPattern(); });

    practiceHudText = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    addView(practiceHudText);
    practiceHudText->setSize(620, 58);
    practiceHudText->setPositionNoLayout(24, 118);
    practiceHudText->setAlign(TextView::LEFT);
    practiceHudText->setVAlign(TextView::MIDDLE);
    practiceHudText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
    updatePracticeHud(context.jukebox.getTimeMicros());
  }
  updateSkinResetLayoutVisibility();
  refreshRealtimeTouchLayout();
  setRealtimeGameplayIngressEnabled(true);
}

bool GamePlayScene::reset() {
  stopRealtimeGameplayAuthority(false);
  realtimeGameplayAuthorityWaitingForSkinGeometry = false;
  playbackInitializationFailed = false;
  context.inputDeviceRegistry.resetGyroscopeTurntableSession();
  ownedState.reset();
  state = nullptr;
  presentation->reset();
  gameplaySkinSafeBoundsInitialized = false;
  updateSkinResetLayoutVisibility();
  playfieldProjection.reset();
  capturedPlayfieldVisualState = {};
  capturedPlayfieldProjection = {};
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
  std::string playbackRateError;
  const bool playbackRateApplied =
      context.jukebox.setPlaybackRate(options.playback, playbackRateError);
  const auto playbackInitialization =
      gameplay_startup::playbackInitializationResult(playbackRateApplied,
                                                     playbackRateError);
  if (!playbackInitialization.mayStartAttempt) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Gameplay playback rate could not be applied: %s",
                 playbackInitialization.visibleStatus.c_str());
    showPlaybackInitializationFailure(playbackInitialization.visibleStatus);
    return false;
  }
  const long long startPositionMicros = getStartPositionMicros();
  const std::optional<long long> practiceKeySoundCutoff =
      options.practiceSession != nullptr || options.practiceLeadInMicros > 0
          ? std::optional<long long>(startPositionMicros)
          : std::nullopt;
  const long long audioSeekPosition = getAudioSeekPositionMicros();
  if (options.practiceSession != nullptr) {
    const auto &configuration = options.practiceSession->configuration();
    preparationPlan = preparation::buildPracticePlan(
        *chart, context.settings.startLaneIndicatorsEnabled,
        configuration.startMicros, configuration.endMicros,
        configuration.countInBeats, configuration.playback);
  } else {
    const bool prepMetronomeEnabled = gameplay_timing::shouldApplyPrepMetronome(
        context.settings.prepMetronomeEnabled, options.practiceLeadInMicros,
        startPositionMicros);
    preparationPlan = preparation::buildNormalPlan(
        *chart, context.settings.startLaneIndicatorsEnabled,
        prepMetronomeEnabled, audioSeekPosition, startPositionMicros,
        std::nullopt, options.playback);
  }
  context.jukebox.schedule(
      *chart, options.autoKeySound, isCancelled, practiceKeySoundCutoff,
      preparationPlan.metronome.enabled ? &preparationPlan.metronome : nullptr,
      options.clubMode);
  if (options.replayData != nullptr && !options.autoKeySound) {
    std::optional<gameplay::GameplayTimeRange> allowedRange;
    std::optional<gameplay::GameplayTimeRange> preparationRange;
    if (const auto range = practiceNoteRange(); range.has_value()) {
      allowedRange = {.startMicros = range->startMicros,
                      .endMicros = range->endMicros};
      preparationRange = {
          .startMicros = getGameplayTimeMicros(
              preparationPlan.laneIndicator.startTimeMicros),
          .endMicros = getGameplayTimeMicros(
              preparationPlan.laneIndicator.endTimeMicros),
      };
    }
    const auto definition =
        gameplay::buildGameplayDefinition(*chart, options.longNoteMode);
    const auto replayKeysounds =
        buildReplayKeysoundSchedule(definition, options.replayData->events,
                                    getAudioOffsetMicros(), allowedRange,
                                    preparationRange);
    context.jukebox.appendScheduledAudioEvents(replayKeysounds);
  }
  playfieldFrameSerial = 0;
  playfieldLaneCoverResetPending = false;
  if (startSelectControl.has_value()) {
    startSelectControl->reset();
  }
  if (playfieldVisualStateStore != nullptr) {
    playfieldVisualStateStore->resetModel(playfieldChartVisualModel);
    if (options.replayData != nullptr) {
      playfieldVisualStateStore->setReplayTouchSamples(
          options.replayData->touchSamples);
    }
    playfieldVisualStateStore->setSceneStartMicros(
        getVisualTimeMicros(getGameplayTimeMicros(
            preparationPlan.skinAnimationStartTimeMicros())));
    playfieldVisualStateStore->setPlayStartMicros(getStartPositionMicros());
    playfieldVisualStateStore->clearLiveTouchPoints();
  }
  currentGameplayBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  ownedState = std::make_unique<RhythmState>(chart, false,
                                             rulesetPolicyBuild.policy->gauge);
  state = ownedState.get();
  const bool courseReplayPlayback = isReplayPlayback() && isCoursePlayback() &&
                                    options.courseSession != nullptr &&
                                    options.courseSession->courseReplayPlayback;
  const GaugeType initialGaugeType =
      courseReplayPlayback
          ? options.gaugeType
          : (isReplayPlayback() ? options.replayData->initialGaugeType
                                : options.gaugeType);
  const GaugeProfile gaugeProfile = isReplayPlayback() && !isCoursePlayback()
                                        ? GaugeProfile::Standard
                                        : options.gaugeProfile;
  const GaugeAutoShiftMode gaugeAutoShift =
      courseReplayPlayback
          ? options.gaugeAutoShift
          : (isReplayPlayback() ? options.replayData->gaugeAutoShift
                                : options.gaugeAutoShift);
  state->configureGauge(initialGaugeType, gaugeAutoShift, gaugeProfile,
                        options.gaugeAutoShiftLowerBound);
  if (options.startingGaugePercent.has_value()) {
    state->setStartingGaugePercent(*options.startingGaugePercent);
  }
  if (options.courseSession != nullptr &&
      options.courseSession->courseCarriedGauge() != nullptr) {
    GaugeStateSnapshot carriedGauge =
        *options.courseSession->courseCarriedGauge();
    carriedGauge.gaugeProfile = state->gaugeProfile;
    state->restoreGaugeState(carriedGauge);
  }
  if (isCoursePlayback()) {
    state->combo = options.courseSession->courseCarriedCombo();
    state->maxCombo = options.courseSession->courseMaximumCombo();
    courseStageInitialGauge = state->gaugeSnapshot();
  } else {
    courseStageInitialGauge.reset();
  }
  const std::string assistOption = isReplayPlayback()
                                       ? options.replayData->assistOption
                                       : options.assistOption;
  state->setAssistClearMark(clear_policy::assistClearMarkRequired(
      assist_options::isEnabled(assistOption), options.playback));
  initializeStartPositionState();
  configurePacemakerTarget();
  updatePacemakerStatus();
  resetHellChargeGaugeTracking(
      getGameplayTimeMicros(context.jukebox.getTimeMicros()));
  state->isPlaying = true;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  const long long initialRawSongTimeMicros = context.jukebox.getTimeMicros();
  const long long initialGameplayTimeMicros =
      getGameplayTimeMicros(initialRawSongTimeMicros);
  capturePlayfieldVisualState(
      initialGameplayTimeMicros,
      getVisualTimeMicros(initialGameplayTimeMicros),
      preparationIndicatorActive(initialRawSongTimeMicros));
  acquireGameplaySkinForAttempt();
  if (playbackInitializationFailed) {
    return false;
  }
  updateSkinResetLayoutVisibility();
#endif
  context.jukebox.play(preparationPlan.playbackStartTimeMicros);
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
  if (options.practiceSession != nullptr) {
    options.practiceSession->beginAttempt();
  }
  updatePracticeHud(context.jukebox.getTimeMicros());
  updateGaugeStatusText();
  if (gameplay::shouldAttemptRealtimeGameplayReset(
          laneInputController != nullptr, inputHandler != nullptr,
          options.autoPlay)) {
    (void)startRealtimeGameplayAuthority();
  }
  return true;
}

void GamePlayScene::showPlaybackInitializationFailure(
    const std::string &message) {
  playbackInitializationFailed = true;
  escapeHandledByInputPipeline = true;
  if (state != nullptr) {
    state->isPlaying = false;
    state->isEnding = true;
  }
  context.jukebox.stop();
  if (inputHandler != nullptr) {
    inputHandler->stopListen();
  }
  ownedInputHandler.reset();
  inputHandler = nullptr;
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(false);
  }
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setVisible(false);
  }
  if (skinResetLayoutButton != nullptr) {
    skinResetLayoutButton->setVisible(false);
  }
  if (playbackFailureLayout != nullptr) {
    return;
  }

  playbackFailureLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(playbackFailureLayout);
  playbackFailureLayout->setFlexDirection(FlexDirection::Column);
  playbackFailureLayout->setAlignItems(YGAlignCenter);
  playbackFailureLayout->setJustifyContent(YGJustifyCenter);
  playbackFailureLayout->setGap(18);
  playbackFailureLayout->setPadding(Edge::All, 32);
  playbackFailureLayout->setBackgroundColor(Color(2, 5, 9, 255));

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 38);
  title->setText("PLAYBACK UNAVAILABLE");
  title->setAlign(TextView::CENTER);
  title->setVAlign(TextView::MIDDLE);
  title->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  title->setSize(720, 64);
  playbackFailureLayout->addView(title);

  auto *detail = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  detail->setText(message);
  detail->setAlign(TextView::CENTER);
  detail->setVAlign(TextView::MIDDLE);
  detail->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  detail->setSize(820, 84);
  playbackFailureLayout->addView(detail);

  auto *returnButton = new Button();
  auto *returnText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  returnText->setText("Return");
  returnText->setAlign(TextView::CENTER);
  returnText->setVAlign(TextView::MIDDLE);
  returnText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  returnButton->setContentView(returnText);
  returnButton->setSize(320, 64);
  returnButton->setCornerRadius(ui_theme::controlRadius());
  returnButton->setBackgroundColors(ui_theme::primaryAction(),
                                    ui_theme::primaryActionHover(),
                                    ui_theme::primaryActionPressed());
  returnButton->setBorderColors(ui_theme::accentBorder(),
                                ui_theme::accentBorderStrong(),
                                ui_theme::withAlpha(ui_theme::amber(), 210));
  returnButton->setStyledBorderWidth(1);

  const bool requestedReturnSceneIsLive =
      options.returnScene != nullptr && context.sceneManager != nullptr &&
      context.sceneManager->backgroundScenes.contains(options.returnScene);
  const auto returnTarget =
      gameplay_startup::failureReturnTarget(requestedReturnSceneIsLive);
  returnButton->setOnClickListener([this, returnTarget]() {
    defer(
        [this, returnTarget]() {
          if (context.sceneManager == nullptr) {
            return false;
          }
          if (returnTarget ==
                  gameplay_startup::FailureReturnTarget::RequestedScene &&
              options.returnScene != nullptr &&
              context.sceneManager->backgroundScenes.contains(
                  options.returnScene)) {
            context.sceneManager->changeScene(options.returnScene, false);
          } else {
            context.sceneManager->changeScene("MainMenu", false);
          }
          return false;
        },
        0, true);
  });
  playbackFailureLayout->addView(returnButton);
}

void GamePlayScene::showPauseMenu(bool pausePlayback) {
  setRealtimeGameplayIngressEnabled(false);
  if (gameplay::shouldSuspendRealtimeGameplayForPause(pausePlayback) &&
      realtimeGameplayAuthorityActive() &&
      !realtimeGameplaySession->worker->suspend()) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime gameplay worker failed to suspend");
  }
  if (pausePlayback) {
    context.jukebox.pause();
  }
  if (playfieldVisualStateStore != nullptr) {
    playfieldVisualStateStore->clearLiveTouchPoints();
  }
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(true);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(false);
  }
  updateSkinResetLayoutVisibility();
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setVisible(false);
  }
  resetCoursePauseHold();
}

void GamePlayScene::closePauseMenu() {
  if (!isCoursePlayback() && context.jukebox.isPaused()) {
    context.jukebox.resume();
  }
  if (realtimeGameplayAuthorityActive() &&
      !realtimeGameplaySession->worker->resume()) {
    SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                 "Realtime gameplay worker failed to resume");
    return;
  }
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  updateSkinResetLayoutVisibility();
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setVisible(true);
  }
  resetCoursePauseHold();
  // Publish the post-pause overlay geometry and any resize that occurred
  // while detached before raw touch ingress becomes observable again.
  refreshRealtimeTouchLayout();
  setRealtimeGameplayIngressEnabled(true);
}

void GamePlayScene::togglePauseMenuFromInput() {
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

void GamePlayScene::handleLogicalInputCommand(
    const input::LogicalInputTransition &transition) {
  if (!transition.pressed) {
    return;
  }
  switch (transition.action.kind) {
  case input::LogicalActionKind::Pause:
    togglePauseMenuFromInput();
    break;
  case input::LogicalActionKind::Retry:
    if (isCoursePlayback()) {
      (void)restartCourseFromBeginning();
    } else {
      restartCurrentPattern();
    }
    break;
  case input::LogicalActionKind::LaneCoverIncrease:
    adjustLaneCoverFromInput(1);
    break;
  case input::LogicalActionKind::LaneCoverDecrease:
    adjustLaneCoverFromInput(-1);
    break;
  case input::LogicalActionKind::Start:
  case input::LogicalActionKind::Select:
  case input::LogicalActionKind::Lane:
  case input::LogicalActionKind::ScratchClockwise:
  case input::LogicalActionKind::ScratchCounterClockwise:
    break;
  }
}

void GamePlayScene::consumeStartSelectInput(
    const gameplay::StartSelectControlInput &input) {
  if (!startSelectControl.has_value() || isReplayPlayback() ||
      options.autoPlay) {
    return;
  }
  const bool noteEnd = state != nullptr && state->isEnding;
  applyStartSelectControlActions(startSelectControl->apply(
      input.control, input.pressed, input.timestampMicros, {.noteEnd = noteEnd}));
}

void GamePlayScene::applyStartSelectControlActions(
    const std::vector<gameplay::StartSelectControlAction> &actions) {
  for (const auto &action : actions) {
    switch (action.kind) {
    case gameplay::StartSelectControlActionKind::AdjustHispeed:
      if (!courseNoSpeed() && action.delta != 0) {
        playfieldHispeedMultiplier = std::clamp(
            playfieldHispeedMultiplier + 0.25F * static_cast<float>(action.delta),
            0.01F, 19.99F);
        context.settings.gameplayHispeedMultiplier = playfieldHispeedMultiplier;
        refreshRuntimePresentationConfiguration();
        (void)context.saveSettings();
      }
      break;
    case gameplay::StartSelectControlActionKind::AdjustDuration:
      if (!courseNoSpeed() && action.delta != 0) {
        context.settings.visibleTimeGreenNumber = std::clamp(
            context.settings.visibleTimeGreenNumber + action.delta,
            AppSettings::kMinVisibleTimeGreenNumber,
            AppSettings::kMaxVisibleTimeGreenNumber);
        refreshRuntimePresentationConfiguration();
        (void)context.saveSettings();
      }
      break;
    case gameplay::StartSelectControlActionKind::AdjustLaneCover:
      if (!courseNoSpeed() && action.delta != 0) {
        const float next = std::clamp(
            playfieldLaneCoverPercentExact +
                static_cast<float>(action.delta) * 0.1F,
            static_cast<float>(AppSettings::kMinNoteStartPositionPercent),
            static_cast<float>(AppSettings::kMaxNoteStartPositionPercent));
        const int nextPercent = static_cast<int>(std::lround(next));
        if (next != playfieldLaneCoverPercentExact) {
          playfieldLaneCoverPercentExact = next;
          context.settings.noteStartPositionPercent = nextPercent;
          playfieldLaneCoverPercent = nextPercent;
          refreshLaneCoverHispeedFactor();
          playfieldLaneCoverResetPending = context.settings.hispeedAutoAdjust;
          refreshRuntimePresentationConfiguration();
          appendReplayLaneCoverEvent(
              nextPercent, getGameplayTimeMicros(context.jukebox.getTimeMicros()),
              context.settings.hispeedAutoAdjust);
          (void)context.saveSettings();
        }
      }
      break;
    case gameplay::StartSelectControlActionKind::ToggleLaneCover:
      if (!courseNoSpeed()) {
        playfieldLaneCoverEnabled = !playfieldLaneCoverEnabled;
        context.settings.laneCoverEnabled = playfieldLaneCoverEnabled;
        (void)context.saveSettings();
      }
      break;
    case gameplay::StartSelectControlActionKind::ToggleLiftHiddenTarget:
      // Aso has no configurable Lift/Hidden planes yet.  The control-state
      // edge is intentionally retained here so its future renderer support
      // can use the same source-faithful input stream.
      break;
    case gameplay::StartSelectControlActionKind::Exit:
      abortPlayFromStartSelectControl();
      return;
    }
  }
}

void GamePlayScene::refreshRuntimePresentationConfiguration() {
  if (playfieldVisualStateStore == nullptr || presentation == nullptr) {
    return;
  }
  playfieldPresentationConfiguration.visibleTimeGreenNumber =
      effectiveVisibleTimeGreenNumber();
  playfieldPresentationConfiguration.hispeedMultiplier =
      playfieldHispeedMultiplier;
  playfieldPresentationConfiguration.laneCoverHispeedFactor =
      playfieldLaneCoverHispeedFactor;
  playfieldVisualStateStore->setConfiguration(
      playfieldPresentationConfiguration);
  presentation->configure(playfieldPresentationConfiguration);
}

void GamePlayScene::abortPlayFromStartSelectControl() {
  if (state == nullptr || state->isEnding || resultTransitionScheduled) {
    return;
  }
  if (options.practiceSession != nullptr) {
    finishPractice();
    return;
  }
  state->isEnding = true;
  context.jukebox.stop();
  stopRealtimeGameplayAuthority(true);
  finishReplayRecording();
  recordedAttemptCompleted = options.practiceMode;
  publishPracticeGhost();
  if (isCoursePlayback()) {
    if (!usesModernCourseContinuation()) {
      options.courseSession->carriedGauge = state->gaugeSnapshot();
      options.courseSession->carriedCombo = state->combo;
      options.courseSession->maxCombo =
          std::max(options.courseSession->maxCombo, state->maxCombo);
    }
    options.courseSession->recordResult(chart->Meta, *state);
    if (!options.courseSession->courseReplayPlayback) {
      options.courseSession->recordStageProvenance(
          options.courseSession->currentIndex, attemptProvenance);
    }
    if (shouldRecordReplay()) {
      options.courseSession->recordReplayStage(recordedReplay);
    }
  }
  scheduleResultTransition(0);
}

void GamePlayScene::adjustLaneCoverFromInput(int deltaPercent) {
  const long long chartTimeMicros =
      getGameplayTimeMicros(context.jukebox.getTimeMicros());
  if (!practiceInputAllowed(chartTimeMicros)) {
    return;
  }
  if (courseNoSpeed() || deltaPercent == 0) {
    return;
  }
  const int previous = context.settings.noteStartPositionPercent;
  const int next = std::clamp(previous + deltaPercent,
                              AppSettings::kMinNoteStartPositionPercent,
                              AppSettings::kMaxNoteStartPositionPercent);
  if (next == previous) {
    return;
  }
  context.settings.noteStartPositionPercent = next;
  playfieldLaneCoverPercent = next;
  playfieldLaneCoverPercentExact = static_cast<float>(next);
  refreshLaneCoverHispeedFactor();
  playfieldLaneCoverResetPending = context.settings.hispeedAutoAdjust;
  refreshRuntimePresentationConfiguration();
  floatingLaneCoverSettingsDirty = true;
  appendReplayLaneCoverEvent(next, chartTimeMicros,
                              context.settings.hispeedAutoAdjust);
  persistFloatingLaneCoverSettings();
}

void GamePlayScene::restartCurrentPattern() {
  if (options.practiceSession != nullptr) {
    options.practiceSession->abandonAttempt();
  }
  if (pauseLayout != nullptr) {
    pauseLayout->setVisible(false);
  }
  if (pauseButton != nullptr) {
    pauseButton->setVisible(true);
  }
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setVisible(true);
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
    session->stageProvenance.clear();
  }
  session->carriedGauge.reset();
  session->carriedCombo = 0;
  session->maxCombo = 0;
  session->courseScoreSaved = false;
  session->resetModernCourseAttempt();
  session->playOption.reset();
  session->playOptionSeed.reset();
  session->playOption2.reset();
  session->playOption2Seed.reset();
  context.jukebox.stop();
  defer(
      [this]() {
        const bool started = options.courseSession != nullptr &&
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

std::optional<NoteTimeRange> GamePlayScene::practiceNoteRange() const {
  return practiceAllowedNoteRange(options);
}

bool GamePlayScene::practiceInputAllowed(long long chartTimeMicros) const {
  const auto range = practiceNoteRange();
  return !range.has_value() || chartTimeMicros < range->endMicros;
}

bool GamePlayScene::practiceReplayEventAllowed(const ReplayEvent &event) const {
  const auto range = practiceNoteRange();
  if (!range.has_value()) {
    return true;
  }
  return replayEventAllowedForPlayback(
      event,
      gameplay::GameplayTimeRange{.startMicros = range->startMicros,
                                  .endMicros = range->endMicros},
      gameplay::GameplayTimeRange{
          .startMicros = getGameplayTimeMicros(
              preparationPlan.laneIndicator.startTimeMicros),
          .endMicros = getGameplayTimeMicros(
              preparationPlan.laneIndicator.endTimeMicros),
      });
}

bool GamePlayScene::preparationIndicatorActive(
    long long rawSongTimeMicros) const {
  return preparationPlan.indicatorVisibleAt(rawSongTimeMicros);
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
  return resultCapturePolicy().recordReplay;
}

bool GamePlayScene::shouldPersistRecordedReplay() const {
  return resultCapturePolicy().persistReplay;
}

bool GamePlayScene::usesModernCourseContinuation() const {
  if (!isCoursePlayback() || options.courseSession == nullptr ||
      options.courseSession->courseReplayPlayback) {
    return false;
  }
  const auto policy = resultCapturePolicy();
  return gameplay_startup::completedAttemptPersistenceRoute(
             policy.persistScore && policy.persistReplay, true) ==
         gameplay_startup::CompletedAttemptPersistenceRoute::ModernCourseFile;
}

practice::ResultCapturePolicy GamePlayScene::resultCapturePolicy() const {
  return practice::resultCapturePolicy({
      .autoPlay = options.autoPlay,
      .practice = options.practiceMode || options.practiceSession != nullptr,
      .replayPlayback = isReplayPlayback(),
      .coursePlayback = isCoursePlayback(),
  });
}

void GamePlayScene::configurePacemakerTarget() {
  stopBestReplayLoad();
  activePacemakerTarget = {};
  activeBestScoreTarget = {};
  activePacemakerBest.reset();
  activeReplayPacemakerPreviousBest.reset();

  const std::string selected =
      pacemaker::normalizeTargetId(options.pacemakerTarget);
  if (chart == nullptr || options.autoPlay || options.practiceMode ||
      isCoursePlayback()) {
    return;
  }

  if (isReplayPlayback()) {
    if (options.replayData == nullptr || options.replayData->autoPlay) {
      return;
    }

    const auto previousBest =
        result_presentation::previousBestForReplayChart(
            context.scoreRepository, chart->Meta, *options.replayData);
    activeReplayPacemakerPreviousBest = previousBest;
    if (previousBest.has_value()) {
      activePacemakerBest =
          result_presentation::scoreBestSnapshotFromPreviousBest(*previousBest);
    }
  } else {
    activePacemakerBest = context.scoreRepository.LoadBestScore(chart->Meta);
  }

  if (activePacemakerBest.has_value()) {
    activeBestScoreTarget =
        pacemaker::targetFromBestSnapshot(*chart, *activePacemakerBest);
    if (activePacemakerBest->attemptId.has_value()) {
      startBestReplayLoad(*activePacemakerBest->attemptId,
                          chart->Meta.BmsPath);
    }
  }

  if (options.gbattleRecordData != nullptr) {
    activePacemakerTarget =
        gbattle::targetFromRecord(*chart, *options.gbattleRecordData);
    return;
  }

  if (selected == pacemaker::kTargetOff) {
    return;
  }

  // Beatoraja's BMSPlayer passes a decoded ghost for the personal best, but
  // explicitly passes null for the selected target ghost.  Its target score
  // therefore stays proportional to passed notes, including when BEST is the
  // selected pacemaker; do not substitute our persisted-best replay here.
  activePacemakerTarget = pacemaker::targetFromSelection(
      *chart, selected, activePacemakerBest, nullptr);
}

void GamePlayScene::startBestReplayLoad(
    std::string attemptId, std::filesystem::path chartPath) {
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  bestReplayLoadCancelled = cancelled;
  bestReplayLoadThread = std::jthread(
      [this, cancelled, attemptId = std::move(attemptId),
       chartPath = std::move(chartPath)](std::stop_token stopToken) {
        auto resolver = replay::makeRuntimeBestReplayResolver(
            context.replayRepository);
        auto loaded = resolver.load(attemptId, chartPath, *cancelled);
        if (stopToken.stop_requested() || cancelled->load() ||
            loaded == nullptr) {
          return;
        }
        std::lock_guard<std::mutex> lock(bestReplayLoadMutex);
        if (!cancelled->load()) {
          pendingBestReplay = std::move(loaded);
        }
      });
}

void GamePlayScene::applyPendingBestReplay() {
  std::shared_ptr<ReplayData> loaded;
  {
    std::lock_guard<std::mutex> lock(bestReplayLoadMutex);
    loaded = std::move(pendingBestReplay);
  }
  if (loaded == nullptr || chart == nullptr) {
    return;
  }

  if (activePacemakerBest.has_value()) {
    // The saved best maps to ScoreDataProperty.bestGhost.  This is the only
    // ghost BMSPlayer supplies to ScoreDataProperty during gameplay.
    activeBestScoreTarget =
        pacemaker::targetFromBestSnapshot(*chart, *activePacemakerBest,
                                          loaded.get());
  }
}

void GamePlayScene::stopBestReplayLoad() {
  if (bestReplayLoadCancelled != nullptr) {
    bestReplayLoadCancelled->store(true, std::memory_order_release);
  }
  if (bestReplayLoadThread.joinable()) {
    bestReplayLoadThread.request_stop();
    bestReplayLoadThread.join();
  }
  {
    std::lock_guard<std::mutex> lock(bestReplayLoadMutex);
    pendingBestReplay.reset();
  }
}

void GamePlayScene::updatePacemakerStatus() {
  // Pacemaker authority is captured atomically in PlayfieldAuthorityUpdate.
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
  auto replayChart = session->takePreparedCourseChart(session->currentIndex);
  if (replayChart == nullptr) {
    replayChart = play_options::prepareReplayChart(
        stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  }
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
  const ReplayData *retrySetup =
      session->courseRetrySameStageSetup(session->currentIndex);
  std::unique_ptr<bms_parser::Chart> nextChart =
      session->takePreparedCourseChart(session->currentIndex);
  if (nextChart == nullptr) {
    try {
      nextChart = retrySetup != nullptr
                      ? play_options::prepareReplayChart(
                            nextMeta->BmsPath, *retrySetup, parseCancelled)
                      : play_options::parseChart(nextMeta->BmsPath,
                                                 parseCancelled, "course");
    } catch (const std::exception &e) {
      SDL_Log("Course parse failed %s: %s",
              fspath_to_utf8(nextMeta->BmsPath).c_str(), e.what());
      archive_file::appendDebugLogLine(
          "Course parse exception: " + fspath_to_utf8(nextMeta->BmsPath) +
          ": " + e.what());
      return false;
    }
  }
  if (nextChart == nullptr || parseCancelled) {
    return false;
  }
  play_options::PlayOptionReplayInfo playInfo;
  if (retrySetup != nullptr) {
    session->applyReplayStagePlayOptions(*retrySetup);
    playInfo = {.option = retrySetup->playOption,
                .seed = retrySetup->playOptionSeed,
                .option2 = retrySetup->playOption2,
                .seed2 = retrySetup->playOption2Seed};
  } else {
    applyCourseConstraintsToChart(*nextChart, session->constraints);
    playInfo = play_options::applySelectedPlayOptions(
        *nextChart, session->requestedPlayOption);
    applyEffectiveLongNoteModeToChart(*nextChart, options.longNoteMode);
    session->playOption = playInfo.option;
    session->playOptionSeed = playInfo.seed;
    session->playOption2 = playInfo.option2;
    session->playOption2Seed = playInfo.seed2;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*nextChart, true, parseCancelled);
  if (parseCancelled) {
    return false;
  }

  StartOptions nextOptions =
      retrySetup != nullptr
          ? makeCourseRetrySameStageStartOptions(session, *retrySetup)
          : StartOptions{};
  if (retrySetup == nullptr) {
    nextOptions.startPosition = 0;
    nextOptions.autoKeySound = session->autoKeySound;
    nextOptions.autoPlay = false;
    nextOptions.gaugeType = session->gaugeType;
    nextOptions.gaugeProfile = session->gaugeProfile;
    nextOptions.gaugeAutoShift = session->gaugeAutoShift;
    nextOptions.gaugeAutoShiftLowerBound =
        session->gaugeAutoShiftLowerBound;
    nextOptions.playOption = playInfo.option;
    nextOptions.playOptionSeed = playInfo.seed;
    nextOptions.playOption2 = playInfo.option2;
    nextOptions.playOption2Seed = playInfo.seed2;
    nextOptions.longNoteMode = options.longNoteMode;
    nextOptions.assistOption = session->assistOption;
    nextOptions.playback = course_rules::kRequiredPlaybackRate;
    nextOptions.clubMode = options.clubMode;
    nextOptions.courseSession = session;
    nextOptions.courseConstraints = session->constraints;
    nextOptions.ruleset = session->ruleset;
    nextOptions.requiredRulesetDescriptor = session->rulesetDescriptor;
    nextOptions.ownsChart = true;
  }

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
  resultPersistenceOptions = {};
  resultPersistenceAttemptId.clear();
  resultPersistenceAttemptCreationTried = false;
  modernReplayInputRecorder.reset();
  completedModernReplayInput.reset();
  modernReplayCaptureDiagnostic.clear();
  practiceGhostPublished = false;
  recordedAttemptCompleted = false;
  lastRecordedTouchSamples.clear();
  const auto capturePolicy = resultCapturePolicy();
  if (gameplay_startup::completedAttemptPersistenceRoute(
          capturePolicy.persistScore && capturePolicy.persistReplay,
          isCoursePlayback()) !=
      gameplay_startup::CompletedAttemptPersistenceRoute::None) {
    modernReplayInputRecorder = std::make_unique<replay::ReplayInputRecorder>();
  }
  analyticsReplay = {};
  if (capturePolicy.captureAnalytics) {
    analyticsReplay.autoPlay = options.autoPlay;
    analyticsReplay.chartMeta = chart->Meta;
    analyticsReplay.provenance = attemptProvenance;
    analyticsReplay.events.reserve(
        static_cast<size_t>(std::max(0, chart->Meta.TotalNotes)) * 2);
  }
  if (!capturePolicy.recordReplay) {
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
  recordedReplay.provenance = attemptProvenance;
  recordedReplay.initialGaugeType = options.gaugeType;
  recordedReplay.gaugeAutoShift = options.gaugeAutoShift;
  recordedReplay.gaugeAutoShiftLowerBound = options.gaugeAutoShiftLowerBound;
  recordedReplay.finalScore = 0;
  recordedReplay.finalGauge = state != nullptr ? state->currentGauge : 0.0f;
  recordedReplay.clearType = kClearTypeFailedRank;
  recordedReplay.events.reserve(
      static_cast<size_t>(std::max(0, chart->Meta.TotalNotes)) * 2);
  recordedReplay.touchSamples.reserve(1024);
  recordedReplay.laneCoverEvents.reserve(128);
  appendReplayLaneCoverEvent(
      effectiveNoteStartPositionPercent(),
      getGameplayTimeMicros(preparationPlan.playbackStartTimeMicros), false);
}

void GamePlayScene::captureModernReplayInput(
    int physicalLane, replay::LogicalControl control, bool hasReplayControl,
    bool pressed, bool replayOnly) {
  if (realtimeGameplayAuthorityActive()) {
    (void)realtimeGameplaySession->emitLegacyApplied(
        physicalLane, control, hasReplayControl, pressed, replayOnly);
    return;
  }
  if (modernReplayInputRecorder == nullptr || !hasReplayControl) {
    return;
  }
  std::string diagnostic;
  if (!modernReplayInputRecorder->recordSongTime(
          getGameplayTimeMicros(context.jukebox.getTimeMicros()), control,
          pressed, diagnostic, replayOnly)) {
    modernReplayCaptureDiagnostic = diagnostic.empty()
                                        ? "Raw replay input capture failed."
                           : std::move(diagnostic);
  }
}

void GamePlayScene::finishReplayRecording() {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }

  recordedReplay.finalScore = state->getScore();
  recordedReplay.maxCombo = state->maxCombo;
  recordedReplay.finalGauge = state->currentGauge;
  recordedReplay.clearType = state->getClearTypeRank();
  const int totalNotes =
      chart != nullptr ? std::max(0, chart->Meta.TotalNotes) : 0;
  const bool fullCombo =
      totalNotes > 0 && state->comboBreak == 0 && state->maxCombo >= totalNotes;
  recordedReplay.clearType = clear_policy::fullComboRankForPlayback(
      recordedReplay.clearType, fullCombo, options.playback);
}

GamePlayScene::CompletedModernReplayCapture
GamePlayScene::completeModernReplayCapture() {
  CompletedModernReplayCapture capture;
  std::int64_t completionSongTimeMicros = std::max<std::int64_t>(
      0, getGameplayTimeMicros(context.jukebox.getTimeMicros()));
  capture.touchSamples.reserve(recordedReplay.touchSamples.size());
  for (const auto &sample : recordedReplay.touchSamples) {
    capture.touchSamples.push_back({.action = modernTouchAction(sample.action),
                                    .fingerId = sample.fingerId,
                                    .songTimeMicros = sample.songTimeMicros,
                                    .x = sample.x,
                                    .y = sample.y});
  }
  capture.laneCoverEvents.reserve(recordedReplay.laneCoverEvents.size());
  for (const auto &event : recordedReplay.laneCoverEvents) {
    capture.laneCoverEvents.push_back({
        .songTimeMicros = event.songTimeMicros,
        .noteStartPositionPercent = event.noteStartPositionPercent,
        .resetVisibleTimeReference = event.resetVisibleTimeReference,
    });
  }
  std::string auxiliaryDiagnostic;
  const bool auxiliaryCaptureAvailable =
      replay::normalizeLocalReplayAuxiliaryStreams(
          capture.touchSamples, capture.laneCoverEvents,
          auxiliaryDiagnostic);
  if (!auxiliaryCaptureAvailable) {
    modernReplayCaptureDiagnostic =
        auxiliaryDiagnostic.empty()
            ? "Raw replay auxiliary capture normalization failed."
            : std::move(auxiliaryDiagnostic);
  }
  capture.timeBounds = replay::replayCaptureTimeBounds(
      {.completionSongTimeMicros = completionSongTimeMicros}, {},
      capture.touchSamples, capture.laneCoverEvents);
  if (!completedModernReplayInput.has_value() &&
      modernReplayInputRecorder != nullptr) {
    std::string diagnostic;
    completedModernReplayInput =
        modernReplayInputRecorder->finish(capture.timeBounds, diagnostic);
    if (!completedModernReplayInput.has_value()) {
      modernReplayCaptureDiagnostic = diagnostic.empty()
                                          ? "Raw replay input capture failed."
                                          : std::move(diagnostic);
    }
    modernReplayInputRecorder.reset();
  }
  if (completedModernReplayInput.has_value()) {
    std::string diagnostic;
    auto normalized = replay::normalizeReplayInput(
        *completedModernReplayInput, capture.timeBounds, diagnostic);
    if (!normalized.has_value()) {
      modernReplayCaptureDiagnostic =
          diagnostic.empty() ? "Raw replay input normalization failed."
                             : std::move(diagnostic);
    }
    completedModernReplayInput = std::move(normalized);
  }
  if (!auxiliaryCaptureAvailable) {
    completedModernReplayInput.reset();
  }
  capture.acceptedInput = completedModernReplayInput;
  if (capture.acceptedInput.has_value()) {
    capture.timeBounds = replay::replayCaptureTimeBounds(
        capture.timeBounds, *capture.acceptedInput, capture.touchSamples,
        capture.laneCoverEvents);
  }
  return capture;
}

void GamePlayScene::recordModernCourseStage(
    const CompletedModernReplayCapture &capture) {
  auto session = options.courseSession;
  if (session == nullptr || session->courseReplayPlayback || chart == nullptr ||
      state == nullptr || !session->validCurrentIndex()) {
    return;
  }
  if (session->modernCourseAttemptId.empty()) {
    session->modernCourseAttemptId = uuid::generateV4();
  }

  const int longNoteMode =
      scoreLongNoteModeForClearLamp(chart->Meta, options.longNoteMode);
  std::string diagnostic;
  auto result = result_persistence::captureModernCourseStageResult(
      static_cast<int>(session->currentIndex), chart->Meta, *state,
      attemptProvenance, longNoteMode, diagnostic);
  if (!result) {
    session->modernCourseDiagnostic =
        diagnostic.empty() ? "Modern course stage result capture failed."
                           : std::move(diagnostic);
    return;
  }

  const int initialLaneCover =
      recordedReplay.laneCoverEvents.empty()
          ? effectiveNoteStartPositionPercent()
          : recordedReplay.laneCoverEvents.front().noteStartPositionPercent;
  const replay::LocalReplaySetupFacts setupFacts{
      .chart = {.md5 = result->score.chartMd5,
                .sha256 = result->score.chartSha256,
                .keyMode = result->keyMode},
      .longNoteMode =
          result_persistence::replaySetupLongNoteMode(result->score)
              .value_or(-1),
      .hasUndefinedLongNotes = chartContainsUndefinedLongNote(*chart),
      .initialLaneCoverPercent = initialLaneCover,
      .laneCoverEnabled = initialLaneCover > 0,
  };
  auto setup = replay::captureLocalReplaySetup(
      setupFacts, result->score.provenance, diagnostic);

  replay::CourseReplayStageCapture replayCapture{
      .timeBounds = capture.timeBounds};
  if (capture.acceptedInput.has_value() && setup.has_value()) {
    replayCapture.playback = replay::ReplayPlaybackData{
        .setup = *setup,
        .input = *capture.acceptedInput,
        .touchSamples = capture.touchSamples,
        .laneCoverEvents = capture.laneCoverEvents,
    };
  }
  if (!capture.acceptedInput.has_value() || !setup.has_value()) {
    if (!session->modernCourseDiagnostic.empty()) {
      session->modernCourseDiagnostic += "; ";
    }
    session->modernCourseDiagnostic +=
        diagnostic.empty() ? "Course BRD capture was unavailable." : diagnostic;
  }
  std::optional<replay::CourseContinuationState> currentContinuation =
      session->modernCourseContinuation;
  if (!currentContinuation.has_value() && courseStageInitialGauge.has_value()) {
    const auto started = replay::startCourseContinuation(
        {.totalStages = session->entries.size(),
         .initialGauge = *courseStageInitialGauge,
         .constraints = {
             .beatorajaConstraintIds =
                 beatorajaCourseConstraintIdsFromJson(session->constraintJson),
             .longNoteMode = session->longNoteMode,
         }});
    if (started.ready()) {
      currentContinuation = std::move(started.state);
    }
  }

  std::optional<replay::CourseContinuationState> advancedContinuation;
  if (currentContinuation.has_value()) {
    const auto advanced = replay::advanceCourseContinuation(
        *currentContinuation,
        {.stageIndex = session->currentIndex,
         .score = result->score.score,
         .maximumScore = result->score.maxScore,
         .combo = state->combo,
         .maximumCombo = result->score.maxCombo,
         .gauge = state->gaugeSnapshot(),
         .adoptedGauge = result->adoptedGaugeType,
         .restMicrosAfterStage = 0,
         .setup = setup});
    if (advanced.advanced()) {
      advancedContinuation = std::move(advanced.state);
    }
  }

  if (!session->recordModernCourseStage(std::move(*result),
                                        std::move(replayCapture))) {
    session->modernCourseDiagnostic =
        "Modern course stage capture was not contiguous.";
    return;
  }
  if (!advancedContinuation.has_value()) {
    session->modernCourseDiagnostic =
        "Modern course continuation rejected the completed stage.";
    session->modernCourseContinuation.reset();
    return;
  }
  session->adoptModernCourseContinuation(
      std::move(*advancedContinuation));
}

void GamePlayScene::publishPracticeGhost() {
  const auto capturePolicy = resultCapturePolicy();
  if (!capturePolicy.publishPracticeGhost || practiceGhostPublished ||
      !options.practiceGhostCallback) {
    return;
  }

  const ReplayData *completedReplay = practice::completedAttemptForGhost(
      options.practiceSession.get(), recordedReplay, recordedAttemptCompleted);
  if (completedReplay == nullptr) {
    return;
  }
  practiceGhostPublished = true;
  options.practiceGhostCallback(*completedReplay);
}

void GamePlayScene::completePracticeAttempt() {
  if (options.practiceSession == nullptr) {
    return;
  }
  const bool recordReplay = shouldRecordReplay();
  finishReplayRecording();
  recordedAttemptCompleted = recordReplay;
  options.practiceSession->completeAttempt(
      recordReplay ? std::move(recordedReplay) : std::move(analyticsReplay));
  publishPracticeGhost();
}

void GamePlayScene::completePracticeSection(bool realtimeRangeFinalized) {
  if (!realtimeRangeFinalized) {
    finalizePracticeRangeMisses();
  }
  state->isEnding = true;
  completePracticeAttempt();
  if (options.practiceSession->shouldLoop()) {
    reset();
  } else {
    scheduleResultTransition(0);
  }
}

void GamePlayScene::finalizePracticeRangeMisses() {
  const auto range = practiceNoteRange();
  if (!range.has_value() || chart == nullptr || state == nullptr) {
    return;
  }
  const long long finalizationTimeMicros = range->endMicros - 1;
  for (auto *note : finalizePendingPracticeNotes(
           *chart, *range, finalizationTimeMicros, options.longNoteMode)) {
    const long long noteTimeMicros =
        note != nullptr && note->Timeline != nullptr ? note->Timeline->Timing
                                                     : finalizationTimeMicros;
    const JudgeResult miss(Poor, finalizationTimeMicros - noteTimeMicros);
    onJudge(miss, judgeEventClock(finalizationTimeMicros), false);
    appendReplayEvent(ReplayEventAction::Miss, note->Lane, note,
                      finalizationTimeMicros, finalizationTimeMicros, miss);
  }
}

void GamePlayScene::finishPractice() {
  if (options.practiceSession == nullptr || resultTransitionScheduled) {
    return;
  }
  options.practiceSession->abandonAttempt();
  if (state != nullptr) {
    state->isEnding = true;
  }
  context.jukebox.stop();
  scheduleResultTransition(0);
}

void GamePlayScene::exitPracticeWithoutSummary() {
  if (options.practiceSession == nullptr) {
    return;
  }
  options.practiceSession->abandonAttempt();
  options.practiceSession.reset();
  context.jukebox.stop();
  defer(
      [this]() {
        if (options.returnScene != nullptr) {
          context.sceneManager->changeScene(options.returnScene, false);
        } else {
          context.sceneManager->changeScene("MainMenu");
        }
        return false;
      },
      0, true);
}

void GamePlayScene::updatePracticeHud(long long chartTimeMicros) {
  if (practiceHudText == nullptr || options.practiceSession == nullptr) {
    return;
  }
  const auto &configuration = options.practiceSession->configuration();
  const auto remaining = std::ranges::count_if(
      preparationPlan.metronome.clicks, [chartTimeMicros](const auto &click) {
        return click.timeMicros > chartTimeMicros;
      });
  std::string text =
      "Loop " + std::to_string(options.practiceSession->loopNumber()) +
      "  |  " + formatPracticeTime(configuration.startMicros) + " - " +
      formatPracticeTime(configuration.endMicros) + "  |  " +
      std::to_string(configuration.playback.percent) + "%";
  if (remaining > 0) {
    text += "  |  Count-in " + std::to_string(remaining);
  }
  practiceHudText->setText(std::move(text));
}

long long GamePlayScene::getAudioOffsetMicros() const {
  return static_cast<long long>(context.settings.audioOffsetMs) * 1000LL;
}

long long GamePlayScene::getStartPositionMicros() const {
  const long long requested =
      static_cast<long long>(std::min<unsigned long long>(
          options.startPosition,
          static_cast<unsigned long long>(std::max(
              0LL, chart != nullptr ? chart->Meta.TotalLength : 0LL))));
  return std::max(0LL, requested);
}

long long GamePlayScene::getAudioSeekPositionMicros() const {
  const long long startPosition = getStartPositionMicros();
  if (options.practiceSession != nullptr) {
    return startPosition;
  }
  const long long leadIn = static_cast<long long>(std::min<unsigned long long>(
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
}

long long
GamePlayScene::getGameplayTimeMicros(long long rawSongTimeMicros) const {
  return gameplay_timing::gameplayTimeFromRawSongTime(rawSongTimeMicros,
                                                      getAudioOffsetMicros());
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

PlayfieldJudgeEventClock
GamePlayScene::judgeEventClock(long long songTimeMicros) const {
  return makePlayfieldJudgeEventClock(songTimeMicros,
                                      getVisualOffsetMicros());
}

void GamePlayScene::initializePlayfieldVisualNoteSources() {
  playfieldVisualNoteSources.clear();
  if (chart == nullptr) {
    return;
  }
  playfieldVisualNoteSources.reserve(playfieldChartVisualModel.notes.size());
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note != nullptr) {
          playfieldVisualNoteSources.push_back(note);
        }
      }
      for (const auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          playfieldVisualNoteSources.push_back(note);
        }
      }
      for (const auto *note : timeline->LandmineNotes) {
        if (note != nullptr) {
          playfieldVisualNoteSources.push_back(note);
        }
      }
    }
  }
  if (playfieldVisualNoteSources.size() !=
      playfieldChartVisualModel.notes.size()) {
    playfieldVisualNoteSources.clear();
  }
}

void GamePlayScene::capturePlayfieldVisualState(
    long long gameplayTimeMicros, long long visualTimeMicros,
    bool startLaneIndicatorsVisible) {
  if (playfieldVisualStateStore == nullptr || state == nullptr) {
    return;
  }

  const auto laneCover = gameplayLaneCoverAuthority(
      playfieldLaneCoverPercent, playfieldLaneCoverEnabled);
  PlayfieldAuthorityUpdate authority{
      .currentBpm = currentGameplayBpm,
      .judgementCounters = state->judgeCount,
      .judgementFastSlowCounters = [&] {
        std::map<Judgement, PlayfieldJudgementFastSlowCount> counters;
        for (const auto &[judgement, value] : state->judgementFastSlowCount) {
          counters.emplace(judgement, PlayfieldJudgementFastSlowCount{
                                        .fast = value.fast, .slow = value.slow});
        }
        return counters;
      }(),
      .comboBreak = state->comboBreak,
      .maximumCombo = state->maxCombo,
      .bestScore = activePacemakerBest ? activePacemakerBest->score : 0,
      .bestScoreTarget = activeBestScoreTarget,
      .gaugeType = state->gaugeType,
      .gaugeAutoShift = state->gaugeAutoShift,
      .currentGauge = state->currentGauge,
      .gaugeRules = state->gaugeRules(),
      .pacemakerTarget = activePacemakerTarget,
      .pacemakerStatus =
          pacemaker::snapshotForState(activePacemakerTarget, *state),
      .playOptionLabel = gameplayPlayOptionLabel(options),
      .autoPlayMarkVisible =
          options.autoPlay ||
          (options.replayData != nullptr && options.replayData->autoPlay),
      .gameplayMode =
          isReplayPlayback()
              ? PlayfieldGameplayMode::Replay
              : ((options.practiceMode || options.practiceSession != nullptr)
                     ? PlayfieldGameplayMode::Practice
                     : PlayfieldGameplayMode::Play),
      .loadingState = PlayfieldLoadingState::Loaded,
      .startLaneIndicators = preparationPlan.laneIndicator.lanes,
      .startLaneIndicatorsVisible = startLaneIndicatorsVisible,
      .laneCoverPercent = laneCover.percent,
      .laneCoverEnabled = laneCover.enabled,
      .liftEnabled = false,
      .liftRatio = 0.0F,
      .hiddenEnabled = false,
      .hiddenRatio = 0.0F,
      .resetLaneCoverVisibleTimeReference =
          playfieldLaneCoverResetPending,
  };
  playfieldVisualStateStore->applyAuthorityUpdate(authority);

  const std::size_t noteCount =
      std::min(playfieldVisualNoteSources.size(),
               playfieldChartVisualModel.notes.size());
  std::vector<NotePresentationState> noteStates;
  noteStates.reserve(noteCount);
  for (std::size_t index = 0; index < noteCount; ++index) {
    const auto *source = playfieldVisualNoteSources[index];
    const auto &model = playfieldChartVisualModel.notes[index];
    NotePresentationState noteState{
        .id = model.id,
        .judged = source->IsPlayed,
        .dead = source->IsDead,
        .playedTimeMicros = source->IsPlayed ? source->PlayedTime
                                             : kPlayfieldTimestampOff,
    };
    if (const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(source);
        longNote != nullptr) {
      const auto *head = longNote->IsTail() && longNote->Head != nullptr
                             ? longNote->Head
                             : longNote;
      const bool headReachedJudge =
          head->IsPlayed || head->IsDead ||
          (head->Timeline != nullptr &&
           head->Timeline->Timing <= visualTimeMicros);
      const auto laneIt = lanePressed.find(head->Lane);
      const bool laneDown =
          laneIt != lanePressed.end() && laneIt->second;
      noteState.longReactive =
          model.longNoteMode == ChartLongNoteMode::HCN &&
          headReachedJudge && laneDown;
      noteState.longActive = head->IsHolding || noteState.longReactive;
      noteState.longDamaged =
          model.longNoteMode == ChartLongNoteMode::HCN &&
          headReachedJudge && !noteState.longActive;
    }
    noteStates.push_back(noteState);
  }
  playfieldVisualStateStore->setNoteStates(std::move(noteStates));

  capturedPlayfieldVisualState = playfieldVisualStateStore->capture({
      .serial = ++playfieldFrameSerial,
      .visualTimeMicros = visualTimeMicros,
      .gameplayTimeMicros = gameplayTimeMicros,
      .replayTouchTimeMicros = gameplayTimeMicros,
      .bgaTimeMicros = gameplayTimeMicros,
      .playTimer = {
          .active = gameplayTimeMicros >= getStartPositionMicros(),
          .startMicros = getStartPositionMicros(),
          .elapsedMillisExact = options.practiceSession == nullptr &&
                                !options.practiceMode,
          .playtimeMillis = beatorajaPlaytimeMillis(chart, options),
      },
  });
  capturedPlayfieldProjection = playfieldProjection.project(
      playfieldChartVisualModel, capturedPlayfieldVisualState,
      {.includeInvisibleNotes =
           capturedPlayfieldVisualState.configuration.showInvisibleNotes,
       .latePoorTimingMicros =
           builtInPresentation->projectionLatePoorTimingMicros(),
       .builtInTraversal = builtInPresentation->projectionTraversal()});
  playfieldLaneCoverResetPending = false;
}

void GamePlayScene::scheduleResultTransition(int delayMillis) {
  if (resultTransitionScheduled) {
    return;
  }
  resultTransitionScheduled = true;

  finishReplayRecording();
  const auto capturePolicy = resultCapturePolicy();
  const auto persistenceRoute =
      gameplay_startup::completedAttemptPersistenceRoute(
          capturePolicy.persistScore && capturePolicy.persistReplay,
          isCoursePlayback());
  const auto modernCapture =
      persistenceRoute ==
              gameplay_startup::CompletedAttemptPersistenceRoute::None
          ? std::optional<CompletedModernReplayCapture>{}
          : std::optional<CompletedModernReplayCapture>{
                completeModernReplayCapture()};
  if (persistenceRoute ==
      gameplay_startup::CompletedAttemptPersistenceRoute::ModernChartFile) {
    resultPersistenceAttemptCreationTried = true;
    if (resultPersistenceAttemptId.empty()) {
      resultPersistenceAttemptId = uuid::generateV4();
    }

    const std::int64_t playedAt = nowUnixMillis();
    const int resultLongNoteMode =
        chart != nullptr
            ? scoreLongNoteModeForClearLamp(chart->Meta, options.longNoteMode)
            : 0;
    std::string constructionDiagnostic;
    std::optional<result_persistence::ModernChartResult> result;
    if (chart != nullptr && state != nullptr) {
      result = result_persistence::captureModernChartResult(
          resultPersistenceAttemptId, chart->Meta, *state, attemptProvenance,
          resultLongNoteMode, playedAt, constructionDiagnostic);
    }

    replay::ChartReplayPersistenceOutcome persistenceOutcome{
        .state = replay::ChartReplayPersistenceState::InvalidAttempt,
        .diagnostic = constructionDiagnostic.empty()
                          ? "modern result capture failed"
                          : constructionDiagnostic};
    if (result.has_value()) {
      const replay::ReplayChartIdentity resultIdentity{
          .md5 = result->score.chartMd5,
          .sha256 = result->score.chartSha256,
          .keyMode = result->keyMode,
      };
      const int replayLongNoteMode =
          result_persistence::replaySetupLongNoteMode(result->score)
              .value_or(-1);
      const int initialLaneCover =
          recordedReplay.laneCoverEvents.empty()
              ? effectiveNoteStartPositionPercent()
              : recordedReplay.laneCoverEvents.front().noteStartPositionPercent;
      const replay::ChartReplayCapture capture{
          .result = std::move(*result),
          .setupFacts = {.chart = resultIdentity,
               .longNoteMode = replayLongNoteMode,
               .hasUndefinedLongNotes =
                   chartContainsUndefinedLongNote(*chart),
               .initialLaneCoverPercent = initialLaneCover,
               .laneCoverEnabled = initialLaneCover > 0},
          .acceptedInput = modernCapture->acceptedInput,
          .touchSamples = modernCapture->touchSamples,
          .laneCoverEvents = modernCapture->laneCoverEvents,
          .timeBounds = modernCapture->timeBounds,
      };
      auto attempt = replay::captureChartReplayPersistenceAttempt(
          capture, constructionDiagnostic);
      if (attempt.has_value()) {
        auto retainedAttempt =
            std::make_shared<const replay::ChartReplayPersistenceAttempt>(
                std::move(*attempt));
        resultPersistenceOptions.chartAttempt = retainedAttempt;
        if (retainedAttempt->irSnapshot.has_value()) {
          resultPersistenceOptions.irSubmission =
              std::make_shared<const ir::IrSubmission>(
                  retainedAttempt->irSnapshot->submission);
        }
        const std::vector<ir::IrOutboxDraft> automaticDrafts =
            retainedAttempt->irSnapshot
                ? context.irDrivers.buildAutomaticDrafts(
                      context.settings.irProviders,
                      retainedAttempt->irSnapshot->submission)
                : std::vector<ir::IrOutboxDraft>{};
        persistenceOutcome = context.persistModernChart(*retainedAttempt,
                                                        automaticDrafts);
        if (persistenceOutcome.durable() && !automaticDrafts.empty() &&
            context.irSubmissionService) {
          context.irSubmissionService->notifyOutboxChanged();
        }
      } else {
        persistenceOutcome.diagnostic =
            constructionDiagnostic.empty()
                ? "modern chart completion construction failed"
                : constructionDiagnostic;
      }
    }
    resultPersistenceOptions.chartOutcome = persistenceOutcome;
    resultPersistenceOptions.outcome =
        chartResultPersistencePresentation(persistenceOutcome);
    if (!modernReplayCaptureDiagnostic.empty()) {
      SDL_Log("Modern replay capture diagnostic=%s",
              modernReplayCaptureDiagnostic.c_str());
    }
    if (!constructionDiagnostic.empty()) {
      SDL_Log("Modern chart construction diagnostic=%s",
              constructionDiagnostic.c_str());
    }
    SDL_Log("Modern chart persistence state=%s diagnostic=%s",
            chartReplayPersistenceStateName(persistenceOutcome.state),
            persistenceOutcome.diagnostic.c_str());
    if (!persistenceOutcome.saved()) {
      delayMillis = 0;
    }
  } else if (persistenceRoute ==
             gameplay_startup::CompletedAttemptPersistenceRoute::
                 ModernCourseFile) {
      resultPersistenceAttemptCreationTried = true;
    if (modernCapture.has_value()) {
      recordModernCourseStage(*modernCapture);
      }
    if (!modernReplayCaptureDiagnostic.empty()) {
      SDL_Log("Modern course replay capture diagnostic=%s",
              modernReplayCaptureDiagnostic.c_str());
    }
  }

  defer(
      [this, capturePolicy]() {
        const ReplayData *presentationReplay =
            shouldPersistRecordedReplay() ? &recordedReplay : nullptr;
        const ReplayData *retrySource =
            presentationReplay != nullptr
                ? presentationReplay
                : (options.replayData != nullptr ? options.replayData.get()
                                                 : nullptr);
        const ReplayData *analyticsSource =
            practice::selectResultAnalyticsSource(
                capturePolicy.captureAnalytics ? &analyticsReplay : nullptr,
                presentationReplay, retrySource);
        ResultPracticeOptions practiceResultOptions;
        if (options.practiceMode || options.practiceSession != nullptr) {
          practiceResultOptions.enabled = true;
          practiceResultOptions.session = options.practiceSession;
          if (options.practiceSession == nullptr) {
            practiceResultOptions.startPosition =
                static_cast<unsigned long long>(getStartPositionMicros());
            practiceResultOptions.gaugeType = options.gaugeType;
          }
          practiceResultOptions.autoKeySound = options.autoKeySound;
          practiceResultOptions.autoPlay = options.autoPlay;
          practiceResultOptions.gaugeAutoShift = options.gaugeAutoShift;
          practiceResultOptions.gaugeAutoShiftLowerBound =
              options.gaugeAutoShiftLowerBound;
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
        std::optional<ResultPacemakerData> gbattleResultPacemaker;
        if (options.gbattleRecordData != nullptr) {
          gbattleResultPacemaker = gbattle::resultPacemakerDataFromRecord(
              *chart, *state, *options.gbattleRecordData);
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
                context, resultMeta, *state, attemptProvenance,
                presentationReplay, resultPersistenceOptions, retrySource,
                practiceResultOptions,
                options.autoPlay || (options.replayData != nullptr &&
                                     options.replayData->autoPlay),
                courseResultOptions, resultPacemakerTarget,
                std::move(ownedReusableRetryChart), reusableRetryChart,
                gbattleResultPacemaker, analyticsSource,
                resultPersistenceOptions.chartAttempt != nullptr &&
                        resultPersistenceOptions.chartOutcome.has_value() &&
                        resultPersistenceOptions.chartOutcome->durable()
                    ? std::optional<std::string>(
                          resultPersistenceOptions.chartAttempt->result.attemptId)
                    : std::nullopt,
                true),
            false);
        return false;
      },
      delayMillis, true);
}

bool GamePlayScene::finishIfGaugeFailed() {
  if (state == nullptr || state->isEnding || !state->activeGaugeFailed()) {
    return false;
  }

  SDL_Log("Active survival gauge failed");
  state->isEnding = true;
  context.jukebox.stop();
  if (options.practiceSession != nullptr) {
    completePracticeAttempt();
  } else {
    finishReplayRecording();
    recordedAttemptCompleted = options.practiceMode;
    publishPracticeGhost();
  }
  if (isCoursePlayback()) {
    if (!usesModernCourseContinuation()) {
      options.courseSession->carriedGauge = state->gaugeSnapshot();
      options.courseSession->carriedCombo = state->combo;
      options.courseSession->maxCombo =
          std::max(options.courseSession->maxCombo, state->maxCombo);
    }
    options.courseSession->recordResult(chart->Meta, *state);
    if (!options.courseSession->courseReplayPlayback) {
      options.courseSession->recordStageProvenance(
          options.courseSession->currentIndex, attemptProvenance);
    }
    if (shouldRecordReplay()) {
      options.courseSession->recordReplayStage(recordedReplay);
    }
  }
  scheduleResultTransition(0);
  return true;
}

void GamePlayScene::update(float dt) {
  (void)dt;
  applyPendingBestReplay();
  const bool realtimeAtFrameStart = realtimeGameplayAuthorityActive();
  if (inputHandler != nullptr && !realtimeAtFrameStart) {
    inputHandler->pumpPendingTouchEvents();
  }
  if (realtimeAtFrameStart) {
    drainRealtimeInputCommands();
    drainRealtimeStartSelectInputs();
    drainRealtimeTouchSamples();
  }
  if (startSelectControl.has_value() && !isReplayPlayback() &&
      !options.autoPlay) {
    applyStartSelectControlActions(startSelectControl->tick(
        nowMicros(), {.noteEnd = state != nullptr && state->isEnding}));
  }
  updateCoursePauseHoldProgress(nowMicros());
  if (state == nullptr || !state->isPlaying || state->isEnding) {
    return;
  }

  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  long long gameplayTimeMicros = getGameplayTimeMicros(rawSongTimeMicros);
  bool practiceSectionComplete = false;
  if (options.practiceSession != nullptr) {
    const auto practiceFrame = gameplay_timing::practiceFrameTiming(
        rawSongTimeMicros, getAudioOffsetMicros(),
        options.practiceSession->configuration().endMicros);
    gameplayTimeMicros = practiceFrame.chartTimeMicros;
    practiceSectionComplete = practiceFrame.sectionComplete;
  }
  updatePracticeHud(gameplayTimeMicros);
  if (realtimeAtFrameStart) {
    syncRealtimeGameplaySnapshot();
    updateRealtimeVisualTimeline(gameplayTimeMicros);
  }
  touchVisualizerLoaded = true;
  if (isReplayPlayback()) {
    processReplayEvents(gameplayTimeMicros);
    processReplayLaneCoverEvents(gameplayTimeMicros);
  }
  if (preparationIndicatorActive(rawSongTimeMicros)) {
    return;
  }
  if (realtimeAtFrameStart) {
    const auto fault = realtimeGameplaySession->worker->fault();
    const auto terminalReason = [this] {
      auto realtimeSnapshot =
          realtimeGameplaySession->worker->acquireLatestSnapshot();
      return realtimeSnapshot ? realtimeSnapshot->terminalReason
                              : gameplay::GameplayTerminalReason::None;
    }();
    if (fault != gameplay::RealtimeGameplayFault::None) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT, "Realtime gameplay input fault: %d",
                   static_cast<int>(fault));
      stopRealtimeGameplayAuthority(false);
      if (state != nullptr) {
        state->isEnding = true;
      }
      showPlaybackInitializationFailure(
          "Realtime input integrity failed. This attempt was invalidated; "
          "return and retry.");
      return;
    }
    const auto terminalAction = gameplay::classifyRealtimeGameplayTerminal(
        terminalReason, options.practiceSession != nullptr);
    if (terminalAction == gameplay::RealtimeGameplayTerminalAction::Wait) {
      return;
    }
    if (terminalAction ==
        gameplay::RealtimeGameplayTerminalAction::SurvivalGaugeFailed) {
      stopRealtimeGameplayAuthority(true);
      (void)finishIfGaugeFailed();
      return;
    }
    if (terminalAction ==
        gameplay::RealtimeGameplayTerminalAction::CompletePractice) {
      stopRealtimeGameplayAuthority(true);
      completePracticeSection(true);
      return;
    }
    if (terminalAction ==
        gameplay::RealtimeGameplayTerminalAction::IntegrityFailure) {
      SDL_LogError(SDL_LOG_CATEGORY_INPUT,
                   "Realtime gameplay terminated for integrity reason: %d",
                   static_cast<int>(terminalReason));
      stopRealtimeGameplayAuthority(false);
      if (state != nullptr) {
        state->isEnding = true;
      }
      showPlaybackInitializationFailure(
          "Realtime input integrity failed. This attempt was invalidated; "
          "return and retry.");
      return;
    }
    stopRealtimeGameplayAuthority(true);
    state->passedMeasureCount = chart->Measures.size();
    state->passedTimelineCount = 0;
  } else {
    updateHellChargeGauge(gameplayTimeMicros);
    if (finishIfGaugeFailed()) {
      return;
    }
    checkPassedTimeline(gameplayTimeMicros);
    if (finishIfGaugeFailed()) {
      return;
    }
  }
  if (practiceSectionComplete) {
    completePracticeSection(false);
    return;
  }
  if (state->passedMeasureCount != chart->Measures.size()) {
    return;
  }

  if (options.practiceSession != nullptr) {
    completePracticeSection(false);
    return;
  }

  SDL_Log("All measures passed");
  state->isEnding = true;
  finishReplayRecording();
  recordedAttemptCompleted = options.practiceMode;
  publishPracticeGhost();
  if (isCoursePlayback()) {
    if (!usesModernCourseContinuation()) {
      options.courseSession->carriedGauge = state->gaugeSnapshot();
      options.courseSession->carriedCombo = state->combo;
      options.courseSession->maxCombo =
          std::max(options.courseSession->maxCombo, state->maxCombo);
    }
    options.courseSession->recordResult(chart->Meta, *state);
    if (!options.courseSession->courseReplayPlayback) {
      options.courseSession->recordStageProvenance(
          options.courseSession->currentIndex, attemptProvenance);
    }
    if (shouldRecordReplay()) {
      options.courseSession->recordReplayStage(recordedReplay);
    }
  }
  scheduleResultTransition(2000);
}

void GamePlayScene::renderScene() {
  if (playbackInitializationFailed) {
    return;
  }
  RenderContext renderContext;
  pauseLayout->setSize(rendering::window_width, rendering::window_height);
  if (pauseButton != nullptr) {
    pauseButton->setPositionNoLayout(rendering::window_width - 88, 38);
  }
  if (practiceRestartButton != nullptr) {
    practiceRestartButton->setPositionNoLayout(rendering::window_width - 88,
                                               98);
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (skinResetLayoutButton != nullptr) {
    const auto safe = gameplaySkinSafeUiBounds();
    skinResetLayoutButton->setPositionNoLayout(
        static_cast<int>(std::max(safe.x, safe.x + safe.width - 250.0)),
        static_cast<int>(safe.y + 38.0));
  }
#endif
  refreshGameplayPresentationGeometry();
  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  const bool startLaneIndicatorsVisible =
      preparationIndicatorActive(rawSongTimeMicros);
  long long gameplayTimeMicros = getGameplayTimeMicros(rawSongTimeMicros);
  if (const auto range = practiceNoteRange();
      range.has_value() && gameplayTimeMicros >= range->endMicros) {
    gameplayTimeMicros = range->endMicros - 1;
  }
  const long long visualTimeMicros = getVisualTimeMicros(gameplayTimeMicros);
  capturePlayfieldVisualState(gameplayTimeMicros, visualTimeMicros,
                              startLaneIndicatorsVisible);
  (void)presentation->prepareFrame(capturedPlayfieldVisualState,
                                   capturedPlayfieldProjection);
  const PresentationFrameResult presentationFrame =
      presentation->render(renderContext);
  if (!options.autoPlay && chart != nullptr) {
    renderVirtualControllerOverlay(currentVirtualControllerLayout(
        context.inputProfile.virtualController, chart->Meta.KeyMode,
        realtimeTouchUiTransform()));
  }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  if (!realtimeGameplayAuthorityActive() && !options.autoPlay &&
      gameplay::shouldRetryRealtimeGameplayAuthorityAfterSkinFrame(
          realtimeGameplayAuthorityWaitingForSkinGeometry,
          presentationFrame.outcome == PresentationFrameOutcome::Ready &&
              presentationFrame.submittedMode == PresentationMode::Skin &&
              !presentation->touchLayout().laneRegions.empty())) {
    (void)startRealtimeGameplayAuthority();
  }
#endif
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  context.gameplayBgaCompositeState = {
      .frameSerial = presentationFrame.frameSerial,
      .mode = presentationFrame.bgaCompositeMode,
      .prepared = presentationFrame.preparedBga,
  };
  if (presentationFrame.failure &&
      (presentationFrame.outcome == PresentationFrameOutcome::CriticalFailure ||
       presentationFrame.submittedMode == PresentationMode::BuiltIn) &&
      !presentationFrame.failure->entry.collisionKey.empty()) {
    showPlaybackInitializationFailure(
        gameplaySkinFailureMessage(presentationFrame.failure->diagnostic));
    return;
  }
#endif
  updateSkinResetLayoutVisibility();
  renderCoursePauseHoldRing();
  if (laneStateText != nullptr) {
    laneStateText->render(renderContext);
  }
}

bool GamePlayScene::renderViewBeforeScene(const View *view) const {
  return view != pauseLayout && view != pauseButton &&
         view != practiceRestartButton && view != skinResetLayoutButton &&
         view != practiceHudText && view != playbackFailureLayout;
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
    if (event.window.event == SDL_WINDOWEVENT_LEAVE && coursePauseHoldActive &&
        !coursePauseHoldTouch) {
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
  coursePauseHoldStartMicros =
      nowMicros() -
      static_cast<long long>(coursePauseHoldProgress *
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
    const float progress =
        static_cast<float>(currentMicros - coursePauseHoldStartMicros) /
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
  coursePauseHoldProgress = coursePauseHoldRewindStartProgress *
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
  const float radius = static_cast<float>(std::max(pauseButton->getWidth(),
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
  stopBestReplayLoad();
  stopRealtimeGameplayAuthority(false);
  context.profileGameplayActive.store(false, std::memory_order_release);
  profileGameplayBlockerActive = false;
  context.jukebox.removeOnTick();
  SDL_Log("Stopping input handler");
  if (inputHandler != nullptr) {
    inputHandler->stopListen();
  }
  ownedInputHandler.reset();
  inputHandler = nullptr;
  ownedLaneInputController.reset();
  laneInputController = nullptr;
  ownedPresentationEventFanout.reset();
  presentationEventFanout = nullptr;
  hellChargeGaugeBalanceMicros.clear();
  presentation = nullptr;
  builtInPresentation = nullptr;
  ownedPresentation.reset();
  ownedPlayfieldVisualStateStore.reset();
  playfieldVisualStateStore = nullptr;
  playfieldProjection.reset();
  capturedPlayfieldVisualState = {};
  capturedPlayfieldProjection = {};
  ownedState.reset();
  state = nullptr;
  ownedLaneStateText.reset();
  laneStateText = nullptr;
  ownedChart.reset();
  chart = nullptr;
  playbackFailureLayout = nullptr;
  skinResetLayoutButton = nullptr;
  SDL_Log("Cleaned up GamePlayScene");
}
bms_parser::Note *GamePlayScene::pressLane(int lane, double inputDelay) {
  if (laneInputController == nullptr && !realtimeGameplayAuthorityActive()) {
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
  if (realtimeGameplayAuthorityActive()) {
    (void)realtimeGameplaySession->prepareLegacyInput(
        gameplay::RealtimeGameplayInputType::Press, mainLane, compensateLane,
        false, static_cast<long long>(inputDelay * 1000000.0));
    return nullptr;
  }
  if (laneInputController == nullptr) {
    return nullptr;
  }
  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  const long long gameplayTimeMicros =
      getGameplayTimeMicros(rawSongTimeMicros);
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = gameplayTimeMicros,
      .laneBeamTimeMicros = playfieldVisualEventTimeMicros(
          gameplayTimeMicros, getVisualOffsetMicros()),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  const bool preparationInput = gameplay::preparationInputUsesVisualOnlyPath(
      preparationIndicatorActive(rawSongTimeMicros),
      options.practiceSession != nullptr);
  if (preparationInput) {
    const auto result = laneInputController->pressLaneForPreparation(
        mainLane, compensateLane, inputContext);
    updateLaneStateText();
    if (result.keySoundNote != nullptr &&
        result.keySoundNote->Wav != bms_parser::Parser::NoWav &&
        !options.autoKeySound && !isReplayPlayback()) {
      context.jukebox.playKeySound(result.keySoundNote->Wav);
    }
    if (result.hasReplayEvent) {
      const auto &event = result.replayEvent;
      recordPreparationLaneEvent(event.action, event.lane,
                                 event.songTimeMicros);
    }
    return result.note;
  }
  const auto result =
      laneInputController->pressLane(mainLane, compensateLane, inputContext);
  updateLaneStateText();
  if (result.keySoundNote != nullptr &&
      result.keySoundNote->Wav != bms_parser::Parser::NoWav &&
      !options.autoKeySound && !isReplayPlayback()) {
    context.jukebox.playKeySound(result.keySoundNote->Wav);
  }
  for (const auto &transaction : result.transactions) {
    if (transaction.hasJudge) {
      const long long eventSongTimeMicros =
          transaction.hasReplayEvent ? transaction.replayEvent.songTimeMicros
                                     : inputContext.songTimeMicros;
      onJudge(transaction.judge, judgeEventClock(eventSongTimeMicros),
              !options.autoPlay || isReplayPlayback());
    }
    if (transaction.hasReplayEvent) {
      const auto &event = transaction.replayEvent;
      appendReplayEvent(event.action, event.lane, event.note,
                        event.songTimeMicros, event.judgeTimeMicros,
                        event.judge);
    }
  }
  return result.note;
}
bms_parser::Note *GamePlayScene::releaseLane(int lane, double inputDelay,
                                             bool isBackSpin) {
  if (isGamePaused || state == nullptr || !state->isPlaying ||
      state->isEnding) {
    return nullptr;
  }
  if (realtimeGameplayAuthorityActive()) {
    (void)realtimeGameplaySession->prepareLegacyInput(
        gameplay::RealtimeGameplayInputType::Release, lane, lane, isBackSpin,
        static_cast<long long>(inputDelay * 1000000.0));
    return nullptr;
  }
  if (laneInputController == nullptr) {
    return nullptr;
  }
  const long long rawSongTimeMicros = context.jukebox.getTimeMicros();
  const long long gameplayTimeMicros =
      getGameplayTimeMicros(rawSongTimeMicros);
  const RhythmLaneInputController::InputContext inputContext{
      .songTimeMicros = gameplayTimeMicros,
      .laneBeamTimeMicros = playfieldVisualEventTimeMicros(
          gameplayTimeMicros, getVisualOffsetMicros()),
      .inputDelay = inputDelay,
      .notePriorityMode = context.settings.notePriorityMode,
  };
  const bool preparationInput = gameplay::preparationInputUsesVisualOnlyPath(
      preparationIndicatorActive(rawSongTimeMicros),
      options.practiceSession != nullptr);
  if (preparationInput) {
    const auto result =
        laneInputController->releaseLaneForPreparation(lane, inputContext);
    updateLaneStateText();
    if (result.hasReplayEvent) {
      const auto &event = result.replayEvent;
      recordPreparationLaneEvent(event.action, event.lane,
                                 event.songTimeMicros);
    }
    return result.note;
  }
  const auto result =
      laneInputController->releaseLane(lane, inputContext, isBackSpin);
  updateLaneStateText();
  for (const auto &transaction : result.transactions) {
    if (transaction.hasJudge) {
      const long long eventSongTimeMicros =
          transaction.hasReplayEvent ? transaction.replayEvent.songTimeMicros
                                     : inputContext.songTimeMicros;
      onJudge(transaction.judge, judgeEventClock(eventSongTimeMicros),
              !options.autoPlay || isReplayPlayback());
    }
    if (transaction.hasReplayEvent) {
      const auto &event = transaction.replayEvent;
      appendReplayEvent(event.action, event.lane, event.note,
                        event.songTimeMicros, event.judgeTimeMicros,
                        event.judge);
    }
  }
  return result.note;
}
void GamePlayScene::checkPassedTimeline(long long time) {
  const auto &measures = chart->Measures;
  if (state == nullptr) {
    return;
  }
  const long long visualEventMicros = getVisualTimeMicros(time);
  const PlayfieldJudgeEventClock eventClock = judgeEventClock(time);
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
                onJudge(poorResult, eventClock, false);
                appendReplayEvent(ReplayEventAction::Miss, note->Lane, note,
                                  time, judgedTime, poorResult);
                if (longNote->Tail != nullptr && !longNote->Tail->IsPlayed) {
                  markLongNoteMissed(longNote->Tail, judgedTime,
                                     !longNoteTailJudgedBeforeTiming(
                                         longNote->Tail, judgedTime));
                  onJudge(poorResult, eventClock, false);
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
              onJudge(poorResult, eventClock, false);
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
          onJudge(poorResult, eventClock, false);
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
                            rulesetPolicyBuild.policy->judge.judgeAt(
                                gameplay::judgeRoleFor(longNote, chart->Meta,
                                    options.longNoteMode),
                                longNote->Timeline->Timing, judgedTime))
                      : judgeClassicLongNoteRelease(
                            rulesetPolicyBuild.policy->judge, chart->Meta,
                            options.longNoteMode, longNote, judgedTime);
              onJudge(judgeResult, eventClock, false);
              appendReplayEvent(ReplayEventAction::Release, note->Lane, note,
                                time, judgedTime, judgeResult);
              if (options.autoPlay) {
                presentationEventFanout->onLaneReleased(note->Lane,
                                                         visualEventMicros);
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
            presentationEventFanout->onLanePressed(note->Lane, judgeResult,
                                                    visualEventMicros);
            if (!note->IsLongNote()) {
              presentationEventFanout->onLaneReleased(note->Lane,
                                                       visualEventMicros);
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
  const auto range = practiceNoteRange();
  if (event.noteTimeMicros < 0 ||
      (range.has_value() && !range->contains(event.noteTimeMicros))) {
    return nullptr;
  }
  const auto it =
      replayNoteLookup.find(replayNoteKey(event.lane, event.noteTimeMicros));
  return it == replayNoteLookup.end() ? nullptr : it->second;
}

void GamePlayScene::processReplayEvents(long long gameplayTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr) {
    return;
  }

  const auto &events = options.replayData->events;
  while (replayEventCursor < events.size() &&
         events[replayEventCursor].songTimeMicros <= gameplayTimeMicros) {
    if (practiceReplayEventAllowed(events[replayEventCursor])) {
      applyReplayEvent(events[replayEventCursor],
                       getVisualTimeMicros(
                           events[replayEventCursor].songTimeMicros));
    }
    replayEventCursor++;
  }
}

void GamePlayScene::processReplayLaneCoverEvents(long long gameplayTimeMicros) {
  if (!isReplayPlayback() || options.replayData == nullptr) {
    return;
  }

  const auto &events = options.replayData->laneCoverEvents;
  while (replayLaneCoverCursor < events.size() &&
         events[replayLaneCoverCursor].songTimeMicros <= gameplayTimeMicros) {
    if (practiceInputAllowed(events[replayLaneCoverCursor].songTimeMicros)) {
      applyReplayLaneCoverEvent(events[replayLaneCoverCursor]);
    }
    replayLaneCoverCursor++;
  }
}

void GamePlayScene::applyReplayLaneCoverEvent(
    const ReplayLaneCoverEvent &event) {
  playfieldLaneCoverPercent = event.noteStartPositionPercent;
  playfieldLaneCoverPercentExact =
      static_cast<float>(event.noteStartPositionPercent);
  refreshLaneCoverHispeedFactor();
  playfieldLaneCoverResetPending = event.resetVisibleTimeReference;
  refreshRuntimePresentationConfiguration();
}

void GamePlayScene::applyReplayEvent(const ReplayEvent &event,
                                     long long visualTimeMicros) {
  if (state == nullptr || !state->isPlaying || state->isEnding ||
      !practiceReplayEventAllowed(event)) {
    return;
  }

  const JudgeResult recordedJudge(event.judgement, event.diffMicros);
  const PlayfieldJudgeEventClock eventClock{
      .songTimeMicros = event.songTimeMicros,
      .visualTimeMicros = visualTimeMicros,
      .bgaTimeMicros = event.songTimeMicros,
  };
  switch (event.action) {
  case ReplayEventAction::MultiBad:
    if (auto *note = findReplayNote(event);
        note != nullptr && event.judgement != None) {
      note->Play(event.judgeTimeMicros);
      if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
          longNote != nullptr && !longNote->IsTail() &&
          longNote->Tail != nullptr && !longNote->Tail->IsPlayed) {
        longNote->Tail->Play(event.judgeTimeMicros);
      }
      onJudge(recordedJudge, eventClock, false);
      applyReplayGauge(event);
    }
    break;
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
    presentationEventFanout->onLanePressed(event.lane, recordedJudge,
                                            visualTimeMicros);
    break;
  }
  case ReplayEventAction::Release: {
    if (auto pressedIt = lanePressed.find(event.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = false;
    }
    updateLaneStateText();
    presentationEventFanout->onLaneReleased(event.lane, visualTimeMicros);

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
      onJudge(recordedJudge, eventClock, false);
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
  case ReplayEventAction::Gauge:
    if (event.judgement == Great || event.judgement == Bad) {
      state->applyGaugeJudgementRate(event.judgement, 0.5f);
    } else {
      state->gaugeHistory.push_back(event.gauge);
    }
    applyReplayGauge(event);
    break;
  }
  if (event.action != ReplayEventAction::MultiBad) {
    (void)finishIfGaugeFailed();
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
    if (gaugeIsSurvival(event.gaugeType, state->gaugeProfile) &&
        event.gauge <= 0.0f) {
      state->gaugeSurvivalFailed[gaugeIndex] = true;
    }
  }
  if (!state->gaugeHistory.empty()) {
    state->gaugeHistory.back() = event.gauge;
  }
  auto &typedHistory = state->gaugeHistoryFor(event.gaugeType);
  if (!typedHistory.empty()) {
    typedHistory.back() = event.gauge;
  } else {
    typedHistory.push_back(event.gauge);
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

  const auto applyGaugeTick = [&](Judgement judgement) {
    state->applyGaugeJudgementRate(judgement, 0.5f);
    updateGaugeStatusText();
    appendReplayEvent(ReplayEventAction::Gauge, -1, nullptr, gameplayTimeMicros,
                      gameplayTimeMicros, JudgeResult(judgement, 0));
    return state->isEnding;
  };
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
            longNote->Timeline == nullptr ||
            longNote->Tail->Timeline == nullptr ||
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
          balance -= kHellChargeGaugeTickMicros;
          if (applyGaugeTick(Great)) {
            return;
          }
        }
        while (balance < -kHellChargeGaugeTickMicros) {
          balance += kHellChargeGaugeTickMicros;
          if (applyGaugeTick(Bad)) {
            return;
          }
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
                            PlayfieldJudgeEventClock clock,
                            bool recordTimingSample) {
  if (state == nullptr || state->isEnding) {
    return;
  }
  const int previousCount = state->judgeCount[judgeResult.judgement];
  state->commitJudge(judgeResult);
  const int judgementCount = previousCount + 1;
  presentationEventFanout->onJudge(judgeResult, state->combo,
                                   state->getScore(), clock,
                                   recordTimingSample);
  (void)judgementCount;
  // CurrentRhythmHUD->OnJudge(state);
  // UE_LOG(LogTemp, Warning, TEXT("Judge: %s, Combo: %d, Diff: %lld"),
  // *JudgeResult.ToString(), state->Combo, JudgeResult.Diff);

  updateGaugeStatusText();
  updatePacemakerStatus();
}

void GamePlayScene::appendReplayEvent(ReplayEventAction action, int lane,
                                      const bms_parser::Note *note,
                                      long long songTimeMicros,
                                      long long judgeTimeMicros,
                                      const JudgeResult &judgeResult) {
  const auto capturePolicy = resultCapturePolicy();
  if (state == nullptr || state->isEnding) {
    return;
  }
  const auto range = practiceNoteRange();
  const bool withinPracticeRange =
      !range.has_value() || (range->contains(songTimeMicros) &&
                             (note == nullptr || range->contains(note)));
  if (withinPracticeRange &&
      (capturePolicy.recordReplay || capturePolicy.captureAnalytics)) {
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
    if (capturePolicy.captureAnalytics) {
      analyticsReplay.events.push_back(event);
    }
    if (capturePolicy.recordReplay) {
      recordedReplay.events.push_back(event);
    }
  }
  if (action != ReplayEventAction::MultiBad) {
    (void)finishIfGaugeFailed();
  }
}

void GamePlayScene::recordPreparationLaneEvent(ReplayEventAction action,
                                               int lane,
                                               long long songTimeMicros) {
  if (!shouldRecordReplay() || state == nullptr || state->isEnding) {
    return;
  }
  recordedReplay.events.push_back({
      .action = action,
      .lane = lane,
      .noteTimeMicros = -1,
      .songTimeMicros = songTimeMicros,
      .judgeTimeMicros = songTimeMicros,
      .judgement = None,
      .gauge = state->currentGauge,
      .gaugeType = state->gaugeType,
      .combo = state->combo,
      .score = state->getScore(),
  });
}

void GamePlayScene::appendReplayLaneCoverEvent(int noteStartPositionPercent,
                                               long long songTimeMicros,
                                               bool resetVisibleTimeReference) {
  if (!shouldRecordReplay() || state == nullptr) {
    return;
  }
  if (!practiceInputAllowed(songTimeMicros)) {
    return;
  }

  ReplayLaneCoverEvent event;
  event.songTimeMicros = songTimeMicros;
  event.noteStartPositionPercent = std::clamp(
      noteStartPositionPercent, AppSettings::kMinNoteStartPositionPercent,
      AppSettings::kMaxNoteStartPositionPercent);
  event.resetVisibleTimeReference = resetVisibleTimeReference;
  recordedReplay.laneCoverEvents.push_back(event);
}

bool GamePlayScene::handleTouchInput(SDL_FingerID fingerIndex,
                                     ReplayTouchAction action,
                                     Vector3 normalizedLocation) {
  const long long gameplayTimeMicros =
      getGameplayTimeMicros(context.jukebox.getTimeMicros());
  return handleTouchInputAtGameplayTime(fingerIndex, action, normalizedLocation,
                                        gameplayTimeMicros);
}

bool GamePlayScene::handleTouchInputAtGameplayTime(
    SDL_FingerID fingerIndex, ReplayTouchAction action,
    Vector3 normalizedLocation, long long gameplayTimeMicros,
    std::optional<long long> visualGameplayTimeMicros,
    bool allowBuiltInControl) {
  if (!practiceInputAllowed(gameplayTimeMicros)) {
    return false;
  }
  const bool activeFloatingDrag =
      floatingLaneCoverDragActive && fingerIndex == floatingLaneCoverFinger;
  if (state == nullptr || !state->isPlaying || state->isEnding ||
      context.jukebox.isPaused()) {
    if (activeFloatingDrag && (action == ReplayTouchAction::Up ||
                               action == ReplayTouchAction::Cancel)) {
      floatingLaneCoverDragActive = false;
      floatingLaneCoverDragChanged = false;
      floatingLaneCoverFinger = -1;
      floatingLaneCoverDragOffsetY = 0.0f;
      persistFloatingLaneCoverSettings();
    }
    return activeFloatingDrag;
  }

  if (touchVisualizerLoaded) {
    const long long touchTimeMicros =
        visualGameplayTimeMicros.value_or(gameplayTimeMicros);
    (void)touchTimeMicros;
    if (playfieldVisualStateStore != nullptr) {
      playfieldVisualStateStore->setLiveTouchPoint(
          static_cast<long long>(fingerIndex), action, normalizedLocation.x,
          normalizedLocation.y, gameplayTimeMicros);
    }
  }
  appendReplayTouchSample(fingerIndex, action, normalizedLocation,
                          gameplayTimeMicros);
  return !allowBuiltInControl ||
         handleFloatingLaneCoverInput(fingerIndex, action,
                                      normalizedLocation, gameplayTimeMicros);
}

bool GamePlayScene::handleFloatingLaneCoverInput(SDL_FingerID fingerIndex,
                                                 ReplayTouchAction action,
                                                 Vector3 normalizedLocation,
                                                 long long songTimeMicros) {
  if (!practiceInputAllowed(songTimeMicros) ||
      builtInPresentation == nullptr || presentation == nullptr ||
      presentation->activeMode() != PresentationMode::BuiltIn) {
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
    const int next = builtInPresentation->dragLaneCoverHandleTo(
        renderX, renderY, floatingLaneCoverDragOffsetY);
    context.settings.noteStartPositionPercent = next;
    playfieldLaneCoverPercent = next;
    playfieldLaneCoverPercentExact = static_cast<float>(next);
    if (next == previous) {
      return false;
    }
    refreshLaneCoverHispeedFactor();
    playfieldLaneCoverResetPending = context.settings.hispeedAutoAdjust;
    refreshRuntimePresentationConfiguration();
    floatingLaneCoverDragChanged = true;
    floatingLaneCoverSettingsDirty = true;
    appendReplayLaneCoverEvent(next, songTimeMicros,
                                context.settings.hispeedAutoAdjust);
    return true;
  };

  switch (action) {
  case ReplayTouchAction::Down:
    if (const auto grabOffset =
            builtInPresentation->laneCoverHandleGrabOffset(renderX, renderY);
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

void GamePlayScene::cancelLegacyFloatingLaneCoverTouch() {
  if (!floatingLaneCoverDragActive) {
    return;
  }
  floatingLaneCoverDragActive = false;
  floatingLaneCoverDragChanged = false;
  floatingLaneCoverFinger = -1;
  floatingLaneCoverDragOffsetY = 0.0F;
  persistFloatingLaneCoverSettings();
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
  if (!context.saveSettings()) {
    SDL_Log("Failed to save lane cover drag settings");
  }
}

void GamePlayScene::refreshLaneCoverHispeedFactor() {
  // LaneRenderer.setLanecover() calls resetHispeed(basebpm), whose formula
  // includes cover only while it is enabled. `setEnableLanecover()` itself
  // deliberately leaves the live Hi-Speed untouched.
  playfieldLaneCoverHispeedFactor =
      playfieldLaneCoverEnabled
          ? 1.0F - static_cast<float>(playfieldLaneCoverPercent) / 100.0F
          : 1.0F;
}

void GamePlayScene::appendReplayTouchSample(SDL_FingerID fingerIndex,
                                            ReplayTouchAction action,
                                            Vector3 normalizedLocation,
                                            long long songTimeMicros) {
  if (!shouldRecordReplay() || state == nullptr || !state->isPlaying ||
      state->isEnding || context.jukebox.isPaused() ||
      !practiceInputAllowed(songTimeMicros)) {
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
    if (deltaMicros >= 0 && deltaMicros < kReplayTouchMoveMinIntervalMicros &&
        dx * dx + dy * dy <
            kReplayTouchMoveMinDistance * kReplayTouchMoveMinDistance) {
      return;
    }
  }

  recordedReplay.touchSamples.push_back(sample);
  if (action == ReplayTouchAction::Up || action == ReplayTouchAction::Cancel) {
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
  if (!judge.allowsNote(note)) {
    return JudgeResult(None, 0);
  }
  if (note->Wav != bms_parser::Parser::NoWav && !options.autoKeySound &&
      !isReplayPlayback()) {
    context.jukebox.playKeySound(note->Wav);
  }
  const long long eventSongTimeMicros =
      songTimeMicros >= 0 ? songTimeMicros : pressedTime;
  const JudgeResult judgeResult =
      precomputedJudge != nullptr
                                      ? *precomputedJudge
                                      : rulesetPolicyBuild.policy->judge.judgeAt(
                gameplay::judgeRoleFor(note, chart->Meta, options.longNoteMode),
                note->Timeline->Timing, pressedTime);
  if (judgeResult.judgement != None) {
    if (judgeResult.isNotePlayed()) {
      // TODO: play keybomb
      if (note->IsLongNote()) {
        if (const auto &longNote = static_cast<bms_parser::LongNote *>(note);
            !longNote->IsTail()) {
          longNote->Press(pressedTime);
          const bool chargeLongNote =
              effectiveLongNoteIsCharge(longNote, chart, options.longNoteMode);
          if (chargeLongNote) {
            onJudge(judgeResult, judgeEventClock(eventSongTimeMicros),
                    !options.autoPlay || isReplayPlayback());
          }
          if (recordEvent) {
            appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                              eventSongTimeMicros,
                              pressedTime, judgeResult);
          }
        }
        return judgeResult;
      }
      note->Press(pressedTime);
    }
    onJudge(judgeResult, judgeEventClock(eventSongTimeMicros),
            !options.autoPlay || isReplayPlayback());
    if (recordEvent) {
      appendReplayEvent(ReplayEventAction::Press, note->Lane, note,
                        eventSongTimeMicros,
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
  if (!judge.allowsNote(Note) || !Note->IsLongNote()) {
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
  const long long eventSongTimeMicros =
      songTimeMicros >= 0 ? songTimeMicros : ReleasedTime;
  const auto judgeResult =
      precomputedJudge != nullptr
                               ? *precomputedJudge
                               : rulesetPolicyBuild.policy->judge.judgeAt(
                gameplay::judgeRoleFor(LongNote, chart->Meta,
                                         options.longNoteMode),
                LongNote->Timeline->Timing, ReleasedTime);
  JudgeResult appliedJudge(None, 0);
  const bool chargeLongNote =
      effectiveLongNoteIsCharge(LongNote, chart, options.longNoteMode);
  if (precomputedJudge != nullptr) {
    appliedJudge = *precomputedJudge;
  } else {
    appliedJudge = chargeLongNote
            ? normalizeLongNoteReleaseJudge(judgeResult)
            : judgeClassicLongNoteRelease(
                  rulesetPolicyBuild.policy->judge, chart->Meta,
                  options.longNoteMode, LongNote, ReleasedTime);
  }
  onJudge(appliedJudge, judgeEventClock(eventSongTimeMicros),
          !options.autoPlay || isReplayPlayback());
  if (recordEvent) {
    appendReplayEvent(ReplayEventAction::Release, Note->Lane, Note,
                      eventSongTimeMicros,
                      ReleasedTime, appliedJudge);
  }
  return appliedJudge;
}

EventHandleResult GamePlayScene::handleEvents(SDL_Event &event) {
  if (playbackInitializationFailed) {
    Scene::handleEvents(event);
    return {};
  }
  if (handleCoursePauseButtonEvent(event)) {
    return {};
  }

  Scene::handleEvents(event);
  if (event.type == SDL_KEYDOWN) {
    if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_ESCAPE &&
        !escapeHandledByInputPipeline) {
      togglePauseMenuFromInput();
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
  // Gauge authority is captured atomically in PlayfieldAuthorityUpdate.
}
