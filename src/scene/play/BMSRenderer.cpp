//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"
#include "GamePlayTiming.h"
#include "GameplayScrollGeometry.h"
#include "JudgementTimingText.h"
#include "LaneCoverNumberGeometry.h"
#include "StartLaneIndicatorGeometry.h"
#include "TouchVisualizationTiming.h"

#include "../../CoursePlaySession.h"
#include "GameplayGeometry.h"
#include "Judge.h"
#include "../../RAII.h"
#include "bgfx/bgfx.h"
#include "../../rendering/common.h"
#include "../../utils/SpriteLoader.h"
#include "../../view/ClearLampColors.h"
#include "../../view/UiTheme.h"

#include <assert.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
constexpr long long kDefaultLatePoorTimingMicros = 200000LL;
constexpr long long kJudgementTimingTextLingerMicros = 1000000LL;
constexpr const char *kHudFontPath = "assets/fonts/notosanscjkjp.ttf";
constexpr size_t kHudCounterItemCount = 7;
constexpr float kTouchPointMinRadius = 26.0f;
constexpr float kTouchPointMaxRadius = 58.0f;
constexpr float kTouchPointRadiusScale = 0.035f;
constexpr long long kTouchPointReleaseLingerMicros = 180000LL;
constexpr float kTouchPointReleasePulseScale = 0.42f;
constexpr float kHudMargin = 28.0f;
constexpr float kPauseButtonLeftOffset = 88.0f;
constexpr float kPauseButtonTop = 38.0f;
constexpr float kPauseButtonSize = 52.0f;
constexpr float kAutoPlayMarkGap = 12.0f;
constexpr float kAutoPlayMarkMinWidth = 150.0f;
constexpr float kAutoPlayMarkMaxWidth = 260.0f;
constexpr int kAnimatedLongBodyFrameCount = 24;
constexpr int kAnimatedLongBodyCycleMs = 266;
constexpr int kHellChargeDamageCycleMs = 100;
constexpr double kLaneBeamMaxAlpha = 0.2;
constexpr double kScratchLaneBeamHeldAlpha = 0.08;
constexpr long long kScratchLaneBeamHeldFadeMicros = 180000LL;
constexpr long long kLaneBeamReleaseFadeMicros = 200000LL;

constexpr std::array<const char *, kHudCounterItemCount> kCounterLabels{
    "PGREAT", "GREAT", "GOOD", "BAD", "POOR", "KPOOR", "BREAK"};

struct JudgementCounterLayout {
  bool horizontal = true;
  float x = 0.0f;
  float y = 0.0f;
  float itemWidth = 0.0f;
  float itemHeight = 0.0f;
  float gap = 0.0f;
};

long long latePoorTimingFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  const auto it = windows.find(Bad);
  return it == windows.end() ? kDefaultLatePoorTimingMicros : it->second.second;
}

int skinAnimationFrame(long long timeMicros, int frameCount, int cycleMs) {
  if (frameCount <= 1 || cycleMs <= 0 || timeMicros < 0) {
    return 0;
  }
  const long long timeMs = timeMicros / 1000LL;
  const long long cycle = static_cast<long long>(cycleMs);
  const long long timeInCycle = timeMs % cycle;
  return static_cast<int>((timeInCycle * frameCount / cycle) % frameCount);
}

uint8_t scaledAlpha(uint8_t alpha, float scale) {
  return static_cast<uint8_t>(
      std::clamp(std::lround(static_cast<float>(alpha) * scale), 0L, 255L));
}

double laneBeamFadeProgress(long long elapsedMicros, long long durationMicros) {
  if (durationMicros <= 0) {
    return 1.0;
  }
  return std::clamp(static_cast<double>(std::max(0LL, elapsedMicros)) /
                        static_cast<double>(durationMicros),
                    0.0, 1.0);
}

double scratchLaneBeamPressedAlpha(long long elapsedMicros) {
  const double progress =
      laneBeamFadeProgress(elapsedMicros, kScratchLaneBeamHeldFadeMicros);
  return kLaneBeamMaxAlpha -
         (kLaneBeamMaxAlpha - kScratchLaneBeamHeldAlpha) * progress;
}

double laneBeamReleaseAlpha(double startAlpha, long long elapsedMicros) {
  return startAlpha *
         (1.0 -
          laneBeamFadeProgress(elapsedMicros, kLaneBeamReleaseFadeMicros));
}

float oneDrawablePixelInUi(float scale) {
  if (!std::isfinite(scale) || scale <= 0.0f) {
    return 1.0f;
  }
  return 1.0f / scale;
}

float hudHairlineWidth() {
  return std::max(oneDrawablePixelInUi(rendering::ui_scale_x),
                  oneDrawablePixelInUi(rendering::ui_scale_y));
}

bool wasLongNoteTailReleasedEarly(const bms_parser::LongNote *head) {
  if (head == nullptr || head->Tail == nullptr || !head->Tail->IsPlayed ||
      head->Tail->Timeline == nullptr) {
    return false;
  }
  return head->Tail->PlayedTime < head->Tail->Timeline->Timing;
}

bool wasLongNoteTailMissedWithHead(const bms_parser::LongNote *head) {
  return head != nullptr && head->IsDead && head->Tail != nullptr &&
         !head->Tail->IsDead && wasLongNoteTailReleasedEarly(head);
}

bool wasLongNoteTailResolvedAtOrAfterTiming(const bms_parser::LongNote *head) {
  if (head == nullptr || head->Tail == nullptr || !head->Tail->IsPlayed ||
      head->Tail->Timeline == nullptr) {
    return false;
  }
  return head->Tail->PlayedTime >= head->Tail->Timeline->Timing;
}

bool shouldKeepDeadLongNoteBody(const bms_parser::LongNote *head) {
  return head != nullptr && !head->IsTail() && head->IsDead &&
         head->Tail != nullptr && head->Tail->Timeline != nullptr &&
         !head->Tail->IsDead &&
         !wasLongNoteTailResolvedAtOrAfterTiming(head);
}

gameplay_scroll_geometry::NoteRectangleClip noteRenderClip(
    const bms_parser::Note *note, long long currentTimeMicros, float y,
    float noteHeight, float judgeY) {
  if (note == nullptr || note->Timeline == nullptr) {
    return {.visible = true,
            .y = y,
            .height = noteHeight,
            .bottomTextureFraction = 1.0F};
  }
  return gameplay_scroll_geometry::clipNoteRectangle(
      note->Timeline->Timing, currentTimeMicros, y, noteHeight, judgeY);
}

float clippedBottomV(float topV, float bottomV,
                     float bottomTextureFraction) {
  return topV + (bottomV - topV) * bottomTextureFraction;
}

void destroyTextureHandle(bgfx::TextureHandle &texture) {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
    texture = BGFX_INVALID_HANDLE;
  }
}

void destroyNoteSheet(NoteSheet &sheet) {
  destroyTextureHandle(sheet.texture);
  destroyTextureHandle(sheet.longBodyOffTexture);
  destroyTextureHandle(sheet.longBodyOnTexture);
  destroyTextureHandle(sheet.hellChargeBodyOffTexture);
  destroyTextureHandle(sheet.hellChargeBodyOnTexture);
  destroyTextureHandle(sheet.hellChargeDamageTexture);
}

Color hudPanelFill() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(250, 254, 255, 174)
             : Color(7, 13, 22, 158);
}

Color hudPanelStrongFill() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(255, 255, 255, 206)
             : Color(10, 18, 30, 190);
}

Color hudPanelBorder() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(60, 132, 138, 82)
             : Color(110, 219, 213, 74);
}

Color hudCounterAccent(size_t index) {
  switch (index) {
  case 0:
    return ui_theme::cyan();
  case 1:
    return ui_theme::lime();
  case 2:
    return ui_theme::amber();
  case 3:
    return Color(255, 132, 96, 255);
  case 4:
    return ui_theme::coral();
  case 5:
    return Color(255, 78, 102, 255);
  case 6:
    return ui_theme::coral();
  default:
    return ui_theme::textPrimary();
  }
}

Color hudCounterFill(size_t index, bool active, bool topPosition) {
  if (active) {
    if (topPosition) {
      return ui_theme::activeMode() == ui_theme::ThemeMode::Light
                 ? Color(255, 255, 255, 30)
                 : Color(4, 9, 16, 42);
    }
    return ui_theme::activeMode() == ui_theme::ThemeMode::Light
               ? Color(255, 255, 255, 226)
               : Color(3, 8, 14, 218);
  }

  const Color accent = hudCounterAccent(index);
  const uint8_t alpha = topPosition ? 0 : 7;
  return Color(accent.r, accent.g, accent.b, alpha);
}

Color hudCounterBorder(size_t index, bool active, bool topPosition) {
  const Color accent = hudCounterAccent(index);
  if (active) {
    const uint8_t alpha =
        topPosition
            ? (ui_theme::activeMode() == ui_theme::ThemeMode::Light ? 72 : 92)
            : (ui_theme::activeMode() == ui_theme::ThemeMode::Light ? 188
                                                                    : 214);
    return Color(accent.r, accent.g, accent.b, alpha);
  }

  return Color(accent.r, accent.g, accent.b, topPosition ? 8 : 18);
}

Color hudCounterLabelColor(size_t index, int value, bool topPosition) {
  if (value <= 0) {
    const Color muted = ui_theme::textMuted();
    return Color(muted.r, muted.g, muted.b, 16);
  }
  if (topPosition) {
    const Color accent = hudCounterAccent(index);
    return Color(accent.r, accent.g, accent.b, 210);
  }
  const Color text = ui_theme::textSecondary();
  return Color(text.r, text.g, text.b,
               ui_theme::activeMode() == ui_theme::ThemeMode::Light ? 238
                                                                    : 248);
}

Color hudCounterValueColor(size_t index, int value, bool topPosition) {
  if (value <= 0) {
    const Color muted = ui_theme::textMuted();
    return Color(muted.r, muted.g, muted.b, 20);
  }
  if (topPosition) {
    const Color accent = hudCounterAccent(index);
    return Color(accent.r, accent.g, accent.b, 250);
  }
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? ui_theme::darkText()
             : Color(248, 253, 255, 255);
}

Color hudJudgementAccent(Judgement judgement) {
  switch (judgement) {
  case PGreat:
    return hudCounterAccent(0);
  case Great:
    return hudCounterAccent(1);
  case Good:
    return hudCounterAccent(2);
  case Bad:
    return hudCounterAccent(3);
  case Poor:
    return hudCounterAccent(4);
  case Kpoor:
    return hudCounterAccent(5);
  case None:
  case JudgementCount:
    break;
  }
  return ui_theme::textPrimary();
}

Color hudJudgementTextColor(Judgement judgement) {
  const Color accent = hudJudgementAccent(judgement);
  return Color(accent.r, accent.g, accent.b, 255);
}

Color hudJudgementComboColor(Judgement judgement) {
  const Color accent = hudJudgementAccent(judgement);
  return Color(accent.r, accent.g, accent.b, judgement == None ? 168 : 242);
}

Color hudFastColor() { return ui_theme::fastFeedback(); }

Color hudSlowColor() { return ui_theme::slowFeedback(); }

Color hudTimingColor(long long diffMicros) {
  return diffMicros < 0 ? hudFastColor() : hudSlowColor();
}

int judgementTimingRank(Judgement judgement) {
  switch (judgement) {
  case PGreat:
    return 0;
  case Great:
    return 1;
  case Good:
    return 2;
  case Bad:
    return 3;
  case Kpoor:
  case Poor:
    return 4;
  case None:
  case JudgementCount:
    break;
  }
  return 99;
}

int judgementTimingCriteriaRank(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  switch (criteria) {
  case AppSettings::JudgementTimingDisplayCriteria::PGreatOrBelow:
    return 0;
  case AppSettings::JudgementTimingDisplayCriteria::GreatOrBelow:
    return 1;
  case AppSettings::JudgementTimingDisplayCriteria::GoodOrBelow:
    return 2;
  case AppSettings::JudgementTimingDisplayCriteria::BadOrBelow:
    return 3;
  case AppSettings::JudgementTimingDisplayCriteria::Off:
    return 100;
  }
  return 1;
}

bool judgementMeetsTimingCriteria(
    Judgement judgement, AppSettings::JudgementTimingDisplayCriteria criteria) {
  return judgementTimingRank(judgement) >= judgementTimingCriteriaRank(criteria);
}

int counterValueAt(const JudgementCounterSnapshot &snapshot, size_t index) {
  switch (index) {
  case 0:
    return snapshot.pgreat;
  case 1:
    return snapshot.great;
  case 2:
    return snapshot.good;
  case 3:
    return snapshot.bad;
  case 4:
    return snapshot.poor;
  case 5:
    return snapshot.kpoor;
  case 6:
    return snapshot.comboBreak;
  default:
    return 0;
  }
}

int counterIndexForJudgement(Judgement judgement) {
  switch (judgement) {
  case PGreat:
    return 0;
  case Great:
    return 1;
  case Good:
    return 2;
  case Bad:
    return 3;
  case Poor:
    return 4;
  case Kpoor:
    return 5;
  case None:
  case JudgementCount:
    break;
  }
  return -1;
}

int judgeCountFor(const std::map<Judgement, int> &counts,
                  Judgement judgement) {
  const auto it = counts.find(judgement);
  return it == counts.end() ? 0 : it->second;
}

void placeText(TextView *text, int x, int y, int width, int height) {
  if (text == nullptr) {
    return;
  }
  text->setPosition(x, y);
  text->setSize(width, height);
}

void placeQuarterTurnText(TextView *text, int x, int y, int width,
                          int height) {
  const float centerX = static_cast<float>(x) + width * 0.5f;
  const float centerY = static_cast<float>(y) + height * 0.5f;
  placeText(text,
            static_cast<int>(std::round(centerX - height * 0.5f)),
            static_cast<int>(std::round(centerY - width * 0.5f)), height,
            width);
}

float baseGameplayHudTitleWidth() {
  return std::clamp(static_cast<float>(rendering::window_width) * 0.30f,
                    430.0f, 620.0f);
}

float gameplayHudMetricsWidth() {
  return std::clamp(static_cast<float>(rendering::window_width) * 0.28f,
                    430.0f, 540.0f);
}

float gameplayHudMetricsHeight(bool showPacemaker) {
  return showPacemaker ? 78.0f : 58.0f;
}

float gameplayHudMetricsY(bool showPacemaker) {
  return static_cast<float>(rendering::window_height) - kHudMargin -
         gameplayHudMetricsHeight(showPacemaker);
}

std::string formatSignedScoreDelta(int delta) {
  return (delta >= 0 ? "+" : "") + std::to_string(delta);
}

std::string formatGaugeBarLabel(GaugeType gaugeType,
                                GaugeProfile gaugeProfile,
                                GaugeAutoShiftMode gaugeAutoShift,
                                float currentGauge) {
  char text[64];
  const std::string prefix = gaugeAutoShiftEnabled(gaugeAutoShift)
                                 ? std::string(gaugeAutoShiftShortLabel(
                                       gaugeAutoShift)) +
                                       " "
                                 : "";
  std::snprintf(text, sizeof(text), "%s%s %.1f%%", prefix.c_str(),
                gaugeDisplayShortLabel(gaugeType, gaugeProfile),
                currentGauge);
  return text;
}

std::string formatGaugePercent(float currentGauge) {
  char text[16];
  std::snprintf(text, sizeof(text), "%.1f%%", currentGauge);
  return text;
}

Color gaugeAccentColor(GaugeType gaugeType, GaugeProfile gaugeProfile,
                       float currentGauge) {
  const float border = gaugeBorderValue(gaugeType, gaugeProfile);
  if (!gaugeIsSurvival(gaugeType, gaugeProfile) && border > 0.0f &&
      currentGauge < border) {
    return ui_theme::coral();
  }
  return clearLampColorForRank(
      gaugeTypeToClearRank(gaugeClearTypeForProfile(gaugeType, gaugeProfile)));
}

Color gaugeTrackFill() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(255, 255, 255, 72)
             : Color(4, 9, 15, 126);
}

Color gaugeTrackBorder(const Color &accent) {
  return Color(accent.r, accent.g, accent.b,
               ui_theme::activeMode() == ui_theme::ThemeMode::Light ? 164
                                                                    : 190);
}

Color gaugeFillColor(const Color &accent) {
  return Color(accent.r, accent.g, accent.b,
               ui_theme::activeMode() == ui_theme::ThemeMode::Light ? 218
                                                                    : 232);
}

Color gaugeMarkerColor() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(20, 38, 42, 168)
             : Color(244, 250, 255, 174);
}

Color gaugeReducedDamageMarkerColor() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? Color(180, 112, 8, 218)
             : Color(255, 210, 92, 228);
}

Color gaugeTextColor(const Color &accent) {
  return ui_theme::textOn(gaugeFillColor(accent));
}

std::optional<std::pair<float, float>> projectWorldToUi(float worldX,
                                                        float worldY) {
  const Camera &camera = rendering::game_camera;
  if (camera.getViewWidth() == 0 || camera.getViewHeight() == 0) {
    return std::nullopt;
  }

  const bx::Vec3 screen = camera.project({worldX, worldY, 0.0f});
  if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) {
    return std::nullopt;
  }

  const float uiX =
      (screen.x - static_cast<float>(camera.getViewX())) /
      static_cast<float>(camera.getViewWidth()) *
      static_cast<float>(rendering::window_width);
  const float uiY =
      (screen.y - static_cast<float>(camera.getViewY())) /
      static_cast<float>(camera.getViewHeight()) *
      static_cast<float>(rendering::window_height);
  if (!std::isfinite(uiX) || !std::isfinite(uiY)) {
    return std::nullopt;
  }
  return std::make_pair(uiX, uiY);
}

JudgementCounterLayout judgementCounterLayoutFor(
    AppSettings::JudgementCounterPosition position, float titleWidth,
    float rightReserveLeft, bool compactSideCounter) {
  JudgementCounterLayout layout;
  layout.horizontal = position == AppSettings::JudgementCounterPosition::Top;
  layout.gap = layout.horizontal ? 8.0f : 6.0f;
  layout.itemWidth =
      layout.horizontal
          ? std::clamp((static_cast<float>(rendering::window_width) - 72.0f -
                        layout.gap * 6.0f) /
                           7.0f,
                       92.0f, 118.0f)
          : (compactSideCounter ? 96.0f : 118.0f);
  layout.itemHeight = layout.horizontal ? 58.0f : 50.0f;

  const float totalWidth =
      layout.horizontal ? layout.itemWidth * 7.0f + layout.gap * 6.0f
                        : layout.itemWidth;
  const float totalHeight =
      layout.horizontal ? layout.itemHeight
                        : layout.itemHeight * 7.0f + layout.gap * 6.0f;

  switch (position) {
  case AppSettings::JudgementCounterPosition::Top: {
    layout.x = (static_cast<float>(rendering::window_width) - totalWidth) *
               0.5f;
    layout.y = 28.0f;
    const float titleRight = 28.0f + titleWidth;
    if (layout.x < titleRight + 16.0f ||
        layout.x + totalWidth > rightReserveLeft) {
      layout.y = 124.0f;
    }
    break;
  }
  case AppSettings::JudgementCounterPosition::Left:
    layout.x = 28.0f;
    layout.y = std::max(
        126.0f, (static_cast<float>(rendering::window_height) - totalHeight) *
                    0.5f);
    break;
  case AppSettings::JudgementCounterPosition::Right:
    layout.x =
        static_cast<float>(rendering::window_width) - 28.0f - totalWidth;
    layout.y = std::max(
        126.0f, (static_cast<float>(rendering::window_height) - totalHeight) *
                    0.5f);
    break;
  }

  return layout;
}
} // namespace

AtomicLaneState::AtomicLaneState(AtomicLaneState &&other) noexcept {
  *this = std::move(other);
}

AtomicLaneState &
AtomicLaneState::operator=(AtomicLaneState &&other) noexcept {
  lastStateTime.store(other.lastStateTime.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
  lastPressedTime.store(other.lastPressedTime.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  isPressed.store(other.isPressed.load(std::memory_order_relaxed),
                  std::memory_order_relaxed);
  lastPressedJudgement.store(
      other.lastPressedJudgement.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
  lastPressedDiff.store(other.lastPressedDiff.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
  return *this;
}

BMSRenderer::BMSRenderer(
    bms_parser::Chart *chart,
    const std::map<Judgement, std::pair<long long, long long>> &timingWindows,
    int visibleTimeGreenNumber, bool renderHud,
    audio::PlaybackRate playbackRate)
    : judgementIndicator(timingWindows),
      latePoorTiming(latePoorTimingFromWindows(timingWindows)),
      visibleTimeGreenNumber(visibleTimeGreenNumber),
      playbackRate(playbackRate), renderHud(renderHud), chart(chart) {
  setCurrentBpm(chart != nullptr ? chart->Meta.Bpm : 0.0);
  auto textureGuard = makeScopeExit([this] { destroyNoteSheetTextures(); });

  scratchLaneCount = chart->Meta.GetScratchLaneCount();
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  whiteKeyLaneIndices.reserve(laneOrder.size());
  blueKeyLaneIndices.reserve(laneOrder.size());
  scratchLaneIndices.reserve(2);
  startLaneIndicatorColorRoles.reserve(laneOrder.size());
  std::vector<int> keyLanes;
  keyLanes.reserve(laneOrder.size());
  for (size_t i = 0; i < laneOrder.size(); ++i) {
    const int lane = laneOrder[i];
    laneToOrderIndex.emplace(lane, i);
    if (lane < 0) {
      continue;
    }
    const size_t laneIndex = static_cast<size_t>(lane);
    if (isScratch(lane)) {
      scratchLaneIndices.push_back(laneIndex);
      startLaneIndicatorColorRoles.emplace(
          lane, start_lane_indicator::colorRoleForScratch());
    } else {
      keyLanes.push_back(lane);
    }
  }
  std::unordered_map<int, bool> laneUsesBlueSheet;
  laneUsesBlueSheet.reserve(keyLanes.size());
  for (size_t keyPosition = 0; keyPosition < keyLanes.size(); ++keyPosition) {
    const int lane = keyLanes[keyPosition];
    if (lane < 0) {
      continue;
    }
    const auto colorRole =
        start_lane_indicator::colorRoleForKey(keyPosition, keyLanes.size());
    const bool usesBlue = colorRole == start_lane_indicator::ColorRole::Blue;
    laneUsesBlueSheet.emplace(lane, usesBlue);
    startLaneIndicatorColorRoles.emplace(lane, colorRole);
    const size_t laneIndex = static_cast<size_t>(lane);
    if (usesBlue) {
      blueKeyLaneIndices.push_back(laneIndex);
    } else {
      whiteKeyLaneIndices.push_back(laneIndex);
    }
  }
  state.orphanLongNotes.reserve(laneOrder.size() * 2);
  longNoteLookaheadScratch.reserve(laneOrder.size() * 2);
  // flatten timeline
  size_t timelineCount = 0;
  for (const auto &measure : chart->Measures) {
    timelineCount += measure->TimeLines.size();
  }
  timelines.reserve(timelineCount);
  groupedTimelineNotes.reserve(timelineCount);
  // Beatoraja traverses only timing, section, and note-bearing rows. Omitting
  // BGA-only rows is part of its scroll-gimmick geometry because each retained
  // row participates in the incremental future-Y calculation below.
  double previousBpm = chart->Meta.Bpm;
  double previousScroll = 1.0;
  for (const auto &measure : chart->Measures) {
    for (const auto &timeLine : measure->TimeLines) {
      std::vector<bms_parser::Note *> timelineNotes;
      timelineNotes.reserve(laneOrder.size());
      auto appendLaneGroup = [&](const std::vector<size_t> &laneGroup) {
        for (size_t laneIndex : laneGroup) {
          if (laneIndex >= timeLine->Notes.size()) {
            continue;
          }
          if (auto *note = timeLine->Notes[laneIndex]; note != nullptr) {
            timelineNotes.push_back(note);
          }
        }
      };
      appendLaneGroup(whiteKeyLaneIndices);
      appendLaneGroup(blueKeyLaneIndices);
      appendLaneGroup(scratchLaneIndices);

      const bool hasPlayableNote =
          std::any_of(timeLine->Notes.begin(), timeLine->Notes.end(),
                      [](const auto *note) { return note != nullptr; });
      const bool hasInvisibleNote =
          std::any_of(timeLine->InvisibleNotes.begin(),
                      timeLine->InvisibleNotes.end(),
                      [](const auto *note) { return note != nullptr; });
      const bool hasLandmine =
          std::any_of(timeLine->LandmineNotes.begin(),
                      timeLine->LandmineNotes.end(),
                      [](const auto *note) { return note != nullptr; });
      const bool keep = gameplay_scroll_geometry::shouldKeepRenderTimeline(
          previousBpm, timeLine->Bpm, timeLine->GetStopDuration(),
          previousScroll, timeLine->Scroll, timeLine->IsFirstInMeasure,
          hasPlayableNote, hasInvisibleNote, hasLandmine);
      previousBpm = timeLine->Bpm;
      previousScroll = timeLine->Scroll;
      if (!keep) {
        continue;
      }

      timelines.push_back(timeLine);
      groupedTimelineNotes.push_back(std::move(timelineNotes));
    }
  }
  buildTimelineScrollPositions();
  mostPrevalentBpm = calculateMostPrevalentBpm();
  SpriteLoader spriteLoader(PATH("assets/img/simple_gray.png"));
  if (!spriteLoader.load()) {
    throw std::runtime_error("Failed to load simple_gray.png");
  }
  constexpr int width = 128;
  constexpr int height = 40;
  noteImageHeight = height;
  noteImageWidth = width;
  if (!laneOrder.empty()) {
    const int maxLane = *std::max_element(laneOrder.begin(), laneOrder.end());
    if (maxLane >= 0) {
      laneXLookup.assign(static_cast<size_t>(maxLane + 1),
                         std::numeric_limits<float>::quiet_NaN());
      laneSheetLookup.assign(static_cast<size_t>(maxLane + 1), nullptr);
      for (int lane : laneOrder) {
        if (lane < 0) {
          continue;
        }
        const size_t laneIndex = static_cast<size_t>(lane);
        if (isScratch(lane)) {
          laneSheetLookup[laneIndex] = &scratchSheet;
        } else {
          const auto colorIt = laneUsesBlueSheet.find(lane);
          laneSheetLookup[laneIndex] =
              colorIt != laneUsesBlueSheet.end() && colorIt->second ? &blueSheet
                                                                     : &graySheet;
        }
      }
    }
  }
  rebuildPlayAreaGeometry();

  SpriteLoader spriteLoader2(PATH("assets/img/simple_blue.png"));
  if (!spriteLoader2.load()) {
    throw std::runtime_error("Failed to load simple_blue.png");
  }

  SpriteLoader spriteLoader3(PATH("assets/img/orange.png"));
  if (!spriteLoader3.load()) {
    throw std::runtime_error("Failed to load orange.png");
  }

  graySheet.texture = loadSheetTexture(spriteLoader, "simple_gray");
  blueSheet.texture = loadSheetTexture(spriteLoader2, "simple_blue");
  scratchSheet.texture = loadSheetTexture(spriteLoader3, "orange");
  graySheet.longBodyOffTexture =
      loadCroppedTexture(spriteLoader, 0, 120, 128, 12, "gray long body off");
  graySheet.longBodyOnTexture =
      loadCroppedTexture(spriteLoader, 0, 132, 128, 24, "gray long body on");
  graySheet.hellChargeBodyOffTexture = loadCroppedTexture(
      spriteLoader, 0, 236, 128, 12, "gray hell charge body off");
  graySheet.hellChargeBodyOnTexture = loadCroppedTexture(
      spriteLoader, 0, 248, 128, 24, "gray hell charge body on");
  graySheet.hellChargeDamageTexture = loadCroppedTexture(
      spriteLoader, 0, 272, 128, 24, "gray hell charge body damage");
  blueSheet.longBodyOffTexture =
      loadCroppedTexture(spriteLoader2, 0, 120, 128, 12, "blue long body off");
  blueSheet.longBodyOnTexture =
      loadCroppedTexture(spriteLoader2, 0, 132, 128, 24, "blue long body on");
  blueSheet.hellChargeBodyOffTexture = loadCroppedTexture(
      spriteLoader2, 0, 236, 128, 12, "blue hell charge body off");
  blueSheet.hellChargeBodyOnTexture = loadCroppedTexture(
      spriteLoader2, 0, 248, 128, 24, "blue hell charge body on");
  blueSheet.hellChargeDamageTexture = loadCroppedTexture(
      spriteLoader2, 0, 272, 128, 24, "blue hell charge body damage");
  scratchSheet.longBodyOffTexture = loadCroppedTexture(
      spriteLoader3, 0, 120, 128, 12, "scratch long body off");
  scratchSheet.longBodyOnTexture = loadCroppedTexture(
      spriteLoader3, 0, 132, 128, 24, "scratch long body on");
  scratchSheet.hellChargeBodyOffTexture = loadCroppedTexture(
      spriteLoader3, 0, 236, 128, 12, "scratch hell charge body off");
  scratchSheet.hellChargeBodyOnTexture = loadCroppedTexture(
      spriteLoader3, 0, 248, 128, 24, "scratch hell charge body on");
  scratchSheet.hellChargeDamageTexture = loadCroppedTexture(
      spriteLoader3, 0, 272, 128, 24, "scratch hell charge body damage");

  auto makeUv = [](int x, int y, int w, int h, int textureW, int textureH) {
    NoteUvRegion uv{};
    uv.u0 = static_cast<float>(x) / static_cast<float>(textureW);
    uv.v0 = static_cast<float>(y) / static_cast<float>(textureH);
    uv.u1 = static_cast<float>(x + w) / static_cast<float>(textureW);
    uv.v1 = static_cast<float>(y + h) / static_cast<float>(textureH);
    return uv;
  };

  auto configureSheet = [&](NoteSheet &sheet, int textureW, int textureH) {
    sheet.note = makeUv(0, 0, 128, 40, textureW, textureH);
    sheet.longTail = makeUv(0, 40, 128, 40, textureW, textureH);
    sheet.longHead = makeUv(0, 80, 128, 40, textureW, textureH);
    sheet.longBodyOff = makeUv(0, 120, 128, 12, textureW, textureH);
    sheet.longBodyOn = makeUv(0, 132, 128, 24, textureW, textureH);
    sheet.hellChargeTail = makeUv(0, 156, 128, 40, textureW, textureH);
    sheet.hellChargeHead = makeUv(0, 196, 128, 40, textureW, textureH);
    sheet.hellChargeBodyOff = makeUv(0, 236, 128, 12, textureW, textureH);
    sheet.hellChargeBodyOn = makeUv(0, 248, 128, 24, textureW, textureH);
    sheet.hellChargeDamage = makeUv(0, 272, 128, 24, textureW, textureH);
    sheet.mine = makeUv(0, 296, 128, 40, textureW, textureH);
  };

  configureSheet(graySheet, spriteLoader.getWidth(), spriteLoader.getHeight());
  configureSheet(blueSheet, spriteLoader2.getWidth(),
                 spriteLoader2.getHeight());
  configureSheet(scratchSheet, spriteLoader3.getWidth(),
                 spriteLoader3.getHeight());

  titleText = std::make_unique<TextView>(kHudFontPath, 26);
  titleText->setText(chart->Meta.Title);
  titleText->setAlign(TextView::LEFT);
  titleText->setVAlign(TextView::MIDDLE);
  titleText->setOverflow(TextView::TextOverflow::Marquee);
  titleText->setColor(ui_theme::sdl(ui_theme::textPrimary()));

  judgeText = std::make_unique<TextView>(kHudFontPath, 38);
  judgeText->setAlign(TextView::CENTER);
  judgeText->setVAlign(TextView::MIDDLE);
  judgeText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  judgeText->setOverflow(TextView::TextOverflow::Hidden);
  judgeText->setVisible(false);
  pacemakerDeltaText = std::make_unique<TextView>(kHudFontPath, 32);
  pacemakerDeltaText->setAlign(TextView::CENTER);
  pacemakerDeltaText->setVAlign(TextView::MIDDLE);
  pacemakerDeltaText->setColor(ui_theme::sdl(ui_theme::textMuted()));
  pacemakerDeltaText->setOverflow(TextView::TextOverflow::Hidden);
  pacemakerDeltaText->setVisible(false);
  judgementTimingDirectionText = std::make_unique<TextView>(kHudFontPath, 21);
  judgementTimingDirectionText->setAlign(TextView::LEFT);
  judgementTimingDirectionText->setVAlign(TextView::MIDDLE);
  judgementTimingDirectionText->setColor(ui_theme::sdl(ui_theme::cyan()));
  judgementTimingDirectionText->setOverflow(TextView::TextOverflow::Hidden);
  judgementTimingDirectionText->setVisible(false);
  judgementTimingMsText = std::make_unique<TextView>(kHudFontPath, 21);
  judgementTimingMsText->setAlign(TextView::RIGHT);
  judgementTimingMsText->setVAlign(TextView::MIDDLE);
  judgementTimingMsText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  judgementTimingMsText->setOverflow(TextView::TextOverflow::Hidden);
  judgementTimingMsText->setVisible(false);
  layoutCenteredJudgementText();
  scoreText = std::make_unique<TextView>(kHudFontPath, 34);
  scoreText->setAlign(TextView::LEFT);
  scoreText->setVAlign(TextView::MIDDLE);
  scoreText->setText("SCORE 0");
  scoreText->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  comboText = std::make_unique<TextView>(kHudFontPath, 24);
  comboText->setAlign(TextView::RIGHT);
  comboText->setVAlign(TextView::MIDDLE);
  comboText->setText("COMBO 0");
  comboText->setColor(ui_theme::sdl(ui_theme::lime()));
  pacemakerText = std::make_unique<TextView>(kHudFontPath, 18);
  pacemakerText->setAlign(TextView::CENTER);
  pacemakerText->setVAlign(TextView::MIDDLE);
  pacemakerText->setOverflow(TextView::TextOverflow::Hidden);
  pacemakerText->setText("");
  pacemakerText->setColor(ui_theme::sdl(ui_theme::textMuted()));
  pacemakerText->setVisible(false);
  gaugeText = std::make_unique<TextView>(kHudFontPath, 18);
  gaugeText->setAlign(TextView::CENTER);
  gaugeText->setVAlign(TextView::MIDDLE);
  gaugeTypeBadge = std::make_unique<View>();
  gaugeTypeText = std::make_unique<TextView>(kHudFontPath, 15);
  gaugeTypeText->setAlign(TextView::CENTER);
  gaugeTypeText->setVAlign(TextView::MIDDLE);
  gaugeTypeText->setOverflow(TextView::TextOverflow::Hidden);
  gaugeTypeText->setRotationDegrees(90.0f);
  gaugeAutoShiftText = std::make_unique<TextView>(kHudFontPath, 16);
  gaugeAutoShiftText->setText("GAS");
  gaugeAutoShiftText->setAlign(TextView::CENTER);
  gaugeAutoShiftText->setVAlign(TextView::MIDDLE);
  gaugeAutoShiftText->setOverflow(TextView::TextOverflow::Hidden);
  gaugeAutoShiftText->setRotationDegrees(90.0f);
  setGaugeStatus(GaugeType::Normal, GaugeAutoShiftMode::None,
                 gaugeInitialValue(GaugeType::Normal));
  playOptionText = std::make_unique<TextView>(kHudFontPath, 19);
  playOptionText->setAlign(TextView::LEFT);
  playOptionText->setVAlign(TextView::MIDDLE);
  playOptionText->setOverflow(TextView::TextOverflow::Marquee);
  playOptionText->setColor(ui_theme::sdl(ui_theme::amber()));
  playOptionText->setVisible(false);
  autoPlayMarkText = createAutoPlayMarkText();
  autoPlayMarkText->setVisible(false);
  laneCoverWhiteNumberText = std::make_unique<TextView>(kHudFontPath, 24);
  laneCoverWhiteNumberText->setAlign(TextView::CENTER);
  laneCoverWhiteNumberText->setVAlign(TextView::MIDDLE);
  laneCoverWhiteNumberText->setOverflow(TextView::TextOverflow::Hidden);
  laneCoverWhiteNumberText->setColor(
      ui_theme::sdl(Color(255, 255, 255, 255)));
  laneCoverWhiteNumberText->setVisible(false);
  laneCoverVisibleTimeText = std::make_unique<TextView>(kHudFontPath, 24);
  laneCoverVisibleTimeText->setAlign(TextView::CENTER);
  laneCoverVisibleTimeText->setVAlign(TextView::MIDDLE);
  laneCoverVisibleTimeText->setOverflow(TextView::TextOverflow::Hidden);
  laneCoverVisibleTimeText->setColor(ui_theme::sdl(ui_theme::lime()));
  laneCoverVisibleTimeText->setVisible(false);

  for (size_t i = 0; i < kHudCounterItemCount; ++i) {
    auto &label = judgementCounterLabelTexts[i];
    label = std::make_unique<TextView>(kHudFontPath, 14);
    label->setText(kCounterLabels[i]);
    label->setAlign(TextView::CENTER);
    label->setVAlign(TextView::MIDDLE);
    label->setColor(ui_theme::sdl(ui_theme::textSecondary()));

    auto &value = judgementCounterValueTexts[i];
    value = std::make_unique<TextView>(kHudFontPath, 24);
    value->setText("0");
    value->setAlign(TextView::CENTER);
    value->setVAlign(TextView::MIDDLE);
    value->setColor(ui_theme::sdl(hudCounterAccent(i)));
  }

  refreshGeometry();
  textureGuard.dismiss();
}

bgfx::TextureHandle BMSRenderer::loadSheetTexture(SpriteLoader &loader,
                                                  const char *label) {
  if (!loader.isLoaded() || loader.getData() == nullptr) {
    SDL_Log("Failed to load %s texture: image is not loaded", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  const int width = loader.getWidth();
  const int height = loader.getHeight();
  if (width <= 0 || height <= 0) {
    SDL_Log("Failed to load %s texture: invalid dimensions", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  constexpr int kBytesPerPixel = 4; // stbi_load(..., 4) in SpriteLoader
  const auto handle = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(loader.getData(), width * height * kBytesPerPixel));
  if (!bgfx::isValid(handle)) {
    SDL_Log("Failed to create bgfx texture for %s", label);
    throw std::runtime_error(std::string("Failed to create texture for ") +
                             label);
  }
  return handle;
}

bgfx::TextureHandle BMSRenderer::loadCroppedTexture(SpriteLoader &loader, int x,
                                                    int y, int width,
                                                    int height,
                                                    const char *label) {
  UniqueResource<unsigned char, SDL_free> data(loader.crop(x, y, width, height));
  if (data == nullptr) {
    SDL_Log("Failed to load %s texture", label);
    throw std::runtime_error(std::string("Failed to load ") + label +
                             " texture");
  }
  constexpr int kBytesPerPixel = 4;
  const auto handle = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::RGBA8, 0,
      bgfx::copy(data.get(), width * height * kBytesPerPixel));
  if (!bgfx::isValid(handle)) {
    SDL_Log("Failed to create %s texture", label);
    throw std::runtime_error(std::string("Failed to create ") + label +
                             " texture");
  }
  return handle;
}

std::unique_ptr<TextView> BMSRenderer::createAutoPlayMarkText() {
  auto text = std::make_unique<TextView>(kHudFontPath, 27);
  text->setText("AUTO PLAY");
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
  text->setColor(ui_theme::sdl(ui_theme::textPrimary()));
  text->setBackgroundColor(Color(7, 13, 22, 218));
  text->setBorderColor(ui_theme::amber());
  text->setBorderWidth(1);
  text->setCornerRadius(ui_theme::controlRadius());
  return text;
}

void BMSRenderer::drawTitle(RenderContext &context) const {
  titleText->render(context);
}
void BMSRenderer::drawJudgement(RenderContext context) const {
  if (pacemakerDeltaText != nullptr) {
    pacemakerDeltaText->render(context);
  }
  if (judgementTimingDirectionText != nullptr) {
    judgementTimingDirectionText->render(context);
  }
  if (judgementTimingMsText != nullptr) {
    judgementTimingMsText->render(context);
  }
  judgeText->render(context);
}
void BMSRenderer::drawScore(RenderContext &context) const {
  scoreText->render(context);
  if (pacemakerText != nullptr) {
    pacemakerText->render(context);
  }
}
void BMSRenderer::drawGauge(RenderContext &context) const {
  if (gaugeText != nullptr) {
    gaugeText->render(context);
  }
  if (gaugeTypeBadge != nullptr) {
    gaugeTypeBadge->render(context);
  }
  if (gaugeTypeText != nullptr) {
    gaugeTypeText->render(context);
  }
  if (gaugeAutoShiftText != nullptr) {
    gaugeAutoShiftText->render(context);
  }
}
void BMSRenderer::drawPlayOption(RenderContext &context) const {
  playOptionText->render(context);
}
void BMSRenderer::drawAutoPlayMark(RenderContext &context) const {
  if (!autoPlayMarkVisible || autoPlayMarkText == nullptr) {
    return;
  }
  renderAutoPlayMark(autoPlayMarkText.get(), context);
}

void BMSRenderer::drawHudRoundedPanel(float x, float y, float width,
                                      float height, float radius,
                                      const Color &fill,
                                      const Color &border) {
  drawRoundedPanel(x, y, width, height, radius, hudHairlineWidth(), fill,
                   border);
}

void BMSRenderer::drawRoundedPanel(float x, float y, float width, float height,
                                   float radius, float borderWidth,
                                   const Color &fill, const Color &border) {
  const float kBorder = std::max(0.0f, borderWidth);
  simpleBatchRenderer.addRoundedRect(x, y, width, height, radius,
                                     border.toABGR());
  simpleBatchRenderer.addRoundedRect(
      x + kBorder, y + kBorder, std::max(0.0f, width - kBorder * 2.0f),
      std::max(0.0f, height - kBorder * 2.0f),
      std::max(0.0f, radius - kBorder), fill.toABGR());
}

void BMSRenderer::drawGameplayHudPanels() {
  constexpr float margin = 28.0f;
  constexpr float radius = 12.0f;
  const float titleWidth = gameplayHudTitleWidth();
  const float metricsWidth = gameplayHudMetricsWidth();
  const bool showPacemaker =
      pendingPacemakerEnabled.load(std::memory_order_relaxed);
  const float metricsHeight = gameplayHudMetricsHeight(showPacemaker);

  if (titleWidth > 1.0f) {
    drawHudRoundedPanel(margin, margin, titleWidth, 82.0f, radius,
                        hudPanelFill(), hudPanelBorder());
  }
  drawHudRoundedPanel(margin, gameplayHudMetricsY(showPacemaker), metricsWidth,
                      metricsHeight, radius, hudPanelStrongFill(),
                      hudPanelBorder());
}

std::array<float, 4> BMSRenderer::worldGaugeRect() const {
  const float width = std::max(0.1f, playAreaWidth * 0.84f);
  const float height = std::max(0.055f, noteRenderHeight * 0.34f);
  const float x = playAreaLeftX + (playAreaWidth - width) * 0.5f;
  const float y = judgeY - std::max(0.12f, noteRenderHeight * 0.78f);
  return {x, y, width, height};
}

std::array<float, 4> BMSRenderer::hudGaugeRect() const {
  const float width =
      std::clamp(static_cast<float>(rendering::window_width) * 0.018f, 20.0f,
                 30.0f);
  const float maxHeight =
      std::max(220.0f, static_cast<float>(rendering::window_height) - 260.0f);
  const float height =
      std::min(std::max(220.0f,
                        static_cast<float>(rendering::window_height) * 0.46f),
               maxHeight);
  float y = (static_cast<float>(rendering::window_height) - height) * 0.5f;
  y = std::max(128.0f, y);

  const bool left = gaugeBarPosition == AppSettings::GaugeBarPosition::Left;
  constexpr float kSideBadgeInset = 56.0f;
  float x = left ? kSideBadgeInset
                 : static_cast<float>(rendering::window_width) -
                       kSideBadgeInset - width;
  const bool counterOnSameSide =
      judgementCounterEnabled &&
      ((left && judgementCounterPosition ==
                    AppSettings::JudgementCounterPosition::Left) ||
       (!left && judgementCounterPosition ==
                     AppSettings::JudgementCounterPosition::Right));
  if (counterOnSameSide) {
    x += left ? 132.0f : -132.0f;
  }
  x = std::clamp(x, 12.0f,
                 std::max(12.0f,
                          static_cast<float>(rendering::window_width) - width -
                              12.0f));
  return {x, y, width, height};
}

std::array<float, 4> BMSRenderer::autoPlayMarkRect() {
  const float pauseLeft =
      std::max(kHudMargin, static_cast<float>(rendering::window_width) -
                               kPauseButtonLeftOffset);
  const float availableWidth =
      std::max(1.0f, pauseLeft - kAutoPlayMarkGap - kHudMargin);
  const float preferredWidth =
      std::clamp(static_cast<float>(rendering::window_width) * 0.18f,
                 kAutoPlayMarkMinWidth, kAutoPlayMarkMaxWidth);
  const float width = std::min(preferredWidth, availableWidth);
  const float x =
      std::max(kHudMargin, pauseLeft - kAutoPlayMarkGap - width);
  return {x, kPauseButtonTop, width, kPauseButtonSize};
}

void BMSRenderer::layoutAutoPlayMark(TextView *text) {
  if (text == nullptr) {
    return;
  }
  const auto rect = autoPlayMarkRect();
  text->setPositionNoLayout(static_cast<int>(std::round(rect[0])),
                            static_cast<int>(std::round(rect[1])));
  text->setSize(static_cast<int>(std::round(rect[2])),
                static_cast<int>(std::round(rect[3])));
}

void BMSRenderer::renderAutoPlayMark(TextView *text, RenderContext &context) {
  if (text == nullptr) {
    return;
  }
  layoutAutoPlayMark(text);
  text->render(context);
}

float BMSRenderer::gameplayHudRightReserveLeft() const {
  if (autoPlayMarkVisible) {
    const auto rect = autoPlayMarkRect();
    return std::max(kHudMargin, rect[0] - 16.0f);
  }
  return static_cast<float>(rendering::window_width) -
         kPauseButtonLeftOffset - 16.0f;
}

void BMSRenderer::drawGaugeBar() {
  if (gaugeBarPosition == AppSettings::GaugeBarPosition::World) {
    drawWorldGaugeBar();
  } else {
    drawHudGaugeBar();
  }
}

void BMSRenderer::drawWorldGaugeBar() {
  const auto rect = worldGaugeRect();
  const float x = rect[0];
  const float y = rect[1];
  const float width = rect[2];
  const float height = rect[3];
  const float maximum =
      gaugeMaximumValue(currentGaugeType, currentGaugeProfile);
  const float progress =
      std::clamp(currentGaugeValue, 0.0f, maximum) / maximum;
  const Color accent =
      gaugeAccentColor(currentGaugeType, currentGaugeProfile,
                       currentGaugeValue);
  const float radius = height * 0.5f;
  const float borderWidth = std::max(0.008f, height * 0.12f);

  simpleBatchRenderer.addRoundedRect(x + 0.025f, y - 0.018f, width, height,
                                     radius,
                                     Color(0, 0, 0, 82).toABGR());
  drawRoundedPanel(x, y, width, height, radius, borderWidth, gaugeTrackFill(),
                   gaugeTrackBorder(accent));

  const float fillWidth = width * progress;
  if (fillWidth > 0.0f) {
    simpleBatchRenderer.addRoundedRect(x + borderWidth, y + borderWidth,
                                       std::max(0.0f,
                                                fillWidth - borderWidth * 2.0f),
                                       std::max(0.0f,
                                                height - borderWidth * 2.0f),
                                       std::max(0.0f, radius - borderWidth),
                                       gaugeFillColor(accent).toABGR());
  }

  const float borderValue =
      gaugeBorderValue(currentGaugeType, currentGaugeProfile);
  if (borderValue > 0.0f) {
    const float markerX =
        x + width * std::clamp(borderValue / maximum, 0.0f, 1.0f);
    const float markerWidth = std::max(0.01f, width * 0.004f);
    simpleBatchRenderer.addRect(markerX - markerWidth * 0.5f,
                                y - height * 0.18f, markerWidth,
                                height * 1.36f, gaugeMarkerColor().toABGR());
  }

  const float reducedDamageZone = gaugeReducedDamageZoneUpperBound(
      currentGaugeType, currentGaugeProfile);
  if (reducedDamageZone > 0.0f) {
    const float markerX =
        x + width * std::clamp(reducedDamageZone / maximum, 0.0f, 1.0f);
    const float markerWidth = std::max(0.012f, width * 0.005f);
    simpleBatchRenderer.addRect(
        markerX - markerWidth * 0.5f, y - height * 0.12f, markerWidth,
        height * 1.24f, gaugeReducedDamageMarkerColor().toABGR());
  }
}

void BMSRenderer::drawHudGaugeBar() {
  const auto rect = hudGaugeRect();
  const float x = rect[0];
  const float y = rect[1];
  const float width = rect[2];
  const float height = rect[3];
  const float maximum =
      gaugeMaximumValue(currentGaugeType, currentGaugeProfile);
  const float progress =
      std::clamp(currentGaugeValue, 0.0f, maximum) / maximum;
  const Color accent =
      gaugeAccentColor(currentGaugeType, currentGaugeProfile,
                       currentGaugeValue);
  const float radius = width * 0.5f;

  simpleBatchRenderer.addRoundedRect(x + 3.0f, y + 5.0f, width, height, radius,
                                     Color(0, 0, 0, 48).toABGR());
  drawRoundedPanel(x, y, width, height, radius, 1.0f, gaugeTrackFill(),
                   gaugeTrackBorder(accent));

  const float fillHeight = height * progress;
  if (fillHeight > 0.0f) {
    simpleBatchRenderer.addRoundedRect(
        x + 2.0f, y + height - fillHeight + 2.0f, std::max(0.0f, width - 4.0f),
        std::max(0.0f, fillHeight - 4.0f), std::max(0.0f, radius - 2.0f),
        gaugeFillColor(accent).toABGR());
  }

  const float borderValue =
      gaugeBorderValue(currentGaugeType, currentGaugeProfile);
  if (borderValue > 0.0f) {
    const float markerY =
        y + height * (1.0f - std::clamp(borderValue / maximum, 0.0f, 1.0f));
    simpleBatchRenderer.addRect(x - 5.0f, markerY - 1.0f, width + 10.0f, 2.0f,
                                gaugeMarkerColor().toABGR());
  }

  const float reducedDamageZone = gaugeReducedDamageZoneUpperBound(
      currentGaugeType, currentGaugeProfile);
  if (reducedDamageZone > 0.0f) {
    const float markerProgress =
        std::clamp(reducedDamageZone / maximum, 0.0f, 1.0f);
    const float markerY = y + height * (1.0f - markerProgress);
    simpleBatchRenderer.addRect(
        x - 4.0f, markerY - 1.5f, width + 8.0f, 3.0f,
        gaugeReducedDamageMarkerColor().toABGR());
  }
}

void BMSRenderer::drawJudgementAccentBar() {
  if (renderedJudgement == None || judgeText == nullptr ||
      !judgeText->getVisible()) {
    return;
  }

  const float width = 6.0f;
  const float height =
      std::max(16.0f, static_cast<float>(judgeText->getHeight()) - 28.0f);
  const float y = static_cast<float>(judgeText->getY()) +
                  (static_cast<float>(judgeText->getHeight()) - height) * 0.5f;
  const Color accent = hudJudgementAccent(renderedJudgement);
  const uint32_t color = Color(accent.r, accent.g, accent.b, 210).toABGR();
  const bool showLeft =
      renderedTimingFastShown ||
      (!renderedTimingFastShown && !renderedTimingSlowShown);
  const bool showRight =
      renderedTimingSlowShown ||
      (!renderedTimingFastShown && !renderedTimingSlowShown);
  if (showLeft) {
    const float x =
        std::max(0.0f, static_cast<float>(judgeText->getX()) - 15.0f);
    simpleBatchRenderer.addRoundedRect(x, y, width, height, width * 0.5f,
                                       color);
  }
  if (showRight) {
    const float rightX = static_cast<float>(judgeText->getX() +
                                           judgeText->getWidth()) +
                         9.0f;
    const float x = std::min(
        static_cast<float>(std::max(0, rendering::window_width)) - width,
        rightX);
    simpleBatchRenderer.addRoundedRect(std::max(0.0f, x), y, width, height,
                                       width * 0.5f, color);
  }
}

void BMSRenderer::drawJudgementCounterPanels() {
  constexpr float radius = 10.0f;
  const bool counterSharesGaugeSide =
      (judgementCounterPosition ==
           AppSettings::JudgementCounterPosition::Left &&
       gaugeBarPosition == AppSettings::GaugeBarPosition::Left) ||
      (judgementCounterPosition ==
           AppSettings::JudgementCounterPosition::Right &&
       gaugeBarPosition == AppSettings::GaugeBarPosition::Right);
  const JudgementCounterLayout layout =
      judgementCounterLayoutFor(judgementCounterPosition,
                                gameplayHudTitleWidth(),
                                gameplayHudRightReserveLeft(),
                                counterSharesGaugeSide);
  const bool topPosition =
      judgementCounterPosition == AppSettings::JudgementCounterPosition::Top;
  if (topPosition) {
    return;
  }

  for (size_t i = 0; i < kHudCounterItemCount; ++i) {
    const float itemX =
        layout.x +
        (layout.horizontal ? (layout.itemWidth + layout.gap) * i : 0.0f);
    const float itemY =
        layout.y +
        (layout.horizontal ? 0.0f : (layout.itemHeight + layout.gap) * i);
    const int value = counterValueAt(renderedJudgementCounterSnapshot, i);
    const bool active = value > 0;
    drawHudRoundedPanel(itemX, itemY, layout.itemWidth, layout.itemHeight,
                        radius,
                        hudCounterFill(i, active, topPosition),
                        hudCounterBorder(i, active, topPosition));
  }
}

void BMSRenderer::layoutGameplayHud() {
  constexpr int margin = 28;
  const int titleWidth = static_cast<int>(gameplayHudTitleWidth());
  const bool titleVisible = titleWidth > 48;
  if (titleText != nullptr) {
    titleText->setVisible(titleVisible);
  }
  placeText(titleText.get(), margin + 18, margin + 8,
            std::max(1, titleWidth - 36), 34);
  placeText(playOptionText.get(), margin + 18, margin + 44,
            std::max(1, titleWidth - 36), 26);

  const int metricsWidth = static_cast<int>(gameplayHudMetricsWidth());
  const bool showPacemaker =
      pendingPacemakerEnabled.load(std::memory_order_relaxed);
  const int compactMetricsY =
      static_cast<int>(std::round(gameplayHudMetricsY(showPacemaker)));
  placeText(scoreText.get(), margin + 18, compactMetricsY + 7,
            metricsWidth / 2, 36);
  placeText(comboText.get(), margin + metricsWidth / 2 - 8,
            compactMetricsY + 7, metricsWidth / 2 - 10, 36);
  if (showPacemaker) {
    placeText(pacemakerText.get(), margin + 18, compactMetricsY + 43,
              std::max(1, metricsWidth - 36), 26);
  } else if (pacemakerText != nullptr) {
    placeText(pacemakerText.get(), margin + 18, compactMetricsY + 43, 1, 1);
  }
  layoutGaugeText();
  layoutAutoPlayMark();

  const bool counterSharesGaugeSide =
      (judgementCounterPosition ==
           AppSettings::JudgementCounterPosition::Left &&
       gaugeBarPosition == AppSettings::GaugeBarPosition::Left) ||
      (judgementCounterPosition ==
           AppSettings::JudgementCounterPosition::Right &&
       gaugeBarPosition == AppSettings::GaugeBarPosition::Right);
  const JudgementCounterLayout layout =
      judgementCounterLayoutFor(judgementCounterPosition,
                                gameplayHudTitleWidth(),
                                gameplayHudRightReserveLeft(),
                                counterSharesGaugeSide);
  const int gap = static_cast<int>(layout.gap);
  const int itemWidth = static_cast<int>(layout.itemWidth);
  const int itemHeight = static_cast<int>(layout.itemHeight);
  const int counterX = static_cast<int>(std::round(layout.x));
  const int counterY = static_cast<int>(std::round(layout.y));

  for (size_t i = 0; i < kHudCounterItemCount; ++i) {
    const int itemX =
        counterX +
        (layout.horizontal ? (itemWidth + gap) * static_cast<int>(i) : 0);
    const int itemY =
        counterY +
        (layout.horizontal ? 0 : (itemHeight + gap) * static_cast<int>(i));
    placeText(judgementCounterLabelTexts[i].get(), itemX + 8, itemY + 6,
              itemWidth - 16, layout.horizontal ? 18 : 16);
    placeText(judgementCounterValueTexts[i].get(), itemX + 8,
              itemY + (layout.horizontal ? 24 : 22), itemWidth - 16,
              itemHeight - (layout.horizontal ? 28 : 24));
  }
}

void BMSRenderer::layoutAutoPlayMark() {
  if (autoPlayMarkText == nullptr) {
    return;
  }
  autoPlayMarkText->setVisible(autoPlayMarkVisible);
  layoutAutoPlayMark(autoPlayMarkText.get());
}

void BMSRenderer::layoutGaugeText() {
  if (gaugeText == nullptr) {
    return;
  }

  gaugeText->setVisible(true);
  if (gaugeBarPosition == AppSettings::GaugeBarPosition::World) {
    if (gaugeTypeBadge != nullptr) {
      gaugeTypeBadge->setVisible(false);
    }
    if (gaugeTypeText != nullptr) {
      gaugeTypeText->setVisible(false);
    }
    if (gaugeAutoShiftText != nullptr) {
      gaugeAutoShiftText->setVisible(false);
    }
    const auto rect = worldGaugeRect();
    const auto topLeft = projectWorldToUi(rect[0], rect[1] + rect[3]);
    const auto topRight =
        projectWorldToUi(rect[0] + rect[2], rect[1] + rect[3]);
    const auto bottomLeft = projectWorldToUi(rect[0], rect[1]);
    const auto bottomRight = projectWorldToUi(rect[0] + rect[2], rect[1]);
    if (!topLeft || !topRight || !bottomLeft || !bottomRight) {
      gaugeText->setVisible(false);
      return;
    }

    const float minX = std::min(
        {topLeft->first, topRight->first, bottomLeft->first,
         bottomRight->first});
    const float maxX = std::max(
        {topLeft->first, topRight->first, bottomLeft->first,
         bottomRight->first});
    const float minY = std::min(
        {topLeft->second, topRight->second, bottomLeft->second,
         bottomRight->second});
    const float maxY = std::max(
        {topLeft->second, topRight->second, bottomLeft->second,
         bottomRight->second});
    const int textWidth =
        std::max(120, static_cast<int>(std::round(maxX - minX)));
    const int textHeight =
        std::clamp(static_cast<int>(std::round((maxY - minY) + 12.0f)), 24,
                   42);
    const int x = static_cast<int>(std::round((minX + maxX) * 0.5f)) -
                  textWidth / 2;
    const int y = static_cast<int>(std::round((minY + maxY) * 0.5f)) -
                  textHeight / 2;
    placeText(gaugeText.get(), x, y, textWidth, textHeight);
    return;
  }

  const auto rect = hudGaugeRect();
  constexpr int textWidth = 76;
  constexpr int textHeight = 36;
  const bool left = gaugeBarPosition == AppSettings::GaugeBarPosition::Left;
  const int x = left ? static_cast<int>(std::round(rect[0] + rect[2] + 8.0f))
                     : static_cast<int>(
                           std::round(rect[0] - textWidth - 8.0f));
  const float maximum =
      gaugeMaximumValue(currentGaugeType, currentGaugeProfile);
  const float progress =
      std::clamp(currentGaugeValue, 0.0f, maximum) / maximum;
  const float tipY = rect[1] + rect[3] * (1.0f - progress);
  const int y = std::clamp(
      static_cast<int>(std::round(tipY - textHeight * 0.5f)), 8,
      std::max(8, rendering::window_height - textHeight - 8));
  placeText(gaugeText.get(), x, y, textWidth, textHeight);

  if (gaugeTypeText != nullptr) {
    gaugeTypeText->setVisible(true);
    gaugeTypeText->setRotationDegrees(left ? -90.0f : 90.0f);
    if (gaugeAutoShiftText != nullptr) {
      gaugeAutoShiftText->setVisible(
          gaugeAutoShiftEnabled(currentGaugeAutoShift));
      gaugeAutoShiftText->setRotationDegrees(left ? -90.0f : 90.0f);
    }
    constexpr int typeWidth = 34;
    constexpr int typePadding = 12;
    constexpr int gasGap = 4;
    const int gaugeLabelLength = gaugeTypeText->textureWidth();
    const bool autoShiftEnabled =
        gaugeAutoShiftEnabled(currentGaugeAutoShift);
    const int gasLabelLength = autoShiftEnabled && gaugeAutoShiftText
                                   ? gaugeAutoShiftText->textureWidth()
                                   : 0;
    const int typeHeight =
        std::clamp(gaugeLabelLength + gasLabelLength +
                       (autoShiftEnabled ? gasGap : 0) + typePadding * 2,
                   76, 188);
    const int typeX =
        left ? static_cast<int>(std::round(rect[0] - typeWidth - 8.0f))
             : static_cast<int>(std::round(rect[0] + rect[2] + 8.0f));
    const int typeY = static_cast<int>(
        std::round(rect[1] + (rect[3] - typeHeight) * 0.24f));
    if (gaugeTypeBadge != nullptr) {
      gaugeTypeBadge->setVisible(true);
      gaugeTypeBadge->setPosition(typeX, typeY);
      gaugeTypeBadge->setSize(typeWidth, typeHeight);
    }

    if (!autoShiftEnabled || gaugeAutoShiftText == nullptr) {
      placeQuarterTurnText(gaugeTypeText.get(), typeX, typeY, typeWidth,
                           typeHeight);
    } else {
      const int gaugeSpan = gaugeLabelLength + typePadding;
      const int gasSpan = typeHeight - gaugeSpan;
      if (left) {
        placeQuarterTurnText(gaugeAutoShiftText.get(), typeX, typeY,
                             typeWidth, gasSpan);
        placeQuarterTurnText(gaugeTypeText.get(), typeX, typeY + gasSpan,
                             typeWidth, gaugeSpan);
      } else {
        placeQuarterTurnText(gaugeTypeText.get(), typeX, typeY, typeWidth,
                             gaugeSpan);
        placeQuarterTurnText(gaugeAutoShiftText.get(), typeX,
                             typeY + gaugeSpan, typeWidth, gasSpan);
      }
    }
  }
}

void BMSRenderer::refreshGaugeTextStyle() {
  if (gaugeText == nullptr) {
    return;
  }

  const Color accent =
      gaugeAccentColor(currentGaugeType, currentGaugeProfile,
                       currentGaugeValue);
  if (gaugeBarPosition == AppSettings::GaugeBarPosition::World) {
    gaugeText->setText(formatGaugeBarLabel(
        currentGaugeType, currentGaugeProfile, currentGaugeAutoShift,
        currentGaugeValue));
    gaugeText->setColor(ui_theme::sdl(gaugeTextColor(accent)));
    gaugeText->clearBackgroundColor();
    gaugeText->clearBorderColor();
    gaugeText->setBorderWidth(0);
    gaugeText->setCornerRadius(0.0f);
    if (gaugeTypeText != nullptr) {
      gaugeTypeText->setVisible(false);
    }
    if (gaugeTypeBadge != nullptr) {
      gaugeTypeBadge->setVisible(false);
    }
    if (gaugeAutoShiftText != nullptr) {
      gaugeAutoShiftText->setVisible(false);
    }
    return;
  }

  gaugeText->setText(formatGaugePercent(currentGaugeValue));
  gaugeText->setColor({250, 253, 255, 255});
  gaugeText->setBackgroundColor(Color(4, 8, 15, 238));
  gaugeText->setBorderColor(Color(accent.r, accent.g, accent.b, 224));
  gaugeText->setBorderWidth(1);
  gaugeText->setCornerRadius(9.0f);
  if (gaugeTypeBadge != nullptr) {
    gaugeTypeBadge->setVisible(true);
    gaugeTypeBadge->setBackgroundColor(Color(4, 8, 15, 238));
    gaugeTypeBadge->setBorderColor(Color(accent.r, accent.g, accent.b, 224));
    gaugeTypeBadge->setBorderWidth(1);
    gaugeTypeBadge->setCornerRadius(9.0f);
  }
  if (gaugeTypeText != nullptr) {
    gaugeTypeText->setVisible(true);
    gaugeTypeText->setText(
        gaugeDisplayShortLabel(currentGaugeType, currentGaugeProfile));
    gaugeTypeText->setColor({250, 253, 255, 255});
    gaugeTypeText->clearBackgroundColor();
    gaugeTypeText->clearBorderColor();
    gaugeTypeText->setBorderWidth(0);
  }
  if (gaugeAutoShiftText != nullptr) {
    gaugeAutoShiftText->setText(
        gaugeAutoShiftShortLabel(currentGaugeAutoShift));
    gaugeAutoShiftText->setVisible(
        gaugeAutoShiftEnabled(currentGaugeAutoShift));
    gaugeAutoShiftText->setColor({255, 225, 112, 255});
    gaugeAutoShiftText->setBackgroundColor(Color(255, 200, 64, 32));
    gaugeAutoShiftText->clearBorderColor();
    gaugeAutoShiftText->setBorderWidth(0);
    gaugeAutoShiftText->setCornerRadius(6.0f);
  }
}

float BMSRenderer::gameplayHudTitleWidth() const {
  constexpr float kTitleMargin = 28.0f;
  constexpr float kLaneGap = 18.0f;
  constexpr float kTitleTop = 28.0f;
  constexpr float kTitleBottom = kTitleTop + 82.0f;

  const float baseWidth = baseGameplayHudTitleWidth();
  const float laneLeft = projectedLaneLeftUiInBand(kTitleTop, kTitleBottom);
  if (!std::isfinite(laneLeft)) {
    return baseWidth;
  }

  const float maxWidth = laneLeft - kTitleMargin - kLaneGap;
  return std::clamp(maxWidth, 0.0f, baseWidth);
}

std::optional<std::pair<float, float>>
BMSRenderer::projectLanePointToUi(float worldX, float worldY) const {
  return projectWorldToUi(worldX, worldY);
}

float BMSRenderer::projectedLaneLeftUiInBand(float bandTop,
                                             float bandBottom) const {
  const auto leftBottom = projectLanePointToUi(playAreaLeftX, judgeY);
  const auto rightBottom =
      projectLanePointToUi(playAreaLeftX + playAreaWidth, judgeY);
  const auto leftTop = projectLanePointToUi(playAreaLeftX, upperBound);
  const auto rightTop =
      projectLanePointToUi(playAreaLeftX + playAreaWidth, upperBound);
  if (!leftBottom || !rightBottom || !leftTop || !rightTop) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  float minX = std::numeric_limits<float>::infinity();
  auto considerX = [&minX](float x) {
    if (std::isfinite(x)) {
      minX = std::min(minX, x);
    }
  };
  auto considerPoint = [&](const std::pair<float, float> &point) {
    if (point.second >= bandTop && point.second <= bandBottom) {
      considerX(point.first);
    }
  };
  auto considerEdge = [&](const std::pair<float, float> &a,
                          const std::pair<float, float> &b) {
    considerPoint(a);
    considerPoint(b);

    const float minY = std::min(a.second, b.second);
    const float maxY = std::max(a.second, b.second);
    const float dy = b.second - a.second;
    if (std::abs(dy) <= 0.0001f) {
      if (a.second >= bandTop && a.second <= bandBottom) {
        considerX(std::min(a.first, b.first));
      }
      return;
    }

    auto considerAtY = [&](float targetY) {
      if (targetY < minY || targetY > maxY) {
        return;
      }
      const float t = (targetY - a.second) / dy;
      if (t < 0.0f || t > 1.0f) {
        return;
      }
      considerX(a.first + (b.first - a.first) * t);
    };

    considerAtY(bandTop);
    considerAtY(bandBottom);
  };

  considerEdge(*leftBottom, *leftTop);
  considerEdge(*leftTop, *rightTop);
  considerEdge(*rightTop, *rightBottom);
  considerEdge(*rightBottom, *leftBottom);

  return std::isfinite(minX) ? minX
                             : std::numeric_limits<float>::quiet_NaN();
}

void BMSRenderer::layoutCenteredJudgementText() {
  const bool hasTimingDirection =
      judgementTimingDirectionText != nullptr &&
      judgementTimingDirectionText->getVisible();
  const bool hasTimingMs =
      judgementTimingMsText != nullptr && judgementTimingMsText->getVisible();
  const bool hasPacemakerDelta =
      pacemakerDeltaText != nullptr && pacemakerDeltaText->getVisible();
  if (judgementLayoutWidth == rendering::window_width &&
      judgementLayoutHeight == rendering::window_height &&
      judgementLayoutHasTimingDirection == hasTimingDirection &&
      judgementLayoutHasTimingMs == hasTimingMs &&
      judgementLayoutHasPacemakerDelta == hasPacemakerDelta) {
    return;
  }

  judgementLayoutWidth = rendering::window_width;
  judgementLayoutHeight = rendering::window_height;
  judgementLayoutHasTimingDirection = hasTimingDirection;
  judgementLayoutHasTimingMs = hasTimingMs;
  judgementLayoutHasPacemakerDelta = hasPacemakerDelta;

  const int maxAvailableWidth = std::max(1, judgementLayoutWidth - 48);
  const int judgeLineHeight = 68;
  const int timingLineHeight = 28;
  const int pacemakerDeltaLineHeight = 40;
  const int lineGap = 2;
  const float normalizedY =
      std::clamp(judgementTextY, AppSettings::kMinJudgementTextY,
                 AppSettings::kMaxJudgementTextY);
  const int centerY = static_cast<int>(
      std::round(static_cast<float>(judgementLayoutHeight) *
                 (1.0f - normalizedY)));

  int judgeWidth = 1;
  if (judgeText != nullptr && judgeText->getVisible()) {
    const int minJudgeWidth = std::min(170, maxAvailableWidth);
    judgeWidth = std::clamp(judgeText->textureWidth() + 28, minJudgeWidth,
                            maxAvailableWidth);
  }
  const int judgeY =
      std::clamp(centerY - judgeLineHeight / 2, 0,
                 std::max(0, judgementLayoutHeight - judgeLineHeight));
  const int judgeX = (judgementLayoutWidth - judgeWidth) / 2;
  if (judgeText != nullptr) {
    judgeText->setPosition(judgeX, judgeY);
    judgeText->setSize(judgeWidth, judgeLineHeight);
  }

  constexpr int kTimingDirectionMaxWidth = 104;
  constexpr int kTimingMsMaxWidth = 96;
  constexpr int kTimingInnerGap = 6;
  const int timingWidth =
      std::min(maxAvailableWidth, kTimingDirectionMaxWidth + kTimingInnerGap +
                                      kTimingMsMaxWidth);
  const int timingX = (judgementLayoutWidth - timingWidth) / 2;
  const int timingY = std::max(0, judgeY - timingLineHeight - lineGap);
  const int pacemakerDeltaWidth = std::min(maxAvailableWidth, 220);
  const int pacemakerDeltaX =
      (judgementLayoutWidth - pacemakerDeltaWidth) / 2;
  const int pacemakerDeltaY =
      std::max(0, timingY - pacemakerDeltaLineHeight - lineGap);
  if (hasPacemakerDelta) {
    pacemakerDeltaText->setPosition(pacemakerDeltaX, pacemakerDeltaY);
    pacemakerDeltaText->setSize(pacemakerDeltaWidth,
                                pacemakerDeltaLineHeight);
  } else if (pacemakerDeltaText != nullptr) {
    pacemakerDeltaText->setPosition(pacemakerDeltaX, pacemakerDeltaY);
    pacemakerDeltaText->setSize(1, 1);
  }
  if (hasTimingDirection) {
    judgementTimingDirectionText->setPosition(timingX, timingY);
    judgementTimingDirectionText->setSize(timingWidth, timingLineHeight);
  } else if (judgementTimingDirectionText != nullptr) {
    judgementTimingDirectionText->setPosition(timingX, timingY);
    judgementTimingDirectionText->setSize(1, 1);
  }
  if (hasTimingMs) {
    judgementTimingMsText->setPosition(timingX, timingY);
    judgementTimingMsText->setSize(timingWidth, timingLineHeight);
  } else if (judgementTimingMsText != nullptr) {
    judgementTimingMsText->setPosition(timingX, timingY);
    judgementTimingMsText->setSize(1, 1);
  }
}

void BMSRenderer::onLanePressed(int lane, const JudgeResult judge,
                                long long time) {
  const auto it = laneToOrderIndex.find(lane);
  if (it == laneToOrderIndex.end()) {
    return;
  }
  AtomicLaneState &laneState = laneStatesByOrder[it->second];
  laneState.lastPressedJudgement.store(static_cast<int>(judge.judgement),
                                       std::memory_order_relaxed);
  laneState.lastPressedDiff.store(judge.Diff, std::memory_order_relaxed);
  laneState.lastPressedTime.store(time, std::memory_order_relaxed);
  laneState.isPressed.store(true, std::memory_order_relaxed);
  laneState.lastStateTime.store(time, std::memory_order_release);
}

void BMSRenderer::onLaneReleased(int lane, long long time) {
  const auto it = laneToOrderIndex.find(lane);
  if (it == laneToOrderIndex.end()) {
    return;
  }
  AtomicLaneState &laneState = laneStatesByOrder[it->second];
  laneState.isPressed.store(false, std::memory_order_relaxed);
  laneState.lastStateTime.store(time, std::memory_order_release);
}
void BMSRenderer::onJudge(JudgeResult judgeResult, int combo, int score,
                          long long displayTimeMicros,
                          bool recordTimingSample) {
  if (judgeResult.judgement == None) {
    return;
  }
  if (recordTimingSample) {
    judgementIndicator.record(judgeResult, displayTimeMicros);
  }
  if (renderHud && judgeText != nullptr && scoreText != nullptr) {
    pendingJudge.store(static_cast<int>(judgeResult.judgement),
                       std::memory_order_relaxed);
    pendingScore.store(score, std::memory_order_relaxed);
    pendingCombo.store(combo, std::memory_order_relaxed);
    pendingJudgeDiffMicros.store(judgeResult.Diff, std::memory_order_relaxed);
    pendingJudgeDisplayMicros.store(displayTimeMicros,
                                    std::memory_order_relaxed);
    hudRevision.fetch_add(1, std::memory_order_release);
  }
}
void BMSRenderer::drawLongNote(
    float headY, float tailY, bms_parser::LongNote *const &head,
    gameplay_note_submission_order::LongNoteOrder order,
    bool renderBudgetReserved) {
  if (!renderBudgetReserved) {
    return;
  }
  // assert head
  assert(!head->IsTail() && "head is tail");
  const bool tailMissedWithHead = wasLongNoteTailMissedWithHead(head);
  const bool tailReleasedEarly =
      wasLongNoteTailReleasedEarly(head) && !tailMissedWithHead;
  const bool tailResolvedForRendering =
      head->Tail->IsPlayed && !tailMissedWithHead;
  if (tailResolvedForRendering && !tailReleasedEarly)
    return;
  const float headRenderY =
      head->IsPlayed && !head->IsDead ? judgeY : headY;
  const auto headClip = noteRenderClip(
      head, currentRenderMicros, headRenderY, noteRenderHeight, judgeY);
  const auto tailClip = noteRenderClip(
      head->Tail, currentRenderMicros, tailY, noteRenderHeight, judgeY);
  float bodyStartY = headRenderY;
  if (head->Timeline != nullptr &&
      head->Timeline->Timing >= currentRenderMicros) {
    bodyStartY = std::max(bodyStartY, judgeY);
  }
  const float bodyHeight = tailY - bodyStartY;
  const float bodyWidth = noteRenderWidth;

  const NoteSheet &sheet = sheetForLane(head->Lane);
  const bool isClassicLongNote = effectiveLongNoteIsClassic(head, chart);
  const bool isHellCharge = effectiveLongNoteIsHellCharge(head, chart);
  const NoteUvRegion &headUv =
      isHellCharge ? sheet.hellChargeHead : sheet.longHead;
  const NoteUvRegion &tailUv =
      isHellCharge ? sheet.hellChargeTail : sheet.longTail;
  const bool headHasReachedJudge = head->IsPlayed || head->IsDead ||
                                   headY <= judgeY;
  const bool hcnBodyRegrabbed = headHasReachedJudge && isHellCharge &&
                                laneIsCurrentlyPressed(head->Lane);
  const bool bodyActive = head->IsHolding || hcnBodyRegrabbed;
  bgfx::TextureHandle bodyTexture = BGFX_INVALID_HANDLE;
  float bodyRenderHeight = longBodyRenderHeightOff;
  int bodyFrameCount = 1;
  int bodyCycleMs = 0;
  if (isHellCharge) {
    if (bodyActive) {
      bodyTexture = sheet.hellChargeBodyOnTexture;
      bodyRenderHeight = longBodyRenderHeightOn;
      bodyFrameCount = kAnimatedLongBodyFrameCount;
      bodyCycleMs = kAnimatedLongBodyCycleMs;
    } else if (headHasReachedJudge) {
      bodyTexture = sheet.hellChargeDamageTexture;
      bodyRenderHeight = longBodyRenderHeightOn;
      bodyFrameCount = kAnimatedLongBodyFrameCount;
      bodyCycleMs = kHellChargeDamageCycleMs;
    } else {
      bodyTexture = sheet.hellChargeBodyOffTexture;
    }
  } else if (bodyActive) {
    bodyTexture = sheet.longBodyOnTexture;
    bodyRenderHeight = longBodyRenderHeightOn;
    bodyFrameCount = kAnimatedLongBodyFrameCount;
    bodyCycleMs = kAnimatedLongBodyCycleMs;
  } else {
    bodyTexture = sheet.longBodyOffTexture;
  }

  // Body
  if (bodyHeight > 0.0f && bgfx::isValid(bodyTexture)) {
    auto &bodyBatch = noteTextureBatchAtDepth(order.bodyDepth);
    if (bodyFrameCount > 1 && bodyCycleMs > 0) {
      const int frame =
          skinAnimationFrame(currentRenderMicros, bodyFrameCount, bodyCycleMs);
      const float v = (static_cast<float>(frame) + 0.5f) /
                      static_cast<float>(bodyFrameCount);
      bodyBatch.addRectUV(laneToX(head->Lane), bodyStartY, bodyWidth,
                          bodyHeight,
                          0.0f, v, 1.0f, v, bodyTexture);
    } else {
      float tileV = bodyHeight / bodyRenderHeight;
      bodyBatch.addRect(laneToX(head->Lane), bodyStartY, bodyWidth, bodyHeight,
                        1.0f, tileV, bodyTexture);
    }
  }

  if (tailClip.visible && !isClassicLongNote &&
      (!tailReleasedEarly || tailY > judgeY)) {
    noteTextureBatchAtDepth(order.endpointDepth).addRectUV(
        laneToX(head->Tail->Lane), tailClip.y, noteRenderWidth,
        tailClip.height, tailUv.u0, tailUv.v0, tailUv.u1,
        clippedBottomV(tailUv.v0, tailUv.v1,
                       tailClip.bottomTextureFraction),
        sheet.texture);
  }

  if (head->IsPlayed || !headClip.visible)
    return;

  // Head
  noteTextureBatchAtDepth(order.endpointDepth).addRectUV(
      laneToX(head->Lane), headClip.y, noteRenderWidth, headClip.height,
      headUv.u0, headUv.v0, headUv.u1,
      clippedBottomV(headUv.v0, headUv.v1,
                     headClip.bottomTextureFraction),
      sheet.texture);
}

void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note,
                                 uint32_t submitDepth) {
  const auto clip = noteRenderClip(note, currentRenderMicros, y,
                                   noteRenderHeight, judgeY);
  if (note->IsPlayed || !clip.visible ||
      !gameplay_scroll_geometry::noteRectangleIntersectsViewport(
          clip.y, clip.height, lowerBound, upperBound))
    return;

  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
    return;
  }

  const NoteSheet &sheet = sheetForLane(note->Lane);

  noteTextureBatchAtDepth(submitDepth).addRectUV(
      laneToX(note->Lane), clip.y, noteRenderWidth, clip.height, sheet.note.u0,
      sheet.note.v0, sheet.note.u1,
      clippedBottomV(sheet.note.v0, sheet.note.v1,
                     clip.bottomTextureFraction),
      sheet.texture);
}

void BMSRenderer::drawInvisibleNote(float y, bms_parser::Note *const &note,
                                    uint32_t submitDepth) {
  const auto clip = noteRenderClip(note, currentRenderMicros, y,
                                   noteRenderHeight, judgeY);
  if (note->IsPlayed || note->IsDead || !clip.visible ||
      !gameplay_scroll_geometry::noteRectangleIntersectsViewport(
          clip.y, clip.height, lowerBound, upperBound)) {
    return;
  }

  const uint32_t color = Color(255, 149, 36, 224).toABGR();
  const float x = laneToX(note->Lane);
  if (note->IsLongNote()) {
    if (!chartEntityRenderBudget.tryConsume(
            gameplay_chart_entity_render_budget::
                kSingleRectangleEntityCost)) {
      return;
    }
    setInvisibleBatchDepth(submitDepth);
    gimmickBatchRenderer.addRect(x, clip.y, noteRenderWidth, clip.height,
                                 color);
    return;
  }

  const float borderThickness =
      std::max(0.015F,
               noteRenderHeight *
                   gameplay_scroll_geometry::kInvisibleNoteBorderHeightRatio);
  const auto outline = gameplay_scroll_geometry::noteOutlineRectangles(
      x, y, noteRenderWidth, noteRenderHeight, borderThickness, clip);
  if (outline.count == 0U ||
      !chartEntityRenderBudget.tryConsume(
          static_cast<uint32_t>(outline.count))) {
    return;
  }

  setInvisibleBatchDepth(submitDepth);
  for (std::size_t i = 0; i < outline.count; ++i) {
    const auto &rect = outline.rectangles[i];
    gimmickBatchRenderer.addRect(rect.x, rect.y, rect.width, rect.height,
                                 color);
  }
}

void BMSRenderer::drawLandmineNote(float y,
                                   bms_parser::LandmineNote *const &note,
                                   uint32_t submitDepth) {
  const auto clip = noteRenderClip(note, currentRenderMicros, y,
                                   noteRenderHeight, judgeY);
  if (note->IsPlayed || note->IsDead || !clip.visible ||
      !gameplay_scroll_geometry::noteRectangleIntersectsViewport(
          clip.y, clip.height, lowerBound, upperBound)) {
    return;
  }

  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
    return;
  }

  const NoteSheet &sheet = sheetForLane(note->Lane);
  noteTextureBatchAtDepth(submitDepth).addRectUV(
      laneToX(note->Lane), clip.y, noteRenderWidth, clip.height, sheet.mine.u0,
      sheet.mine.v0, sheet.mine.u1,
      clippedBottomV(sheet.mine.v0, sheet.mine.v1,
                     clip.bottomTextureFraction),
      sheet.texture);
}

void BMSRenderer::buildTimelineScrollPositions() {
  timelineScrollPositions.clear();
  timelineScrollPositions.reserve(timelines.size());
  if (timelines.empty()) {
    return;
  }

  double position = timelines.front()->BeatPosition;
  timelineScrollPositions.push_back(position);
  for (size_t i = 1; i < timelines.size(); ++i) {
    const auto *prevTimeline = timelines[i - 1];
    const auto *timeline = timelines[i];
    position += (timeline->BeatPosition - prevTimeline->BeatPosition) *
                prevTimeline->Scroll;
    timelineScrollPositions.push_back(position);
  }
}

double BMSRenderer::calculateMostPrevalentBpm() const {
  const double chartBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  if (timelines.empty()) {
    return chartBpm;
  }

  std::map<double, long long> bpmDurations;
  std::vector<double> bpmOrder;
  auto addDuration = [&](double bpm, long long durationMicros) {
    if (!std::isfinite(bpm) || bpm <= 0.0 || durationMicros <= 0) {
      return;
    }
    if (bpmDurations.find(bpm) == bpmDurations.end()) {
      bpmOrder.push_back(bpm);
    }
    bpmDurations[bpm] += durationMicros;
  };

  addDuration(chartBpm, timelines.front()->Timing);
  const long long chartEnd =
      chart != nullptr
          ? std::max({chart->Meta.TotalLength, chart->Meta.PlayLength,
                      timelines.back()->Timing})
          : timelines.back()->Timing;
  for (size_t i = 0; i < timelines.size(); ++i) {
    const auto *timeline = timelines[i];
    const long long segmentEnd =
        i + 1 < timelines.size() ? timelines[i + 1]->Timing : chartEnd;
    addDuration(timeline->Bpm, segmentEnd - timeline->Timing);
  }

  double bestBpm = chartBpm;
  long long bestDuration = 0;
  for (double bpm : bpmOrder) {
    const long long duration = bpmDurations[bpm];
    if (duration > bestDuration) {
      bestBpm = bpm;
      bestDuration = duration;
    }
  }
  return bestDuration > 0 ? bestBpm : chartBpm;
}

double BMSRenderer::visibleTimeReferenceBpm() const {
  if (floatingVisibleTimeReferenceBpm.has_value() &&
      std::isfinite(*floatingVisibleTimeReferenceBpm) &&
      *floatingVisibleTimeReferenceBpm > 0.0) {
    return *floatingVisibleTimeReferenceBpm;
  }

  double referenceBpm = chart != nullptr ? chart->Meta.Bpm : 0.0;
  if (visibleTimeBpmStrategy ==
          AppSettings::VisibleTimeBpmStrategy::MostPrevalent &&
      std::isfinite(mostPrevalentBpm) && mostPrevalentBpm > 0.0) {
    referenceBpm = mostPrevalentBpm;
  }
  if (!std::isfinite(referenceBpm) || referenceBpm <= 0.0) {
    return 1.0;
  }
  return referenceBpm;
}

int BMSRenderer::effectiveVisibleTimeGreenNumber() const {
  const double referenceBpm = visibleTimeReferenceBpm();
  const double bpm =
      currentBpm > 0.0 && std::isfinite(currentBpm) ? currentBpm : referenceBpm;
  if (!std::isfinite(referenceBpm) || referenceBpm <= 0.0 ||
      !std::isfinite(bpm) || bpm <= 0.0) {
    return std::max(1, visibleTimeGreenNumber);
  }

  const double scaled =
      static_cast<double>(visibleTimeGreenNumber) * referenceBpm / bpm;
  if (!std::isfinite(scaled)) {
    return std::max(1, visibleTimeGreenNumber);
  }
  return std::max(1, static_cast<int>(std::lround(scaled)));
}

std::string BMSRenderer::laneCoverVisibleTimeLabel() const {
  const int greenNumber = effectiveVisibleTimeGreenNumber();
  if (visibleTimeUseMilliseconds) {
    const int milliseconds = std::max(
        1, static_cast<int>(std::lround(static_cast<double>(greenNumber) *
                                        1000.0 / 600.0)));
    return std::to_string(milliseconds) + " ms";
  }
  return std::to_string(greenNumber);
}

double BMSRenderer::scrollPositionAtTime(long long timeMicros) const {
  if (timelines.empty()) {
    return 0.0;
  }

  const auto timelineIt = std::lower_bound(
      timelines.begin(), timelines.end(), timeMicros,
      [](const bms_parser::TimeLine *timeline, long long timing) {
        return timeline->Timing < timing;
      });

  if (timelineIt == timelines.begin()) {
    const auto *timeline = timelines.front();
    if (timeline->Timing <= 0) {
      return timelineScrollPositions.front() -
             gameplay_timing::leadInBeatDistance(timeline->Timing, timeMicros,
                                                 timeline->Bpm) *
                 timeline->Scroll;
    }
    const double progress =
        std::clamp(static_cast<double>(timeMicros) /
                       static_cast<double>(timeline->Timing),
                   0.0, 1.0);
    return timelineScrollPositions.front() * progress;
  }

  if (timelineIt == timelines.end()) {
    return timelineScrollPositions.back();
  }

  const size_t timelineIndex =
      static_cast<size_t>(std::distance(timelines.begin(), timelineIt));
  const auto *timeline = timelines[timelineIndex];
  if (timeline->Timing == timeMicros) {
    return timelineScrollPositions[timelineIndex];
  }

  const size_t prevTimelineIndex = timelineIndex - 1;
  const auto *prevTimeline = timelines[prevTimelineIndex];
  const long long stopDuration =
      static_cast<long long>(prevTimeline->GetStopDuration());
  const long long stopEnd = prevTimeline->Timing + stopDuration;
  if (timeMicros <= stopEnd) {
    return timelineScrollPositions[prevTimelineIndex];
  }

  const long long scrollDuration =
      timeline->Timing - prevTimeline->Timing - stopDuration;
  if (scrollDuration <= 0) {
    return timelineScrollPositions[timelineIndex];
  }

  const double progress =
      std::clamp(static_cast<double>(timeMicros - stopEnd) /
                     static_cast<double>(scrollDuration),
                 0.0, 1.0);
  return timelineScrollPositions[prevTimelineIndex] +
         (timelineScrollPositions[timelineIndex] -
          timelineScrollPositions[prevTimelineIndex]) *
             progress;
}

void BMSRenderer::drawReplayGhosts(float rxhs, long long currentTimeMicros,
                                   double currentScrollPosition) {
  if (replayGhostEvents.empty() || rxhs <= 0.0f) {
    return;
  }

  const auto visible = gameplay_scroll_geometry::visibleScrollRange(
      currentScrollPosition, rxhs, lowerBound, upperBound, noteRenderHeight,
      judgeY);
  const double firstVisibleScrollPosition = visible.minimum;
  const double lastVisibleScrollPosition = visible.maximum;

  const auto firstVisible = std::lower_bound(
      replayGhostEvents.begin(), replayGhostEvents.end(),
      firstVisibleScrollPosition,
      [](const ReplayGhostEvent &event, double scrollPosition) {
        return event.judgeScrollPosition < scrollPosition;
      });
  const auto lastVisible = std::upper_bound(
      firstVisible, replayGhostEvents.end(), lastVisibleScrollPosition,
      [](double scrollPosition, const ReplayGhostEvent &event) {
        return scrollPosition < event.judgeScrollPosition;
      });

  for (auto it = firstVisible; it != lastVisible; ++it) {
    const auto &event = *it;
    if (event.judgeTimeMicros < currentTimeMicros) {
      continue;
    }
    const float ghostY = gameplay_scroll_geometry::renderY(
        event.judgeScrollPosition, currentScrollPosition, rxhs, judgeY);
    drawGhostNoteOutline(ghostY, event);
  }
}

void BMSRenderer::drawReplayMissMarkers(float rxhs,
                                        double currentScrollPosition) {
  if (replayMissMarkers.empty() || rxhs <= 0.0f) {
    return;
  }

  const auto visible = gameplay_scroll_geometry::visibleScrollRange(
      currentScrollPosition, rxhs, lowerBound, upperBound, noteRenderHeight,
      judgeY);
  const double firstVisibleScrollPosition = visible.minimum;
  const double lastVisibleScrollPosition = visible.maximum;

  const auto firstVisible = std::lower_bound(
      replayMissMarkers.begin(), replayMissMarkers.end(),
      firstVisibleScrollPosition,
      [](const ReplayMissMarker &marker, double scrollPosition) {
        return marker.noteScrollPosition < scrollPosition;
      });
  const auto lastVisible = std::upper_bound(
      firstVisible, replayMissMarkers.end(), lastVisibleScrollPosition,
      [](double scrollPosition, const ReplayMissMarker &marker) {
        return scrollPosition < marker.noteScrollPosition;
      });

  for (auto it = firstVisible; it != lastVisible; ++it) {
    const auto &marker = *it;
    const float markerY = gameplay_scroll_geometry::renderY(
        marker.noteScrollPosition, currentScrollPosition, rxhs, judgeY);
    drawMissMarkerX(markerY, marker);
  }
}

void BMSRenderer::drawGhostNoteOutline(float y, const ReplayGhostEvent &event) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
    return;
  }

  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kReplayGhostOutlineCost)) {
    return;
  }

  Color color(255, 255, 255, 220);
  if (event.judgement != PGreat) {
    color = event.judgeTimeMicros < event.noteTimeMicros
                ? Color(0, 96, 255, 220)
                : Color(255, 40, 40, 220);
  }

  const float x = laneToX(event.lane);
  const float thickness = std::max(0.015f, noteRenderHeight * 0.12f);
  const uint32_t abgr = color.toABGR();
  ghostBatchRenderer.addRect(x, y, noteRenderWidth, thickness, abgr);
  ghostBatchRenderer.addRect(x, y + noteRenderHeight - thickness,
                             noteRenderWidth, thickness, abgr);
  ghostBatchRenderer.addRect(x, y, thickness, noteRenderHeight, abgr);
  ghostBatchRenderer.addRect(x + noteRenderWidth - thickness, y, thickness,
                             noteRenderHeight, abgr);
}

void BMSRenderer::drawMissMarkerX(float y, const ReplayMissMarker &marker) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
    return;
  }

  constexpr int kSteps = 7;
  static_assert(kSteps * 2 ==
                gameplay_chart_entity_render_budget::kReplayMissMarkerCost);
  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kReplayMissMarkerCost)) {
    return;
  }
  const float x = laneToX(marker.lane);
  const float block =
      std::max(0.018f, std::min(noteRenderWidth, noteRenderHeight) * 0.22f);
  const float maxX = std::max(0.0f, noteRenderWidth - block);
  const float maxY = std::max(0.0f, noteRenderHeight - block);
  const uint32_t color = Color(255, 42, 42, 236).toABGR();

  for (int i = 0; i < kSteps; ++i) {
    const float t = kSteps == 1 ? 0.0f
                                : static_cast<float>(i) /
                                      static_cast<float>(kSteps - 1);
    const float yOffset = maxY * t;
    ghostBatchRenderer.addRect(x + maxX * t, y + yOffset, block, block, color);
    ghostBatchRenderer.addRect(x + maxX * (1.0f - t), y + yOffset, block, block,
                               color);
  }
}

void BMSRenderer::applyTouchSample(
    std::unordered_map<long long, TouchPointVisual> &activeTouches,
    std::vector<TouchPointVisual> &releasedTouches,
    const ReplayTouchSample &sample) {
  TouchPointVisual visual;
  visual.x = std::clamp(sample.x, 0.0f, 1.0f);
  visual.y = std::clamp(sample.y, 0.0f, 1.0f);
  visual.eventTimeMicros = sample.songTimeMicros;

  switch (sample.action) {
  case ReplayTouchAction::Down:
  case ReplayTouchAction::Move:
    visual.releaseTimeMicros = 0;
    visual.released = false;
    activeTouches[sample.fingerId] = visual;
    break;
  case ReplayTouchAction::Up:
  case ReplayTouchAction::Cancel:
    if (auto it = activeTouches.find(sample.fingerId);
        it != activeTouches.end()) {
      visual = it->second;
      visual.eventTimeMicros = sample.songTimeMicros;
      visual.releaseTimeMicros = sample.songTimeMicros;
      visual.released = true;
      activeTouches.erase(it);
    } else {
      visual.releaseTimeMicros = sample.songTimeMicros;
      visual.released = true;
    }
    visual.x = std::clamp(sample.x, 0.0f, 1.0f);
    visual.y = std::clamp(sample.y, 0.0f, 1.0f);
    releasedTouches.push_back(visual);
    break;
  }
}

void BMSRenderer::advanceReplayTouches(long long replayTouchTimeMicros) {
  if (replayTouchTimeMicros < lastReplayTouchTimeMicros) {
    replayTouchCursor = 0;
    replayActiveTouchSamples.clear();
    replayReleasedTouchSamples.clear();
  }
  lastReplayTouchTimeMicros = replayTouchTimeMicros;

  while (replayTouchCursor < replayTouchSamples.size() &&
         replayTouchSamples[replayTouchCursor].songTimeMicros <=
             replayTouchTimeMicros) {
    applyTouchSample(replayActiveTouchSamples, replayReleasedTouchSamples,
                     replayTouchSamples[replayTouchCursor]);
    ++replayTouchCursor;
  }
}

void BMSRenderer::pruneReleasedTouchSamples(
    std::vector<TouchPointVisual> &releasedTouches,
    long long currentTimeMicros) {
  releasedTouches.erase(
      std::remove_if(releasedTouches.begin(), releasedTouches.end(),
                     [currentTimeMicros](const TouchPointVisual &sample) {
                       return touch_visualization_timing::
                           shouldPruneReleasedTouch(
                               sample.released, sample.releaseTimeMicros,
                               currentTimeMicros,
                               kTouchPointReleaseLingerMicros);
                     }),
      releasedTouches.end());
}

void BMSRenderer::drawTouchSample(const TouchPointVisual &sample,
                                  long long currentTimeMicros) {
  float releaseProgress = 0.0f;
  if (sample.released) {
    const long long elapsedMicros =
        touch_visualization_timing::releaseElapsedMicros(
            sample.released, sample.releaseTimeMicros, currentTimeMicros);
    if (elapsedMicros > kTouchPointReleaseLingerMicros) {
      return;
    }
    releaseProgress = static_cast<float>(elapsedMicros) /
                      static_cast<float>(kTouchPointReleaseLingerMicros);
  }

  const float x = std::clamp(sample.x, 0.0f, 1.0f) *
                  static_cast<float>(rendering::window_width);
  const float y = std::clamp(sample.y, 0.0f, 1.0f) *
                  static_cast<float>(rendering::window_height);
  const float baseRadius =
      std::min(static_cast<float>(rendering::window_width),
               static_cast<float>(rendering::window_height)) *
      kTouchPointRadiusScale;
  const float pulseProgress = 1.0f - (1.0f - releaseProgress) *
                                          (1.0f - releaseProgress);
  const float radius =
      std::clamp(baseRadius, kTouchPointMinRadius, kTouchPointMaxRadius) *
      (1.0f + pulseProgress * kTouchPointReleasePulseScale);
  const float alphaScale =
      sample.released ? (1.0f - releaseProgress) : 1.0f;
  const uint8_t outerAlpha = scaledAlpha(86, alphaScale);
  const uint8_t innerAlpha = scaledAlpha(112, alphaScale * alphaScale);
  if (outerAlpha > 0) {
    simpleBatchRenderer.addCircle(
        x, y, radius, Color(51, 190, 255, outerAlpha).toABGR());
  }
  if (innerAlpha > 0) {
    simpleBatchRenderer.addCircle(
        x, y, radius * 0.42f, Color(255, 255, 255, innerAlpha).toABGR());
  }
}

void BMSRenderer::drawTouchPoints(long long replayTouchTimeMicros) {
  if (!touchVisualizationEnabled) {
    return;
  }

  advanceReplayTouches(replayTouchTimeMicros);
  pruneReleasedTouchSamples(replayReleasedTouchSamples, replayTouchTimeMicros);
  pruneReleasedTouchSamples(liveReleasedTouchSamples, replayTouchTimeMicros);
  for (const auto &sample : replayReleasedTouchSamples) {
    drawTouchSample(sample, replayTouchTimeMicros);
  }
  for (const auto &sample : liveReleasedTouchSamples) {
    drawTouchSample(sample, replayTouchTimeMicros);
  }
  for (const auto &entry : replayActiveTouchSamples) {
    drawTouchSample(entry.second, replayTouchTimeMicros);
  }
  for (const auto &entry : liveTouchSamples) {
    drawTouchSample(entry.second, replayTouchTimeMicros);
  }
}

float BMSRenderer::calculateLanePlaneScreenTopIntersection() {
  Camera &camera = rendering::game_camera;
  constexpr float kFallbackLaneTop = 8.5f;

  const float screenTopY = 0.0f;
  const float screenCenterX = rendering::window_width / 2.0f;
  const bx::Vec3 eye = camera.getEye();
  const bx::Vec3 screenTopWorld =
      camera.deproject(screenCenterX, screenTopY, 5.0f);

  bx::Vec3 rayDir = {screenTopWorld.x - eye.x, screenTopWorld.y - eye.y,
                     screenTopWorld.z - eye.z};
  const float rayLength = bx::length(rayDir);
  if (rayLength <= 0.0001f) {
    return kFallbackLaneTop;
  }
  rayDir = {rayDir.x / rayLength, rayDir.y / rayLength, rayDir.z / rayLength};

  if (std::abs(rayDir.z) < 0.001f) {
    return kFallbackLaneTop;
  }

  const float t = -eye.z / rayDir.z;
  if (t < 0.0f) {
    return kFallbackLaneTop;
  }

  return eye.y + t * rayDir.y;
}

void BMSRenderer::render(RenderContext &context, long long micro) {
  render(context, micro, micro);
}

void BMSRenderer::render(RenderContext &context, long long micro,
                         long long replayTouchTimeMicros) {
  const long long chartTimeMicros =
      gameplay_scroll_geometry::chartRenderTimeMicros(micro);
  currentRenderMicros = micro;
  applyPendingHudText(micro);
  updateJudgementCounterText();

  using gameplay_note_submission_order::kBackgroundDepth;
  using gameplay_note_submission_order::kGaugeDepth;
  using gameplay_note_submission_order::kGhostDepth;
  using gameplay_note_submission_order::kJudgementIndicatorDepth;
  using gameplay_note_submission_order::kLaneBeamDepth;

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kBackgroundDepth);
  gimmickBatchRenderer.setSubmitView(rendering::main_view);
  ghostBatchRenderer.setSubmitDepth(kGhostDepth);
  chartEntityRenderBudget.reset();
  simpleBatchRenderer.begin();
  ghostBatchRenderer.begin();
  beginOrderedNoteBatches();
  // background
  drawRect(playAreaWidth, upperBound - judgeY, playAreaLeftX,
           judgeY, Color(20, 20, 20, 122));
  // judge line
  drawRect(playAreaWidth, noteRenderHeight, playAreaLeftX, judgeY,
           Color(255, 255, 255, 255));
  // Green number is the legacy BMS visible-time unit: 600 green = 1000 ms.
  const float visibleTimeMs = std::max(
      1.0f, static_cast<float>(visibleTimeGreenNumber) * (1000.0f / 600.0f));
  const float hispeed =
      240000.0f / static_cast<float>(visibleTimeReferenceBpm()) /
      visibleTimeMs *
      static_cast<float>(gameplay_timing::playbackTravelScale(playbackRate));
  const float laneHeight = std::max(0.001f, upperBound - judgeY);
  const float hiddenRatio =
      static_cast<float>(noteStartPositionPercent) / 100.0f;
  noteVisibleUpperBound = judgeY + laneHeight * (1.0f - hiddenRatio);
  const float visibleTravelHeight =
      std::max(0.001f, noteVisibleUpperBound - judgeY);
  float rxhs = visibleTravelHeight * hispeed;
  float y = judgeY;
  const double currentScrollPosition = scrollPositionAtTime(chartTimeMicros);
  gameplay_note_submission_order::Allocator submissionOrder;
  const auto pastLongNoteOrder = submissionOrder.captureLongNote();
  auto &longNoteLookahead = longNoteLookaheadScratch;
  longNoteLookahead.clear();
  const auto rememberLongNoteHead =
      [&](bms_parser::LongNote *longNote, float headY,
          const auto &orderProvider) {
        auto [it, inserted] = longNoteLookahead.try_emplace(longNote);
        it->second.headY = headY;
        if (!inserted) {
          return;
        }
        it->second.renderBudgetReserved =
            chartEntityRenderBudget.tryConsume(
                gameplay_chart_entity_render_budget::
                    kLongNoteReservationCost);
        if (it->second.renderBudgetReserved) {
          it->second.order = orderProvider();
        }
      };
  for (auto *orphanLongNote : state.orphanLongNotes) {
    rememberLongNoteHead(orphanLongNote, lowerBound,
                         [&]() { return pastLongNoteOrder; });
  }
  double futureY = static_cast<double>(judgeY);
  bool futureTraversalStarted = false;
  // render timeline
  for (size_t i = state.currentTimelineIndex; i < timelines.size(); ++i) {
    const auto &timeLine = timelines[i];
    if (i >= timelineScrollPositions.size()) {
      break;
    }
    const bool timelineIsFuture = timeLine->Timing >= chartTimeMicros;
    // Match Beatoraja's bounded forward walk. In particular, a NaN produced
    // by equal-microsecond huge-BPM rows makes this direct comparison false.
    if (timelineIsFuture && futureTraversalStarted &&
        !gameplay_scroll_geometry::futureTimelineTraversalContinues(
            futureY, upperBound)) {
      break;
    }
    if (timelineIsFuture) {
      if (i > 0) {
        const auto *previous = timelines[i - 1];
        futureY = gameplay_scroll_geometry::advanceFutureTimelineY(
            futureY, timeLine->BeatPosition - previous->BeatPosition,
            previous->Scroll, previous->Timing,
            previous->GetStopDuration(), timeLine->Timing, chartTimeMicros,
            static_cast<double>(rxhs));
      } else {
        futureY = gameplay_scroll_geometry::initialFutureTimelineY(
            timelineScrollPositions[i], currentScrollPosition, rxhs, judgeY);
      }
      y = static_cast<float>(futureY);
      futureTraversalStarted = true;
    } else {
      y = gameplay_scroll_geometry::renderY(
          timelineScrollPositions[i], currentScrollPosition, rxhs, judgeY);
    }

    if (timeLine->IsFirstInMeasure &&
        gameplay_scroll_geometry::shouldDrawMeasureLine(
            timeLine->Timing, chartTimeMicros, y, judgeY, upperBound) &&
        chartEntityRenderBudget.tryConsume(
            gameplay_chart_entity_render_budget::
                kSingleRectangleEntityCost)) {
      drawRect(playAreaWidth, 0.05f, playAreaLeftX, y,
               Color(255, 255, 255, 128));
    }
    if (timeLine->Timing < chartTimeMicros - latePoorTiming) {
      state.currentTimelineIndex = i;
    }
    const bool rowHasLongHead =
        i < groupedTimelineNotes.size() &&
        std::any_of(groupedTimelineNotes[i].begin(),
                    groupedTimelineNotes[i].end(), [](auto *note) {
                      if (note == nullptr || !note->IsLongNote()) {
                        return false;
                      }
                      return !static_cast<bms_parser::LongNote *>(note)
                                  ->IsTail();
                    });
    std::optional<gameplay_note_submission_order::LongNoteOrder>
        rowLongOrder;
    std::optional<uint32_t> rowPrimaryDepth;
    const auto ensurePrimaryDepth = [&]() {
      if (!rowPrimaryDepth.has_value()) {
        if (rowHasLongHead) {
          rowLongOrder = submissionOrder.captureLongNote();
          rowPrimaryDepth = rowLongOrder->endpointDepth;
        } else {
          rowPrimaryDepth = submissionOrder.next();
        }
      }
      return *rowPrimaryDepth;
    };
    const auto ensureLongOrder = [&]() {
      (void)ensurePrimaryDepth();
      assert(rowLongOrder.has_value());
      return *rowLongOrder;
    };
    //    SDL_Log("BeatPosition: %f", timeLine->BeatPosition);
    // Render notes in grouped lane order (white/blue/scratch) to reduce texture
    // switches while keeping per-lane ordering intact.
    auto processNote = [&](bms_parser::Note *note) {
      if (note == nullptr) {
        return;
      }
      if (timelineIsFuture && !std::isfinite(y)) {
        return;
      }
      auto keepDeadLongNoteBody = [&]() {
        if (!note->IsLongNote()) {
          return false;
        }
        auto *longNote = static_cast<bms_parser::LongNote *>(note);
        if (!shouldKeepDeadLongNoteBody(longNote)) {
          return false;
        }
        state.orphanLongNotes.insert(longNote);
        rememberLongNoteHead(longNote, lowerBound, ensureLongOrder);
        return true;
      };
      if (timeLine->Timing >= chartTimeMicros - latePoorTiming) {
        // note is in the hittable timing
        if (note->IsDead) {
          if (keepDeadLongNoteBody()) {
            return;
          }
          return;
        }
        if (note->IsLandmineNote()) {
          if (timeLine->Timing >= chartTimeMicros) {
            drawLandmineNote(y, static_cast<bms_parser::LandmineNote *>(note),
                             ensurePrimaryDepth());
          }
          return;
        }
        // render note
        if (note->IsLongNote()) {
          auto *longNote = static_cast<bms_parser::LongNote *>(note);
          if (longNote->IsTail()) {
            if (longNote->Head == nullptr) {
              // ignore malformed chart: long note is not terminated
              return;
            }
            // find head's y
            if (auto it = longNoteLookahead.find(longNote->Head);
                it != longNoteLookahead.end()) {
              drawLongNote(it->second.headY, y, longNote->Head,
                           it->second.order,
                           it->second.renderBudgetReserved);
              // remove from lookahead
              longNoteLookahead.erase(longNote->Head);
            } else {
              const bool renderBudgetReserved =
                  chartEntityRenderBudget.tryConsume(
                      gameplay_chart_entity_render_budget::
                          kLongNoteReservationCost);
              drawLongNote(lowerBound, y, longNote->Head, pastLongNoteOrder,
                           renderBudgetReserved);
            }
          } else {
            rememberLongNoteHead(longNote, y, ensureLongOrder);
          }
        } else {
          drawNormalNote(y, note, ensurePrimaryDepth());
        }
      } else {
        // note has passed the last hittable timing
        if (note->IsDead) {
          if (keepDeadLongNoteBody()) {
            return;
          }
          return;
        }
        if (note->IsLandmineNote()) {
          return;
        }
        if (note->IsLongNote()) {
          auto *longNote = static_cast<bms_parser::LongNote *>(note);
          if (longNote->IsTail()) {
            if (longNote->Head == nullptr) {
              // ignore malformed chart: long note is not terminated
              return;
            }
            // remove from orphan long note
            state.orphanLongNotes.erase(longNote->Head);
            // and from long note lookahead
            longNoteLookahead.erase(longNote->Head);
          } else {
            // add to orphan long note
            state.orphanLongNotes.insert(longNote);

            // setting to lowerBound in all cases is OK because the played
            // state will be correctly handled by drawLongNote
            rememberLongNoteHead(longNote, lowerBound, ensureLongOrder);
          }
        }
      }
    };

    if (i < groupedTimelineNotes.size()) {
      for (auto *note : groupedTimelineNotes[i]) {
        processNote(note);
      }
    }
    for (const auto &note : timeLine->LandmineNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= chartTimeMicros) {
        drawLandmineNote(y, note, ensurePrimaryDepth());
      }
    }
    std::optional<uint32_t> rowInvisibleDepth;
    for (const auto &note : timeLine->InvisibleNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= chartTimeMicros) {
        if (showInvisibleNotes) {
          if (!rowInvisibleDepth.has_value()) {
            rowInvisibleDepth = submissionOrder.next();
          }
          drawInvisibleNote(y, note, *rowInvisibleDepth);
        }
      } else {
        note->IsDead = true;
      }
    }
  }

  // render leftover long notes
  for (const auto &pair : longNoteLookahead) {
    drawLongNote(pair.second.headY, upperBound, pair.first,
                 pair.second.order, pair.second.renderBudgetReserved);
  }
  if (replayGhostRenderingEnabled) {
    drawReplayGhosts(rxhs, chartTimeMicros, currentScrollPosition);
    drawReplayMissMarkers(rxhs, currentScrollPosition);
  }

  // Flush background/measure pass before notes.
  simpleBatchRenderer.flush();
  flushOrderedNoteBatches();
  ghostBatchRenderer.flush();

  if (renderLaneBeams) {
    simpleBatchRenderer.setSubmitDepth(kLaneBeamDepth);
    const long long nowMicros =
        useRenderTimeForLaneBeams
            ? micro
            : std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    laneStateSnapshot.clear();
    for (size_t i = 0; i < laneOrder.size(); ++i) {
      const AtomicLaneState &source = laneStatesByOrder[i];
      LaneState snapshot;
      snapshot.lastStateTime =
          source.lastStateTime.load(std::memory_order_acquire);
      snapshot.lastPressedTime =
          source.lastPressedTime.load(std::memory_order_relaxed);
      snapshot.isPressed = source.isPressed.load(std::memory_order_relaxed);
      snapshot.lastPressedJudge = JudgeResult(
          static_cast<Judgement>(
              source.lastPressedJudgement.load(std::memory_order_relaxed)),
          source.lastPressedDiff.load(std::memory_order_relaxed));
      laneStateSnapshot.emplace_back(laneOrder[i], snapshot);
    }
    for (const auto &entry : laneStateSnapshot) {
      drawLaneBeam(entry.first, entry.second, nowMicros);
    }
    simpleBatchRenderer.flush();
  }

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(start_lane_indicator::kLaneCoverDepth);
  simpleBatchRenderer.begin();
  drawLaneCover();
  simpleBatchRenderer.flush();

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(start_lane_indicator::kIndicatorDepth);
  simpleBatchRenderer.begin();
  drawStartLaneIndicators();
  simpleBatchRenderer.flush();
  layoutLaneCoverNumberTexts();
  if (laneCoverWhiteNumberText != nullptr) {
    laneCoverWhiteNumberText->render(context);
  }
  if (laneCoverVisibleTimeText != nullptr) {
    laneCoverVisibleTimeText->render(context);
  }

  if (judgementIndicator.isEnabled()) {
    const bool indicatorHudMode = judgementIndicator.isHudMode();
    simpleBatchRenderer.setSubmitView(indicatorHudMode ? rendering::ui_view
                                                       : rendering::main_view);
    simpleBatchRenderer.setSubmitDepth(indicatorHudMode
                                           ? 0
                                           : kJudgementIndicatorDepth);
    simpleBatchRenderer.begin();
    judgementIndicator.render(simpleBatchRenderer, micro,
                              {.judgeY = judgeY,
                               .upperBound = upperBound,
                               .playAreaLeftX = playAreaLeftX,
                               .playAreaWidth = playAreaWidth,
                               .noteRenderWidth = noteRenderWidth,
                               .noteRenderHeight = noteRenderHeight});
    simpleBatchRenderer.flush();
    simpleBatchRenderer.setSubmitView(rendering::main_view);
  }

  if (renderHud) {
    if (gaugeBarPosition == AppSettings::GaugeBarPosition::World) {
      simpleBatchRenderer.setSubmitView(rendering::main_view);
      simpleBatchRenderer.setSubmitDepth(kGaugeDepth);
      simpleBatchRenderer.begin();
      drawGaugeBar();
      simpleBatchRenderer.flush();
    }
    layoutCenteredJudgementText();
    layoutGameplayHud();
    simpleBatchRenderer.setSubmitView(rendering::ui_view);
    simpleBatchRenderer.setSubmitDepth(0);
    simpleBatchRenderer.begin();
    drawGameplayHudPanels();
    if (gaugeBarPosition != AppSettings::GaugeBarPosition::World) {
      drawGaugeBar();
    }
    if (judgementCounterEnabled) {
      drawJudgementCounterPanels();
    }
    drawJudgementAccentBar();
    simpleBatchRenderer.flush();
    simpleBatchRenderer.setSubmitView(rendering::main_view);
    drawTitle(context);
    drawJudgement(context);
    drawScore(context);
    if (comboText != nullptr) {
      comboText->render(context);
    }
    drawGauge(context);
    drawPlayOption(context);
    drawAutoPlayMark(context);
    if (judgementCounterEnabled) {
      for (size_t i = 0; i < kHudCounterItemCount; ++i) {
        if (judgementCounterLabelTexts[i] != nullptr) {
          judgementCounterLabelTexts[i]->render(context);
        }
        if (judgementCounterValueTexts[i] != nullptr) {
          judgementCounterValueTexts[i]->render(context);
        }
      }
    }
  }

  if (touchVisualizationEnabled) {
    simpleBatchRenderer.setSubmitView(rendering::ui_view);
    simpleBatchRenderer.setSubmitDepth(1);
    simpleBatchRenderer.begin();
    drawTouchPoints(replayTouchTimeMicros);
    simpleBatchRenderer.flush();
    simpleBatchRenderer.setSubmitView(rendering::main_view);
  }
}

void BMSRenderer::expireLingeringTimingText(long long currentMicros) {
  if (renderedTimingTextUntilMicros <= 0 ||
      currentMicros <= renderedTimingTextUntilMicros) {
    return;
  }

  bool changed = false;
  if (judgementTimingDirectionText != nullptr &&
      judgementTimingDirectionText->getVisible()) {
    judgementTimingDirectionText->setVisible(false);
    judgementTimingDirectionText->setText("");
    changed = true;
  }
  if (judgementTimingMsText != nullptr && judgementTimingMsText->getVisible()) {
    judgementTimingMsText->setVisible(false);
    judgementTimingMsText->setText("");
    changed = true;
  }
  renderedTimingTextUntilMicros = 0;
  if (changed) {
    judgementLayoutWidth = 0;
    judgementLayoutHeight = 0;
  }
}

void BMSRenderer::applyPendingHudText(long long currentMicros) {
  applyPendingPacemakerText();

  const uint32_t revision = hudRevision.load(std::memory_order_acquire);
  if (revision == renderedHudRevision) {
    expireLingeringTimingText(currentMicros);
    return;
  }

  const auto judgement =
      static_cast<Judgement>(pendingJudge.load(std::memory_order_relaxed));
  const int score = pendingScore.load(std::memory_order_relaxed);
  const int combo = pendingCombo.load(std::memory_order_relaxed);
  const long long chartDiffMicros =
      pendingJudgeDiffMicros.load(std::memory_order_relaxed);
  const long long diffMicros = gameplay_timing::realJudgementDiffMicros(
      chartDiffMicros, playbackRate);
  const long long displayTimeMicros =
      pendingJudgeDisplayMicros.load(std::memory_order_relaxed);
  renderedHudRevision = revision;
  renderedJudgement = judgement;
  renderedCombo = combo;

  const bool hasJudgement = judgement != None;
  const bool hasTiming = hasJudgement && diffMicros != 0;
  const bool showTimingDirection =
      hasTiming && judgementMeetsTimingCriteria(
                       judgement, judgementTimingFastSlowCriteria);
  const bool showTimingMs =
      hasTiming && judgementMeetsTimingCriteria(
                       judgement, judgementTimingMillisecondsCriteria);
  const bool showTimingFeedback = showTimingDirection || showTimingMs;
  renderedTimingFastShown = showTimingFeedback && diffMicros < 0;
  renderedTimingSlowShown = showTimingFeedback && diffMicros > 0;
  if (judgeText != nullptr) {
    judgeText->setVisible(hasJudgement);
    std::string judgeLine;
    if (hasJudgement) {
      judgeLine = JudgeResult(judgement, 0).toString();
      if (combo > 0) {
        judgeLine.push_back(' ');
        judgeLine += std::to_string(combo);
      }
    }
    judgeText->setText(judgeLine);
    judgeText->setColor(ui_theme::sdl(hasJudgement
                                          ? hudJudgementTextColor(judgement)
                                          : ui_theme::textPrimary()));
  }
  const Color timingColor = hudTimingColor(diffMicros);
  const bool refreshedTimingText = showTimingDirection || showTimingMs;
  if (refreshedTimingText) {
    renderedTimingTextUntilMicros =
        displayTimeMicros + kJudgementTimingTextLingerMicros;
  }
  const bool keepLingeringTimingText =
      !refreshedTimingText &&
      (judgementTimingFastSlowCriteria !=
           AppSettings::JudgementTimingDisplayCriteria::Off ||
       judgementTimingMillisecondsCriteria !=
           AppSettings::JudgementTimingDisplayCriteria::Off) &&
      renderedTimingTextUntilMicros > currentMicros;
  if (!refreshedTimingText && !keepLingeringTimingText) {
    renderedTimingTextUntilMicros = 0;
  }
  if (judgementTimingDirectionText != nullptr) {
    if (refreshedTimingText || !keepLingeringTimingText) {
      judgementTimingDirectionText->setVisible(showTimingDirection);
      judgementTimingDirectionText->setText(
          showTimingDirection ? (diffMicros < 0 ? "FAST" : "SLOW") : "");
      judgementTimingDirectionText->setColor(ui_theme::sdl(timingColor));
    }
  }
  if (judgementTimingMsText != nullptr) {
    if (refreshedTimingText || !keepLingeringTimingText) {
      judgementTimingMsText->setVisible(showTimingMs);
      judgementTimingMsText->setText(
          showTimingMs
              ? gameplay_timing::formatJudgementTimingMilliseconds(diffMicros)
              : "");
      judgementTimingMsText->setColor(ui_theme::sdl(timingColor));
    }
  }

  scoreText->setText("SCORE " + std::to_string(score));
  if (comboText != nullptr) {
    comboText->setText("COMBO " + std::to_string(combo));
    comboText->setColor(
        ui_theme::sdl(hasJudgement ? hudJudgementComboColor(judgement)
                                   : ui_theme::lime()));
  }
  judgementLayoutWidth = 0;
  judgementLayoutHeight = 0;
}

void BMSRenderer::applyPendingPacemakerText() {
  const uint32_t revision =
      pacemakerRevision.load(std::memory_order_acquire);
  if (revision == renderedPacemakerRevision) {
    return;
  }
  renderedPacemakerRevision = revision;

  if (pacemakerText == nullptr && pacemakerDeltaText == nullptr) {
    return;
  }

  const bool enabled =
      pendingPacemakerEnabled.load(std::memory_order_relaxed);
  if (pacemakerText != nullptr) {
    pacemakerText->setVisible(enabled);
  }
  if (!enabled) {
    if (pacemakerText != nullptr) {
      pacemakerText->setText("");
    }
    if (pacemakerDeltaText != nullptr) {
      pacemakerDeltaText->setVisible(false);
      pacemakerDeltaText->setText("");
    }
    judgementLayoutWidth = 0;
    judgementLayoutHeight = 0;
    return;
  }

  const int delta =
      pendingPacemakerDelta.load(std::memory_order_relaxed);
  const int finalTarget =
      pendingPacemakerFinalTargetScore.load(std::memory_order_relaxed);
  const int playedNotes =
      pendingPacemakerPlayedNotes.load(std::memory_order_relaxed);
  const bool usesReplay =
      pendingPacemakerUsesReplayProgression.load(std::memory_order_relaxed);
  const std::string label = pacemakerLabel.empty() ? "TARGET" : pacemakerLabel;
  std::string text = "PM " + label + " " + std::to_string(finalTarget);
  if (usesReplay) {
    text += " GHOST";
  }
  if (pacemakerText != nullptr) {
    pacemakerText->setText(text);
    pacemakerText->setColor(ui_theme::sdl(ui_theme::textSecondary()));
  }
  if (pacemakerDeltaText != nullptr) {
    pacemakerDeltaText->setVisible(playedNotes > 0);
    pacemakerDeltaText->setText(playedNotes > 0 ? formatSignedScoreDelta(delta)
                                                : "");
    pacemakerDeltaText->setColor(
        ui_theme::sdl(delta >= 0 ? ui_theme::lime() : ui_theme::coral()));
  }
  judgementLayoutWidth = 0;
  judgementLayoutHeight = 0;
}

void BMSRenderer::updateJudgementCounterText() {
  const uint32_t revision =
      judgementCounterRevision.load(std::memory_order_acquire);
  if (revision == renderedJudgementCounterRevision) {
    return;
  }

  JudgementCounterSnapshot snapshot;
  snapshot.pgreat = judgementCounterValues[0].load(std::memory_order_relaxed);
  snapshot.great = judgementCounterValues[1].load(std::memory_order_relaxed);
  snapshot.good = judgementCounterValues[2].load(std::memory_order_relaxed);
  snapshot.bad = judgementCounterValues[3].load(std::memory_order_relaxed);
  snapshot.poor = judgementCounterValues[4].load(std::memory_order_relaxed);
  snapshot.kpoor = judgementCounterValues[5].load(std::memory_order_relaxed);
  snapshot.comboBreak =
      judgementCounterValues[6].load(std::memory_order_relaxed);
  renderedJudgementCounterRevision = revision;
  renderedJudgementCounterSnapshot = snapshot;
  const bool topPosition =
      judgementCounterPosition == AppSettings::JudgementCounterPosition::Top;
  for (size_t i = 0; i < kHudCounterItemCount; ++i) {
    const int value = counterValueAt(snapshot, i);
    if (judgementCounterValueTexts[i] != nullptr) {
      judgementCounterValueTexts[i]->setText(std::to_string(value));
    }
    if (judgementCounterLabelTexts[i] != nullptr) {
      judgementCounterLabelTexts[i]->setColor(
          ui_theme::sdl(hudCounterLabelColor(i, value, topPosition)));
    }
    if (judgementCounterValueTexts[i] != nullptr) {
      judgementCounterValueTexts[i]->setColor(
          ui_theme::sdl(hudCounterValueColor(i, value, topPosition)));
    }
  }
}

void BMSRenderer::reset() {
  state.reset();
  floatingVisibleTimeReferenceBpm.reset();
  judgementIndicator.clear();
  pendingJudge.store(None, std::memory_order_relaxed);
  pendingScore.store(0, std::memory_order_relaxed);
  pendingCombo.store(0, std::memory_order_relaxed);
  pendingJudgeDiffMicros.store(0, std::memory_order_relaxed);
  pendingJudgeDisplayMicros.store(0, std::memory_order_relaxed);
  renderedTimingTextUntilMicros = 0;
  renderedTimingFastShown = false;
  renderedTimingSlowShown = false;
  hudRevision.fetch_add(1, std::memory_order_release);
  pacemakerRevision.fetch_add(1, std::memory_order_release);
  publishJudgementCounterSnapshot({});
  renderedJudgementCounterSnapshot = {};
  replayTouchCursor = 0;
  lastReplayTouchTimeMicros = -1;
  replayActiveTouchSamples.clear();
  replayReleasedTouchSamples.clear();
  liveTouchSamples.clear();
  liveReleasedTouchSamples.clear();

  for (auto &laneState : laneStatesByOrder) {
    laneState.lastPressedJudgement.store(None, std::memory_order_relaxed);
    laneState.lastPressedDiff.store(0, std::memory_order_relaxed);
    laneState.lastPressedTime.store(-1, std::memory_order_relaxed);
    laneState.isPressed.store(false, std::memory_order_relaxed);
    laneState.lastStateTime.store(-1, std::memory_order_release);
  }
}

void BMSRenderer::refreshGeometry() {
  upperBound = calculateLanePlaneScreenTopIntersection();
  noteVisibleUpperBound = upperBound;
}

void BMSRenderer::setVisibleTimeGreenNumber(int greenNumber) {
  visibleTimeGreenNumber = greenNumber;
}

void BMSRenderer::setVisibleTimeUseMilliseconds(bool enabled) {
  visibleTimeUseMilliseconds = enabled;
}

void BMSRenderer::setCurrentBpm(double bpm) {
  if (std::isfinite(bpm) && bpm > 0.0) {
    currentBpm = bpm;
  }
}

void BMSRenderer::setVisibleTimeBpmStrategy(
    AppSettings::VisibleTimeBpmStrategy strategy) {
  visibleTimeBpmStrategy = strategy;
}

void BMSRenderer::setPlayAreaWidth(float width) {
  if (!std::isfinite(width)) {
    width = AppSettings::kDefaultPlayAreaWidth;
  }
  const float sanitized =
      std::clamp(width, AppSettings::kMinPlayAreaWidth,
                 AppSettings::kMaxPlayAreaWidth);
  if (std::abs(sanitized - playAreaWidth) <= 0.001f) {
    return;
  }
  playAreaWidth = sanitized;
  rebuildPlayAreaGeometry();
}

void BMSRenderer::setLaneBeamsEnabled(bool enabled) {
  renderLaneBeams = enabled;
}

void BMSRenderer::setLaneCoverFloatingEnabled(bool enabled) {
  laneCoverFloatingEnabled = enabled;
}

std::optional<std::array<std::pair<float, float>, 4>>
BMSRenderer::gameplayTouchBoundsUi() const {
  const auto bottomLeft = projectLanePointToUi(playAreaLeftX, judgeY);
  const auto bottomRight =
      projectLanePointToUi(playAreaLeftX + playAreaWidth, judgeY);
  const auto topLeft = projectLanePointToUi(playAreaLeftX, upperBound);
  const auto topRight =
      projectLanePointToUi(playAreaLeftX + playAreaWidth, upperBound);
  if (!bottomLeft || !bottomRight || !topLeft || !topRight) {
    return std::nullopt;
  }
  return std::array{*bottomLeft, *bottomRight, *topLeft, *topRight};
}

void BMSRenderer::setLaneBeamLengthPercent(int percent) {
  laneBeamLengthPercent =
      std::clamp(percent, AppSettings::kMinLaneBeamLengthPercent,
                 AppSettings::kMaxLaneBeamLengthPercent);
}

void BMSRenderer::setNoteStartPositionPercent(int percent) {
  noteStartPositionPercent =
      std::clamp(percent, AppSettings::kMinNoteStartPositionPercent,
                 AppSettings::kMaxNoteStartPositionPercent);
}

void BMSRenderer::applyLaneCoverState(int percent,
                                      bool resetVisibleTimeReference) {
  setNoteStartPositionPercent(percent);
  if (resetVisibleTimeReference) {
    floatingVisibleTimeReferenceBpm =
        currentBpm > 0.0 ? currentBpm : visibleTimeReferenceBpm();
  }
}

std::optional<bx::Vec3>
BMSRenderer::lanePlanePointAtRenderPosition(float renderX,
                                            float renderY) const {
  if (!std::isfinite(renderX) || !std::isfinite(renderY)) {
    return std::nullopt;
  }

  const bx::Vec3 nearPoint = rendering::game_camera.deproject(
      renderX, renderY, rendering::game_camera.getNearClip());
  const bx::Vec3 farPoint = rendering::game_camera.deproject(
      renderX, renderY, rendering::game_camera.getFarClip());
  const bx::Vec3 ray = {farPoint.x - nearPoint.x, farPoint.y - nearPoint.y,
                        farPoint.z - nearPoint.z};
  if (std::abs(ray.z) <= 0.0001f) {
    return std::nullopt;
  }

  const float t = -nearPoint.z / ray.z;
  if (!std::isfinite(t)) {
    return std::nullopt;
  }
  return bx::Vec3{nearPoint.x + ray.x * t, nearPoint.y + ray.y * t, 0.0f};
}

BMSRenderer::LaneCoverHandleGeometry
BMSRenderer::laneCoverHandleGeometry() const {
  const float coverHeight = std::max(0.0f, upperBound - noteVisibleUpperBound);
  const float handleWidth = std::clamp(playAreaWidth * 0.28f,
                                       noteRenderWidth * 1.75f,
                                       playAreaWidth * 0.48f);
  if (coverHeight <= 0.001f) {
    return {.x = playAreaLeftX + (playAreaWidth - handleWidth) * 0.5f,
            .y = noteVisibleUpperBound,
            .width = handleWidth,
            .height = 0.0f};
  }

  const float desiredHeight = std::max(noteRenderHeight * 0.85f, 0.10f);
  const float edgeInset =
      std::min(std::max(noteRenderHeight * 0.16f, 0.025f),
               coverHeight * 0.28f);
  const float handleHeight =
      std::min(desiredHeight, std::max(0.0f, coverHeight - edgeInset));
  const float handleY =
      std::clamp(noteVisibleUpperBound + edgeInset, noteVisibleUpperBound,
                 std::max(noteVisibleUpperBound, upperBound - handleHeight));
  return {.x = playAreaLeftX + (playAreaWidth - handleWidth) * 0.5f,
          .y = handleY,
          .width = handleWidth,
          .height = handleHeight};
}

std::optional<BMSRenderer::LaneCoverVirtualHandleGeometry>
BMSRenderer::laneCoverVirtualHandleGeometry() const {
  const LaneCoverHandleGeometry handle = laneCoverHandleGeometry();
  if (handle.width <= 0.0f || handle.height <= 0.0f) {
    return std::nullopt;
  }

  const float handleBottomWorldY = handle.y;
  const float handleTopWorldY = handle.y + handle.height;
  const float handleCenterWorldX = handle.x + handle.width * 0.5f;
  const auto bottomCenter =
      projectLanePointToUi(handleCenterWorldX, handleBottomWorldY);
  const auto bottomLeft = projectLanePointToUi(handle.x, handleBottomWorldY);
  const auto bottomRight =
      projectLanePointToUi(handle.x + handle.width, handleBottomWorldY);
  const auto topCenter =
      projectLanePointToUi(handleCenterWorldX, handleTopWorldY);
  if (!bottomCenter || !bottomLeft || !bottomRight || !topCenter) {
    return std::nullopt;
  }

  const float projectedWidth =
      std::abs(bottomRight->first - bottomLeft->first);
  const float projectedHeight =
      std::abs(bottomCenter->second - topCenter->second);
  const float minWidth = std::clamp(
      static_cast<float>(rendering::window_width) * 0.16f, 110.0f, 220.0f);
  const float maxWidth =
      std::max(minWidth, std::min(static_cast<float>(rendering::window_width) *
                                      0.44f,
                                  360.0f));
  const float minHeight = std::clamp(
      static_cast<float>(rendering::window_height) * 0.055f, 48.0f, 82.0f);
  const float maxHeight =
      std::max(minHeight, std::min(static_cast<float>(rendering::window_height) *
                                       0.13f,
                                   112.0f));
  const float width =
      std::clamp(std::max(projectedWidth, minWidth), minWidth, maxWidth);
  const float height =
      std::clamp(std::max(projectedHeight, minHeight), minHeight, maxHeight);
  return LaneCoverVirtualHandleGeometry{
      .x = bottomCenter->first - width * 0.5f,
      .y = bottomCenter->second - height,
      .width = width,
      .height = height,
  };
}

bool BMSRenderer::isLaneCoverHandleHit(float renderX, float renderY) const {
  return laneCoverHandleGrabOffset(renderX, renderY).has_value();
}

std::optional<float>
BMSRenderer::laneCoverHandleGrabOffset(float renderX, float renderY) const {
  if (!laneCoverFloatingEnabled) {
    return std::nullopt;
  }
  const auto point = lanePlanePointAtRenderPosition(renderX, renderY);
  if (!point.has_value()) {
    return std::nullopt;
  }

  const LaneCoverHandleGeometry handle = laneCoverHandleGeometry();
  if (handle.width <= 0.0f || handle.height <= 0.0f) {
    return std::nullopt;
  }

  const float hitSlopX = std::max(noteRenderWidth * 0.35f, 0.12f);
  const float hitSlopY = std::max(noteRenderHeight * 0.55f, 0.10f);
  const float coverHeight = std::max(0.0f, upperBound - noteVisibleUpperBound);
  const float hitMinY = std::max(noteVisibleUpperBound, handle.y - hitSlopY);
  const float hitMaxY =
      std::min(upperBound, handle.y + handle.height + hitSlopY);
  if (point->x < handle.x - hitSlopX ||
      point->x > handle.x + handle.width + hitSlopX || point->y < hitMinY ||
      point->y > hitMaxY) {
    if (const auto virtualHandle = laneCoverVirtualHandleGeometry();
        virtualHandle.has_value()) {
      float uiX = 0.0f;
      float uiY = 0.0f;
      rendering::screenToUi(renderX, renderY, uiX, uiY);
      const bool insideVirtualHandle =
          uiX >= virtualHandle->x &&
          uiX <= virtualHandle->x + virtualHandle->width &&
          uiY >= virtualHandle->y &&
          uiY <= virtualHandle->y + virtualHandle->height;
      if (!insideVirtualHandle) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }

  return std::clamp(point->y - noteVisibleUpperBound, 0.0f, coverHeight);
}

int BMSRenderer::dragLaneCoverHandleTo(float renderX, float renderY,
                                       float lanePointYOffset) {
  const auto point = lanePlanePointAtRenderPosition(renderX, renderY);
  if (!point.has_value()) {
    return noteStartPositionPercent;
  }

  const float laneHeight = std::max(0.001f, upperBound - judgeY);
  const float maxHiddenRatio =
      static_cast<float>(AppSettings::kMaxNoteStartPositionPercent) / 100.0f;
  const float minVisibleY = judgeY + laneHeight * (1.0f - maxHiddenRatio);
  const float anchorOffset =
      std::isfinite(lanePointYOffset) ? std::max(0.0f, lanePointYOffset)
                                      : 0.0f;
  const float targetY =
      std::clamp(point->y - anchorOffset, minVisibleY, upperBound);
  const float hiddenRatio = 1.0f - ((targetY - judgeY) / laneHeight);
  setNoteStartPositionPercent(std::clamp(
      static_cast<int>(std::lround(hiddenRatio * 100.0f)),
      AppSettings::kMinNoteStartPositionPercent,
      AppSettings::kMaxNoteStartPositionPercent));
  return noteStartPositionPercent;
}

void BMSRenderer::setLaneBeamClockUsesRenderTime(bool enabled) {
  useRenderTimeForLaneBeams = enabled;
}

void BMSRenderer::setShowInvisibleNotes(bool enabled) {
  showInvisibleNotes = enabled;
}

void BMSRenderer::setJudgementIndicatorConfig(bool enabled, float y,
                                              float widthScale, bool hudMode,
                                              int rangeMilliseconds) {
  judgementIndicator.configure(enabled, y, widthScale, hudMode,
                               rangeMilliseconds);
}

void BMSRenderer::setJudgementTextY(float y) {
  const float clamped =
      std::clamp(y, AppSettings::kMinJudgementTextY,
                 AppSettings::kMaxJudgementTextY);
  if (std::abs(judgementTextY - clamped) <= 0.0001f) {
    return;
  }
  judgementTextY = clamped;
  judgementLayoutWidth = 0;
  judgementLayoutHeight = 0;
}

void BMSRenderer::setJudgementCounterEnabled(bool enabled) {
  judgementCounterEnabled = enabled;
}

void BMSRenderer::setJudgementCounterPosition(
    AppSettings::JudgementCounterPosition position) {
  if (judgementCounterPosition == position) {
    return;
  }
  judgementCounterPosition = position;
  renderedJudgementCounterRevision =
      judgementCounterRevision.load(std::memory_order_relaxed) - 1;
}

void BMSRenderer::setJudgementTimingFastSlowCriteria(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  if (judgementTimingFastSlowCriteria == criteria) {
    return;
  }
  judgementTimingFastSlowCriteria = criteria;
  hudRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setJudgementTimingMillisecondsCriteria(
    AppSettings::JudgementTimingDisplayCriteria criteria) {
  if (judgementTimingMillisecondsCriteria == criteria) {
    return;
  }
  judgementTimingMillisecondsCriteria = criteria;
  hudRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setGaugeBarPosition(AppSettings::GaugeBarPosition position) {
  switch (position) {
  case AppSettings::GaugeBarPosition::World:
  case AppSettings::GaugeBarPosition::Left:
  case AppSettings::GaugeBarPosition::Right:
    gaugeBarPosition = position;
    refreshGaugeTextStyle();
    break;
  }
}

void BMSRenderer::publishJudgementCounterSnapshot(
    const JudgementCounterSnapshot &snapshot) {
  for (size_t i = 0; i < kJudgementCounterItemCount; ++i) {
    judgementCounterValues[i].store(counterValueAt(snapshot, i),
                                    std::memory_order_relaxed);
  }
  judgementCounterRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setJudgementCounter(Judgement judgement, int count,
                                      int comboBreak) {
  const int index = counterIndexForJudgement(judgement);
  if (index >= 0) {
    judgementCounterValues[static_cast<size_t>(index)].store(
        count, std::memory_order_relaxed);
  }
  judgementCounterValues[6].store(comboBreak, std::memory_order_relaxed);
  judgementCounterRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setJudgementCounters(
    const std::map<Judgement, int> &judgeCounts, int comboBreak) {
  JudgementCounterSnapshot snapshot;
  snapshot.pgreat = judgeCountFor(judgeCounts, PGreat);
  snapshot.great = judgeCountFor(judgeCounts, Great);
  snapshot.good = judgeCountFor(judgeCounts, Good);
  snapshot.bad = judgeCountFor(judgeCounts, Bad);
  snapshot.poor = judgeCountFor(judgeCounts, Poor);
  snapshot.kpoor = judgeCountFor(judgeCounts, Kpoor);
  snapshot.comboBreak = comboBreak;

  publishJudgementCounterSnapshot(snapshot);
}

void BMSRenderer::setGaugeStatus(GaugeType gaugeType,
                                 GaugeAutoShiftMode gaugeAutoShift,
                                 float currentGauge,
                                 GaugeProfile gaugeProfile) {
  currentGaugeType = gaugeType;
  currentGaugeProfile = gaugeProfile;
  currentGaugeAutoShift = gaugeAutoShift;
  currentGaugeValue =
      std::clamp(currentGauge, 0.0f,
                 gaugeMaximumValue(currentGaugeType, currentGaugeProfile));
  if (gaugeText == nullptr) {
    return;
  }

  refreshGaugeTextStyle();
}

void BMSRenderer::setPacemakerTarget(const pacemaker::Target &target) {
  pacemakerLabel = target.label;
  pendingPacemakerEnabled.store(target.enabled, std::memory_order_relaxed);
  pendingPacemakerFinalTargetScore.store(target.finalScore,
                                         std::memory_order_relaxed);
  pendingPacemakerTargetScore.store(0, std::memory_order_relaxed);
  pendingPacemakerCurrentScore.store(0, std::memory_order_relaxed);
  pendingPacemakerDelta.store(0, std::memory_order_relaxed);
  pendingPacemakerPlayedNotes.store(0, std::memory_order_relaxed);
  pendingPacemakerTotalNotes.store(target.totalNotes,
                                   std::memory_order_relaxed);
  pendingPacemakerUsesReplayProgression.store(target.usesReplayProgression,
                                              std::memory_order_relaxed);
  pacemakerRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setPacemakerStatus(const pacemaker::Snapshot &snapshot) {
  pendingPacemakerEnabled.store(snapshot.enabled, std::memory_order_relaxed);
  pendingPacemakerCurrentScore.store(snapshot.currentScore,
                                     std::memory_order_relaxed);
  pendingPacemakerTargetScore.store(snapshot.targetScore,
                                    std::memory_order_relaxed);
  pendingPacemakerFinalTargetScore.store(snapshot.finalTargetScore,
                                         std::memory_order_relaxed);
  pendingPacemakerDelta.store(snapshot.delta, std::memory_order_relaxed);
  pendingPacemakerPlayedNotes.store(snapshot.playedNotes,
                                    std::memory_order_relaxed);
  pendingPacemakerTotalNotes.store(snapshot.totalNotes,
                                   std::memory_order_relaxed);
  pendingPacemakerUsesReplayProgression.store(snapshot.usesReplayProgression,
                                              std::memory_order_relaxed);
  pacemakerRevision.fetch_add(1, std::memory_order_release);
}

void BMSRenderer::setPlayOptionStatus(const std::string &label) {
  if (playOptionText == nullptr) {
    return;
  }

  playOptionText->setVisible(!label.empty());
  playOptionText->setText(label);
}

void BMSRenderer::setReplayData(const JudgedPlaybackData *replayData) {
  replayGhostEvents.clear();
  replayMissMarkers.clear();
  replayTouchSamples.clear();
  replayActiveTouchSamples.clear();
  replayReleasedTouchSamples.clear();
  replayTouchCursor = 0;
  lastReplayTouchTimeMicros = -1;
  if (replayData == nullptr) {
    return;
  }

  std::vector<const bms_parser::TimeLine *> timelineRefs;
  timelineRefs.reserve(timelines.size());
  for (const auto *timeline : timelines) {
    timelineRefs.push_back(timeline);
  }
  replayGhostEvents = replay_ghost::buildReplayGhostEvents(
      *replayData, timelineRefs, laneToOrderIndex,
      [this](long long timeMicros) { return scrollPositionAtTime(timeMicros); });
  replayMissMarkers = replay_ghost::buildReplayMissMarkers(
      *replayData, timelineRefs, laneToOrderIndex,
      [this](long long timeMicros) { return scrollPositionAtTime(timeMicros); });
  replayTouchSamples = replayData->touchSamples;
  std::stable_sort(replayTouchSamples.begin(), replayTouchSamples.end(),
                   [](const ReplayTouchSample &a, const ReplayTouchSample &b) {
                     return a.songTimeMicros < b.songTimeMicros;
                   });
}

void BMSRenderer::setReplayTouchSamples(
    const std::vector<replay::ReplayTouchSample> &samples) {
  replayTouchSamples.clear();
  replayActiveTouchSamples.clear();
  replayReleasedTouchSamples.clear();
  replayTouchCursor = 0;
  lastReplayTouchTimeMicros = -1;
  replayTouchSamples.reserve(samples.size());
  for (const auto &sample : samples) {
    replayTouchSamples.push_back(
        {.action = static_cast<::ReplayTouchAction>(sample.action),
         .fingerId = sample.fingerId,
         .songTimeMicros = sample.songTimeMicros,
         .x = sample.x,
         .y = sample.y});
  }
  std::stable_sort(replayTouchSamples.begin(), replayTouchSamples.end(),
                   [](const ReplayTouchSample &left,
                      const ReplayTouchSample &right) {
                     return left.songTimeMicros < right.songTimeMicros;
                   });
}

void BMSRenderer::setAutoPlayMarkVisible(bool visible) {
  autoPlayMarkVisible = visible;
  if (autoPlayMarkText != nullptr) {
    autoPlayMarkText->setVisible(visible);
  }
}

void BMSRenderer::setTouchVisualizationEnabled(bool enabled) {
  touchVisualizationEnabled = enabled;
}

void BMSRenderer::setReplayGhostRenderingEnabled(bool enabled) {
  replayGhostRenderingEnabled = enabled;
}

void BMSRenderer::setStartLaneIndicators(std::vector<int> lanes) {
  std::ranges::sort(lanes);
  lanes.erase(std::unique(lanes.begin(), lanes.end()), lanes.end());
  startLaneIndicatorLanes = std::move(lanes);
}

void BMSRenderer::setStartLaneIndicatorsVisible(bool visible) {
  startLaneIndicatorsVisible = visible;
}

void BMSRenderer::setLiveTouchPoint(long long fingerId,
                                    ReplayTouchAction action, float x,
                                    float y, long long songTimeMicros) {
  ReplayTouchSample sample;
  sample.action = action;
  sample.fingerId = fingerId;
  sample.songTimeMicros = songTimeMicros;
  sample.x = x;
  sample.y = y;
  applyTouchSample(liveTouchSamples, liveReleasedTouchSamples, sample);
}

void BMSRenderer::clearLiveTouchPoints() {
  liveTouchSamples.clear();
  liveReleasedTouchSamples.clear();
}

void BMSRenderer::drawRect(float width, float height, float x, float y,
                           Color color) {
  simpleBatchRenderer.addRect(x, y, width, height, color.toABGR());
}
void BMSRenderer::drawLaneBeam(int lane, const LaneState &laneState,
                               const long long time) {
  if (laneState.lastStateTime == -1) {
    return;
  }
  // alpha
  const bool scratchLane = isScratch(lane);
  const long long lastPressedTime = laneState.lastPressedTime != -1
                                        ? laneState.lastPressedTime
                                        : laneState.lastStateTime;
  const auto pressedAlphaAt = [scratchLane, lastPressedTime](
                                  long long referenceTime) {
    if (scratchLane) {
      return scratchLaneBeamPressedAlpha(referenceTime - lastPressedTime);
    }
    return kLaneBeamMaxAlpha;
  };
  double alpha;
  if (laneState.isPressed) {
    alpha = pressedAlphaAt(time);
  } else {
    // fade out
    alpha = laneBeamReleaseAlpha(pressedAlphaAt(laneState.lastStateTime),
                                 time - laneState.lastStateTime);
  }
  if (alpha <= 0.0) {
    return;
  }
  if (alpha > 1.0) {
    alpha = 1.0;
  }
  auto color = Color(255, 255, 255, 255 * alpha);

  if (laneState.lastPressedJudge.judgement == PGreat) {
    color = Color(255, 128, 0, 255 * alpha);
  } else if (laneState.lastPressedJudge.judgement == None) {
    color = Color(255, 255, 255, 255 * alpha);
  } else {
    color = laneState.lastPressedJudge.Diff > 0 ? Color(255, 0, 0, 255 * alpha)
                                                : Color(0, 0, 255, 255 * alpha);
  }
  const float beamScale = static_cast<float>(laneBeamLengthPercent) / 100.0f;
  const float beamHeight = std::max(0.0f, upperBound - judgeY) * beamScale;
  if (beamHeight <= 0.0f) {
    return;
  }
  const auto fadedColor = Color(color.r, color.g, color.b, 0);
  simpleBatchRenderer.addRectVerticalGradient(
      laneToX(lane), judgeY, noteRenderWidth, beamHeight, color.toABGR(),
      fadedColor.toABGR());
}

void BMSRenderer::drawStartLaneIndicators() {
  if (!startLaneIndicatorsVisible) {
    return;
  }

  for (const int lane : startLaneIndicatorLanes) {
    const auto colorRole = startLaneIndicatorColorRoles.find(lane);
    if (colorRole == startLaneIndicatorColorRoles.end()) {
      continue;
    }
    const auto triangle = start_lane_indicator::placeTriangle(
        laneToX(lane), noteRenderWidth, judgeY, noteVisibleUpperBound);
    Color color;
    switch (colorRole->second) {
    case start_lane_indicator::ColorRole::Blue:
      color = Color(40, 130, 255, 255);
      break;
    case start_lane_indicator::ColorRole::Red:
      color = Color(255, 55, 65, 255);
      break;
    case start_lane_indicator::ColorRole::White:
    default:
      color = Color(255, 255, 255, 255);
      break;
    }
    simpleBatchRenderer.addTriangle(
        triangle.leftX, triangle.baseY, triangle.rightX, triangle.baseY,
        triangle.tipX, triangle.tipY, color.toABGR());
  }
}

void BMSRenderer::layoutLaneCoverNumberTexts() {
  if (laneCoverWhiteNumberText == nullptr ||
      laneCoverVisibleTimeText == nullptr) {
    return;
  }

  const auto hideLabels = [this] {
    laneCoverWhiteNumberText->setVisible(false);
    laneCoverVisibleTimeText->setVisible(false);
  };

  const float coverHeight = upperBound - noteVisibleUpperBound;
  if (coverHeight <= std::max(0.18f, noteRenderHeight * 1.2f)) {
    hideLabels();
    return;
  }

  const LaneCoverHandleGeometry handle = laneCoverHandleGeometry();
  const bool handleVisible = laneCoverFloatingEnabled && handle.height > 0.0f;
  const float labelBottomWorldY =
      handleVisible ? handle.y + handle.height : noteVisibleUpperBound;
  if (labelBottomWorldY >= upperBound) {
    hideLabels();
    return;
  }

  const auto center =
      projectLanePointToUi(playAreaLeftX + playAreaWidth * 0.5f,
                           labelBottomWorldY);
  const auto left = projectLanePointToUi(playAreaLeftX, labelBottomWorldY);
  const auto right =
      projectLanePointToUi(playAreaLeftX + playAreaWidth, labelBottomWorldY);
  const auto coverTop =
      projectLanePointToUi(playAreaLeftX + playAreaWidth * 0.5f, upperBound);
  if (!center || !left || !right || !coverTop) {
    hideLabels();
    return;
  }

  laneCoverWhiteNumberText->setText(
      lane_cover_number::whiteNumberLabel(noteStartPositionPercent));
  laneCoverVisibleTimeText->setText(laneCoverVisibleTimeLabel());
  const int maxProjectedWidth =
      static_cast<int>(std::round(std::abs(right->first - left->first)));
  const int whiteWidth =
      std::max(72, laneCoverWhiteNumberText->textureWidth() + 28);
  const int greenWidth =
      std::max(72, laneCoverVisibleTimeText->textureWidth() + 28);
  constexpr int kNumberGap = 12;
  const int pairWidth = whiteWidth + kNumberGap + greenWidth;
  if (pairWidth > maxProjectedWidth - 24 ||
      pairWidth > rendering::window_width) {
    hideLabels();
    return;
  }

  auto pair = lane_cover_number::centerPair(
      static_cast<int>(std::round(center->first)), whiteWidth, greenWidth,
      kNumberGap);
  const int pairLeft = std::clamp(
      pair.whiteX, 0, std::max(0, rendering::window_width - pairWidth));
  const int horizontalShift = pairLeft - pair.whiteX;
  constexpr int kTextHeight = 34;
  constexpr int kLabelEdgeGap = 6;
  const int labelBottomY =
      static_cast<int>(std::round(center->second)) - kLabelEdgeGap;
  const int coverTopY =
      static_cast<int>(std::floor(std::min(coverTop->second, center->second)));
  if (labelBottomY - kTextHeight < coverTopY) {
    hideLabels();
    return;
  }
  const int y = std::clamp(
      labelBottomY - kTextHeight, 0,
      std::max(0, rendering::window_height - kTextHeight));
  laneCoverWhiteNumberText->setPositionNoLayout(pair.whiteX + horizontalShift,
                                                y);
  laneCoverWhiteNumberText->setSize(whiteWidth, kTextHeight);
  laneCoverWhiteNumberText->setVisible(true);
  laneCoverVisibleTimeText->setPositionNoLayout(pair.greenX + horizontalShift,
                                                y);
  laneCoverVisibleTimeText->setSize(greenWidth, kTextHeight);
  laneCoverVisibleTimeText->setVisible(true);
}

void BMSRenderer::drawLaneCover() {
  const float coverHeight = upperBound - noteVisibleUpperBound;
  if (coverHeight > 0.001f) {
    drawRect(playAreaWidth, coverHeight, playAreaLeftX, noteVisibleUpperBound,
             Color(9, 12, 18, 255));

    const float edgeHeight = std::max(0.025f, noteRenderHeight * 0.12f);
    drawRect(playAreaWidth, edgeHeight, playAreaLeftX,
             noteVisibleUpperBound - edgeHeight * 0.5f,
             Color(214, 224, 236, 255));
  }

  if (!laneCoverFloatingEnabled) {
    return;
  }

  const LaneCoverHandleGeometry handle = laneCoverHandleGeometry();
  if (handle.width <= 0.0f || handle.height <= 0.0f) {
    return;
  }
  drawRect(handle.width, handle.height, handle.x, handle.y,
           Color(214, 224, 236, 246));

  const float grooveWidth = handle.width * 0.48f;
  const float grooveHeight = std::max(0.012f, handle.height * 0.11f);
  const float grooveX = handle.x + (handle.width - grooveWidth) * 0.5f;
  const float grooveGap = grooveHeight * 1.75f;
  const float grooveY = handle.y + handle.height * 0.34f;
  drawRect(grooveWidth, grooveHeight, grooveX, grooveY,
           Color(31, 43, 58, 210));
  drawRect(grooveWidth, grooveHeight, grooveX, grooveY + grooveGap,
           Color(31, 43, 58, 210));
}

inline bool BMSRenderer::isLeftScratch(int lane) const {
  return scratchLaneCount > 0 && lane == 7;
}
inline bool BMSRenderer::isRightScratch(int lane) const {
  return scratchLaneCount > 1 && lane == 15;
}
inline bool BMSRenderer::isScratch(int lane) const {
  return isLeftScratch(lane) || isRightScratch(lane);
}
void BMSRenderer::rebuildPlayAreaGeometry() {
  playAreaLeftX = gameplay_geometry::playAreaLeft(playAreaWidth);
  noteRenderWidth =
      laneOrder.empty()
          ? gameplay_geometry::standardNoteWidth(playAreaWidth)
          : playAreaWidth / static_cast<float>(laneOrder.size());
  if (noteImageWidth > 0.0f) {
    noteRenderHeight = static_cast<float>(noteImageHeight) /
                       static_cast<float>(noteImageWidth) * noteRenderWidth;
    constexpr float kLongBodyOffImageHeight = 12.0f;
    constexpr float kLongBodyOnImageHeight = 24.0f;
    longBodyRenderHeightOff =
        kLongBodyOffImageHeight / static_cast<float>(noteImageWidth) *
        noteRenderWidth;
    longBodyRenderHeightOn =
        kLongBodyOnImageHeight / static_cast<float>(noteImageWidth) *
        noteRenderWidth;
  }
  for (int lane : laneOrder) {
    if (lane < 0 || static_cast<size_t>(lane) >= laneXLookup.size()) {
      continue;
    }
    laneXLookup[static_cast<size_t>(lane)] = computeLaneX(lane);
  }
}
inline float BMSRenderer::computeLaneX(int lane) const {
  if (const auto it = laneToOrderIndex.find(lane);
      it != laneToOrderIndex.end()) {
    return playAreaLeftX + static_cast<float>(it->second) * noteRenderWidth;
  }

  return playAreaLeftX;
}
inline float BMSRenderer::laneToX(int lane) const {
  if (lane >= 0 && static_cast<size_t>(lane) < laneXLookup.size()) {
    const float cachedX = laneXLookup[static_cast<size_t>(lane)];
    if (!std::isnan(cachedX)) {
      return cachedX;
    }
  }
  return computeLaneX(lane);
}

bool BMSRenderer::laneIsCurrentlyPressed(int lane) const {
  const auto it = laneToOrderIndex.find(lane);
  if (it == laneToOrderIndex.end()) {
    return false;
  }
  return laneStatesByOrder[it->second].isPressed.load(std::memory_order_relaxed);
}

inline const NoteSheet &BMSRenderer::sheetForLane(int lane) const {
  if (lane >= 0 && static_cast<size_t>(lane) < laneSheetLookup.size()) {
    if (const auto *sheet = laneSheetLookup[static_cast<size_t>(lane)];
        sheet != nullptr) {
      return *sheet;
    }
  }
  if (isScratch(lane)) {
    return scratchSheet;
  }
  return graySheet;
}

rendering::TexBatchRenderer &
BMSRenderer::noteTextureBatchAtDepth(uint32_t submitDepth) {
  if (activeNoteTextureDepth != submitDepth) {
    noteTextureBatchRenderer.flush();
    noteTextureBatchRenderer.setSubmitDepth(submitDepth);
    activeNoteTextureDepth = submitDepth;
  }
  return noteTextureBatchRenderer;
}

void BMSRenderer::setInvisibleBatchDepth(uint32_t submitDepth) {
  if (activeInvisibleDepth == submitDepth) {
    return;
  }
  gimmickBatchRenderer.flush();
  gimmickBatchRenderer.setSubmitDepth(submitDepth);
  activeInvisibleDepth = submitDepth;
}

void BMSRenderer::beginOrderedNoteBatches() {
  activeNoteTextureDepth = std::numeric_limits<uint32_t>::max();
  activeInvisibleDepth = std::numeric_limits<uint32_t>::max();
  noteTextureBatchRenderer.begin();
  gimmickBatchRenderer.begin();
}

void BMSRenderer::flushOrderedNoteBatches() {
  noteTextureBatchRenderer.flush();
  gimmickBatchRenderer.flush();
}

void BMSRendererState::reset() {
  orphanLongNotes.clear();
  currentTimelineIndex = 0;
}

void BMSRenderer::destroyNoteSheetTextures() {
  destroyNoteSheet(graySheet);
  destroyNoteSheet(blueSheet);
  destroyNoteSheet(scratchSheet);
}

BMSRenderer::~BMSRenderer() { destroyNoteSheetTextures(); }
