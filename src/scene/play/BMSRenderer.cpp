//
// Created by XF on 9/2/2024.
//

#include "BMSRenderer.h"

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

struct UiPoint {
  float x = 0.0f;
  float y = 0.0f;
};

uint64_t noteTextureBatchKey(bgfx::TextureHandle texture,
                             uint32_t submitDepth) {
  return (static_cast<uint64_t>(submitDepth) << 16U) |
         static_cast<uint64_t>(texture.idx);
}

long long latePoorTimingFromWindows(
    const std::map<Judgement, std::pair<long long, long long>> &windows) {
  const auto it = windows.find(Bad);
  return it == windows.end() ? kDefaultLatePoorTimingMicros : it->second.second;
}

bool usesBlueSymmetricKeyColor(size_t keyPosition, size_t keyLaneCount) {
  if (keyLaneCount == 0 || keyPosition >= keyLaneCount) {
    return false;
  }
  const size_t mirroredPosition =
      std::min(keyPosition, keyLaneCount - keyPosition - 1);
  return (mirroredPosition & 1U) != 0;
}

bool wasLongNoteTailReleasedEarly(const bms_parser::LongNote *head) {
  if (head == nullptr || head->Tail == nullptr || !head->Tail->IsPlayed ||
      head->Tail->Timeline == nullptr) {
    return false;
  }
  return head->Tail->PlayedTime < head->Tail->Timeline->Timing;
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

Color hudFastColor() { return ui_theme::cyan(); }

Color hudSlowColor() { return ui_theme::amber(); }

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

float baseGameplayHudTitleWidth() {
  return std::clamp(static_cast<float>(rendering::window_width) * 0.30f,
                    430.0f, 620.0f);
}

float gameplayHudMetricsWidth() {
  return std::clamp(static_cast<float>(rendering::window_width) * 0.28f,
                    430.0f, 540.0f);
}

std::string formatGaugeBarLabel(GaugeType gaugeType, bool gaugeAutoShift,
                                float currentGauge) {
  char text[64];
  std::snprintf(text, sizeof(text), "%s%s %.1f%%",
                gaugeAutoShift ? "GAS " : "", gaugeTypeToShortLabel(gaugeType),
                currentGauge);
  return text;
}

Color gaugeAccentColor(GaugeType gaugeType, float currentGauge) {
  const float border = gaugeBorderValue(gaugeType);
  if (!gaugeIsSurvival(gaugeType) && border > 0.0f &&
      currentGauge < border) {
    return ui_theme::coral();
  }
  return clearLampColorForRank(gaugeTypeToClearRank(gaugeType));
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

Color gaugeTextColor(const Color &accent) {
  return ui_theme::textOn(gaugeFillColor(accent));
}

std::optional<UiPoint> projectWorldToUi(float worldX, float worldY) {
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
  return UiPoint{uiX, uiY};
}

JudgementCounterLayout judgementCounterLayoutFor(
    AppSettings::JudgementCounterPosition position, float titleWidth) {
  JudgementCounterLayout layout;
  layout.horizontal = position == AppSettings::JudgementCounterPosition::Top;
  layout.gap = layout.horizontal ? 8.0f : 6.0f;
  layout.itemWidth =
      layout.horizontal
          ? std::clamp((static_cast<float>(rendering::window_width) - 72.0f -
                        layout.gap * 6.0f) /
                           7.0f,
                       92.0f, 118.0f)
          : 118.0f;
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
    const float pauseLeft = static_cast<float>(rendering::window_width - 104);
    if (layout.x < titleRight + 16.0f ||
        layout.x + totalWidth > pauseLeft) {
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
    int visibleTimeGreenNumber, bool renderHud)
    : judgementIndicator(timingWindows),
      latePoorTiming(latePoorTimingFromWindows(timingWindows)),
      visibleTimeGreenNumber(visibleTimeGreenNumber), renderHud(renderHud),
      chart(chart) {
  auto textureGuard = makeScopeExit([this] { destroyNoteSheetTextures(); });

  scratchLaneCount = chart->Meta.GetScratchLaneCount();
  laneOrder = chart->Meta.GetTotalLaneIndices();
  laneStatesByOrder.resize(laneOrder.size());
  laneToOrderIndex.reserve(laneOrder.size());
  laneStateSnapshot.reserve(laneOrder.size());
  whiteKeyLaneIndices.reserve(laneOrder.size());
  blueKeyLaneIndices.reserve(laneOrder.size());
  scratchLaneIndices.reserve(2);
  noteTextureBatchRenderers.reserve(16);
  noteTextureBatchLookup.reserve(16);
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
    const bool usesBlue =
        usesBlueSymmetricKeyColor(keyPosition, keyLanes.size());
    laneUsesBlueSheet.emplace(lane, usesBlue);
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
  for (const auto &measure : chart->Measures) {
    for (const auto &timeLine : measure->TimeLines) {
      timelines.push_back(timeLine);
      auto &timelineNotes = groupedTimelineNotes.emplace_back();
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
  blueSheet.longBodyOffTexture =
      loadCroppedTexture(spriteLoader2, 0, 120, 128, 12, "blue long body off");
  blueSheet.longBodyOnTexture =
      loadCroppedTexture(spriteLoader2, 0, 132, 128, 24, "blue long body on");
  scratchSheet.longBodyOffTexture = loadCroppedTexture(
      spriteLoader3, 0, 120, 128, 12, "scratch long body off");
  scratchSheet.longBodyOnTexture = loadCroppedTexture(
      spriteLoader3, 0, 132, 128, 24, "scratch long body on");

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
  gaugeText = std::make_unique<TextView>(kHudFontPath, 18);
  gaugeText->setAlign(TextView::CENTER);
  gaugeText->setVAlign(TextView::MIDDLE);
  setGaugeStatus(GaugeType::Normal, false, gaugeInitialValue(GaugeType::Normal));
  playOptionText = std::make_unique<TextView>(kHudFontPath, 19);
  playOptionText->setAlign(TextView::LEFT);
  playOptionText->setVAlign(TextView::MIDDLE);
  playOptionText->setOverflow(TextView::TextOverflow::Marquee);
  playOptionText->setColor(ui_theme::sdl(ui_theme::amber()));
  playOptionText->setVisible(false);

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
void BMSRenderer::drawTitle(RenderContext &context) const {
  titleText->render(context);
}
void BMSRenderer::drawJudgement(RenderContext context) const {
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
}
void BMSRenderer::drawGauge(RenderContext &context) const {
  if (gaugeText != nullptr) {
    gaugeText->render(context);
  }
}
void BMSRenderer::drawPlayOption(RenderContext &context) const {
  playOptionText->render(context);
}

void BMSRenderer::drawHudRoundedPanel(float x, float y, float width,
                                      float height, float radius,
                                      const Color &fill,
                                      const Color &border) {
  drawRoundedPanel(x, y, width, height, radius, 1.0f, fill, border);
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

  if (titleWidth > 1.0f) {
    drawHudRoundedPanel(margin, margin, titleWidth, 82.0f, radius,
                        hudPanelFill(), hudPanelBorder());
  }
  drawHudRoundedPanel(margin, rendering::window_height - 86.0f, metricsWidth,
                      58.0f, radius, hudPanelStrongFill(), hudPanelBorder());
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
  float x = left ? 28.0f
                 : static_cast<float>(rendering::window_width) - 28.0f - width;
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
  const float progress = std::clamp(currentGaugeValue, 0.0f, 100.0f) / 100.0f;
  const Color accent = gaugeAccentColor(currentGaugeType, currentGaugeValue);
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

  const float borderValue = gaugeBorderValue(currentGaugeType);
  if (borderValue > 0.0f) {
    const float markerX = x + width * std::clamp(borderValue / 100.0f, 0.0f,
                                                1.0f);
    const float markerWidth = std::max(0.01f, width * 0.004f);
    simpleBatchRenderer.addRect(markerX - markerWidth * 0.5f,
                                y - height * 0.18f, markerWidth,
                                height * 1.36f, gaugeMarkerColor().toABGR());
  }
}

void BMSRenderer::drawHudGaugeBar() {
  const auto rect = hudGaugeRect();
  const float x = rect[0];
  const float y = rect[1];
  const float width = rect[2];
  const float height = rect[3];
  const float progress = std::clamp(currentGaugeValue, 0.0f, 100.0f) / 100.0f;
  const Color accent = gaugeAccentColor(currentGaugeType, currentGaugeValue);
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

  const float borderValue = gaugeBorderValue(currentGaugeType);
  if (borderValue > 0.0f) {
    const float markerY =
        y + height * (1.0f - std::clamp(borderValue / 100.0f, 0.0f, 1.0f));
    simpleBatchRenderer.addRect(x - 5.0f, markerY - 1.0f, width + 10.0f, 2.0f,
                                gaugeMarkerColor().toABGR());
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
  const JudgementCounterLayout layout =
      judgementCounterLayoutFor(judgementCounterPosition,
                                gameplayHudTitleWidth());
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
  const int compactMetricsY = rendering::window_height - 86;
  placeText(scoreText.get(), margin + 18, compactMetricsY + 9,
            metricsWidth / 2, 40);
  placeText(comboText.get(), margin + metricsWidth / 2 - 8,
            compactMetricsY + 9, metricsWidth / 2 - 10, 40);
  layoutGaugeText();

  const JudgementCounterLayout layout =
      judgementCounterLayoutFor(judgementCounterPosition,
                                gameplayHudTitleWidth());
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

void BMSRenderer::layoutGaugeText() {
  if (gaugeText == nullptr) {
    return;
  }

  gaugeText->setVisible(true);
  if (gaugeBarPosition == AppSettings::GaugeBarPosition::World) {
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

    const float minX =
        std::min({topLeft->x, topRight->x, bottomLeft->x, bottomRight->x});
    const float maxX =
        std::max({topLeft->x, topRight->x, bottomLeft->x, bottomRight->x});
    const float minY =
        std::min({topLeft->y, topRight->y, bottomLeft->y, bottomRight->y});
    const float maxY =
        std::max({topLeft->y, topRight->y, bottomLeft->y, bottomRight->y});
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
  constexpr int textWidth = 126;
  constexpr int textHeight = 44;
  const bool left = gaugeBarPosition == AppSettings::GaugeBarPosition::Left;
  const int x = left ? static_cast<int>(std::round(rect[0] + rect[2] + 10.0f))
                     : static_cast<int>(
                           std::round(rect[0] - textWidth - 10.0f));
  const int y =
      static_cast<int>(std::round(rect[1] + rect[3] - textHeight));
  placeText(gaugeText.get(), x, y, textWidth, textHeight);
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

float BMSRenderer::projectedLaneLeftUiInBand(float bandTop,
                                             float bandBottom) const {
  auto projectToUi = [](float worldX,
                        float worldY) -> std::optional<UiPoint> {
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
    return UiPoint{uiX, uiY};
  };

  const auto leftBottom = projectToUi(playAreaLeftX, judgeY);
  const auto rightBottom = projectToUi(playAreaLeftX + playAreaWidth, judgeY);
  const auto leftTop = projectToUi(playAreaLeftX, upperBound);
  const auto rightTop =
      projectToUi(playAreaLeftX + playAreaWidth, upperBound);
  if (!leftBottom || !rightBottom || !leftTop || !rightTop) {
    return std::numeric_limits<float>::quiet_NaN();
  }

  float minX = std::numeric_limits<float>::infinity();
  auto considerX = [&minX](float x) {
    if (std::isfinite(x)) {
      minX = std::min(minX, x);
    }
  };
  auto considerPoint = [&](const UiPoint &point) {
    if (point.y >= bandTop && point.y <= bandBottom) {
      considerX(point.x);
    }
  };
  auto considerEdge = [&](const UiPoint &a, const UiPoint &b) {
    considerPoint(a);
    considerPoint(b);

    const float minY = std::min(a.y, b.y);
    const float maxY = std::max(a.y, b.y);
    const float dy = b.y - a.y;
    if (std::abs(dy) <= 0.0001f) {
      if (a.y >= bandTop && a.y <= bandBottom) {
        considerX(std::min(a.x, b.x));
      }
      return;
    }

    auto considerAtY = [&](float targetY) {
      if (targetY < minY || targetY > maxY) {
        return;
      }
      const float t = (targetY - a.y) / dy;
      if (t < 0.0f || t > 1.0f) {
        return;
      }
      considerX(a.x + (b.x - a.x) * t);
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
  if (judgementLayoutWidth == rendering::window_width &&
      judgementLayoutHeight == rendering::window_height &&
      judgementLayoutHasTimingDirection == hasTimingDirection &&
      judgementLayoutHasTimingMs == hasTimingMs) {
    return;
  }

  judgementLayoutWidth = rendering::window_width;
  judgementLayoutHeight = rendering::window_height;
  judgementLayoutHasTimingDirection = hasTimingDirection;
  judgementLayoutHasTimingMs = hasTimingMs;

  const int maxAvailableWidth = std::max(1, judgementLayoutWidth - 48);
  const int judgeLineHeight = 68;
  const int timingLineHeight = 28;
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
void BMSRenderer::drawLongNote(float headY, float tailY,
                               bms_parser::LongNote *const &head) {
  // assert head
  assert(!head->IsTail() && "head is tail");
  const bool tailReleasedEarly = wasLongNoteTailReleasedEarly(head);
  if (head->Tail->IsPlayed && !tailReleasedEarly)
    return;
  float startY = head->IsPlayed ? judgeY : headY;
  const float bodyHeight = tailY - startY;
  const float bodyWidth = noteRenderWidth;

  const NoteSheet &sheet = sheetForLane(head->Lane);
  const NoteUvRegion &headUv = sheet.longHead;
  const NoteUvRegion &tailUv = sheet.longTail;
  const auto bodyTexture =
      head->IsHolding ? sheet.longBodyOnTexture : sheet.longBodyOffTexture;

  // Body
  if (bodyHeight > 0.0f && bgfx::isValid(bodyTexture)) {
    float tileV = bodyHeight / (head->IsHolding ? longBodyRenderHeightOn
                                                : longBodyRenderHeightOff);
    longBodyBatchFor(sheet, head->IsHolding)
        .addRect(laneToX(head->Lane), startY, bodyWidth, bodyHeight, 1.0f,
                 tileV, bodyTexture);
  }

  if (!tailReleasedEarly || tailY > judgeY) {
    sheetBatchFor(sheet).addRectUV(laneToX(head->Tail->Lane), tailY,
                                   noteRenderWidth, noteRenderHeight, tailUv.u0,
                                   tailUv.v0, tailUv.u1, tailUv.v1,
                                   sheet.texture);
  }

  if (head->IsPlayed)
    return;

  // Head
  sheetBatchFor(sheet).addRectUV(laneToX(head->Lane), startY, noteRenderWidth,
                                 noteRenderHeight, headUv.u0, headUv.v0,
                                 headUv.u1, headUv.v1, sheet.texture);
}
void BMSRenderer::drawNormalNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed)
    return;

  const NoteSheet &sheet = sheetForLane(note->Lane);

  sheetBatchFor(sheet).addRectUV(laneToX(note->Lane), y, noteRenderWidth,
                                 noteRenderHeight, sheet.note.u0,
                                 sheet.note.v0, sheet.note.u1, sheet.note.v1,
                                 sheet.texture);
}

void BMSRenderer::drawInvisibleNote(float y, bms_parser::Note *const &note) {
  if (note->IsPlayed || note->IsDead) {
    return;
  }

  gimmickBatchRenderer.addRect(laneToX(note->Lane), y, noteRenderWidth,
                               noteRenderHeight,
                               Color(255, 149, 36, 224).toABGR());
}

void BMSRenderer::drawLandmineNote(float y,
                                   bms_parser::LandmineNote *const &note) {
  if (note->IsPlayed || note->IsDead) {
    return;
  }

  gimmickBatchRenderer.addRect(laneToX(note->Lane), y, noteRenderWidth,
                               noteRenderHeight,
                               Color(217, 69, 58, 232).toABGR());
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
      return timelineScrollPositions.front();
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

  double firstVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteRenderHeight) /
          static_cast<double>(rxhs);
  double lastVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  if (firstVisibleScrollPosition > lastVisibleScrollPosition) {
    std::swap(firstVisibleScrollPosition, lastVisibleScrollPosition);
  }

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
    const float ghostY =
        judgeY +
        static_cast<float>(event.judgeScrollPosition - currentScrollPosition) *
            rxhs;
    drawGhostNoteOutline(ghostY, event);
  }
}

void BMSRenderer::drawReplayMissMarkers(float rxhs,
                                        double currentScrollPosition) {
  if (replayMissMarkers.empty() || rxhs <= 0.0f) {
    return;
  }

  double firstVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteRenderHeight) /
          static_cast<double>(rxhs);
  double lastVisibleScrollPosition =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  if (firstVisibleScrollPosition > lastVisibleScrollPosition) {
    std::swap(firstVisibleScrollPosition, lastVisibleScrollPosition);
  }

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
    const float markerY =
        judgeY +
        static_cast<float>(marker.noteScrollPosition - currentScrollPosition) *
            rxhs;
    drawMissMarkerX(markerY, marker);
  }
}

void BMSRenderer::drawGhostNoteOutline(float y, const ReplayGhostEvent &event) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
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
  applyPendingHudText(micro);
  updateJudgementCounterText();

  constexpr uint32_t kDepthBackground = 100;
  constexpr uint32_t kDepthBeams = 180;
  constexpr uint32_t kDepthLongBodies = 190;
  constexpr uint32_t kDepthNotes = 200;
  constexpr uint32_t kDepthGhosts = 250;
  constexpr uint32_t kDepthLaneCover = 320;
  constexpr uint32_t kDepthJudgementIndicator = 330;
  constexpr uint32_t kDepthGauge = 340;

  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kDepthBackground);
  gimmickBatchRenderer.setSubmitView(rendering::main_view);
  gimmickBatchRenderer.setSubmitDepth(kDepthNotes + 1);
  ghostBatchRenderer.setSubmitDepth(kDepthGhosts);
  simpleBatchRenderer.begin();
  gimmickBatchRenderer.begin();
  ghostBatchRenderer.begin();
  beginNoteTextureBatches(kDepthLongBodies, kDepthNotes);
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
      visibleTimeMs;
  const float laneHeight = std::max(0.001f, upperBound - judgeY);
  const float hiddenRatio =
      static_cast<float>(noteStartPositionPercent) / 100.0f;
  noteVisibleUpperBound = judgeY + laneHeight * (1.0f - hiddenRatio);
  const float visibleTravelHeight =
      std::max(0.001f, noteVisibleUpperBound - judgeY);
  float rxhs = visibleTravelHeight * hispeed;
  float y = judgeY;
  const double currentScrollPosition = scrollPositionAtTime(micro);
  auto &longNoteLookahead = longNoteLookaheadScratch;
  longNoteLookahead.clear();
  for (auto *orphanLongNote : state.orphanLongNotes) {
    longNoteLookahead[orphanLongNote] = lowerBound;
  }
  // render timeline
  for (size_t i = state.currentTimelineIndex;
       i < timelines.size() && y < upperBound; i++) {
    const auto &timeLine = timelines[i];
    if (timeLine->Timing >= micro) {
      if (y < judgeY)
        y = judgeY;
      if (i > 0) {
        if (const auto &prevTimeLine = timelines[i - 1];
            prevTimeLine->Timing + prevTimeLine->GetStopDuration() > micro) {
          // when the previous timeline is stopped
          y += (timeLine->BeatPosition - prevTimeLine->BeatPosition) *
               prevTimeLine->Scroll * rxhs;
        } else {
          y += (timeLine->BeatPosition - prevTimeLine->BeatPosition) *
               prevTimeLine->Scroll * (timeLine->Timing - micro) /
               (timeLine->Timing - prevTimeLine->Timing -
                prevTimeLine->GetStopDuration()) *
               rxhs;
        }
      } else {
        y += timeLine->BeatPosition * (timeLine->Timing - micro) /
             timeLine->Timing * rxhs;
      }

      if (timeLine->IsFirstInMeasure) {
        // render measure line
        drawRect(playAreaWidth, 0.05f, playAreaLeftX, y,
                 Color(255, 255, 255, 128));
      }
    } else if (timeLine->Timing >= micro - latePoorTiming) {
      y = judgeY + (micro - timeLine->Timing) /
                       static_cast<float>(latePoorTiming) * lowerBound;
    } else {
      state.currentTimelineIndex = i;
    }
    //    SDL_Log("BeatPosition: %f", timeLine->BeatPosition);
    // Render notes in grouped lane order (white/blue/scratch) to reduce texture
    // switches while keeping per-lane ordering intact.
    auto processNote = [&](bms_parser::Note *note) {
      if (note == nullptr) {
        return;
      }
      if (timeLine->Timing >= micro - latePoorTiming) {
        // note is in the hittable timing
        if (note->IsDead) {
          return;
        }
        if (note->IsLandmineNote()) {
          if (timeLine->Timing >= micro) {
            drawLandmineNote(y, static_cast<bms_parser::LandmineNote *>(note));
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
              drawLongNote(it->second, y, longNote->Head);
              // remove from lookahead
              longNoteLookahead.erase(longNote->Head);
            } else {
              drawLongNote(lowerBound, y, longNote->Head);
            }
          } else {
            longNoteLookahead[longNote] = y;
          }
        } else {
          drawNormalNote(y, note);
        }
      } else {
        // note has passed the last hittable timing
        if (note->IsDead) {
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
            longNoteLookahead[longNote] = lowerBound;
          }
        }
      }
    };

    if (i < groupedTimelineNotes.size()) {
      for (auto *note : groupedTimelineNotes[i]) {
        processNote(note);
      }
    }
    for (const auto &note : timeLine->InvisibleNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= micro) {
        if (showInvisibleNotes) {
          drawInvisibleNote(y, note);
        }
      } else {
        note->IsDead = true;
      }
    }
    for (const auto &note : timeLine->LandmineNotes) {
      if (note == nullptr || note->IsDead) {
        continue;
      }
      if (timeLine->Timing >= micro) {
        drawLandmineNote(y, note);
      }
    }
  }

  // render leftover long notes
  for (const auto &pair : longNoteLookahead) {
    drawLongNote(pair.second, upperBound, pair.first);
  }
  drawReplayGhosts(rxhs, micro, currentScrollPosition);
  drawReplayMissMarkers(rxhs, currentScrollPosition);

  // Flush background/measure pass before notes.
  simpleBatchRenderer.flush();
  flushNoteTextureBatches();
  gimmickBatchRenderer.flush();
  ghostBatchRenderer.flush();

  if (renderLaneBeams) {
    simpleBatchRenderer.setSubmitDepth(kDepthBeams);
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
  simpleBatchRenderer.setSubmitDepth(kDepthLaneCover);
  simpleBatchRenderer.begin();
  drawLaneCover();
  simpleBatchRenderer.flush();

  if (judgementIndicator.isEnabled()) {
    const bool indicatorHudMode = judgementIndicator.isHudMode();
    simpleBatchRenderer.setSubmitView(indicatorHudMode ? rendering::ui_view
                                                       : rendering::main_view);
    simpleBatchRenderer.setSubmitDepth(indicatorHudMode
                                           ? 0
                                           : kDepthJudgementIndicator);
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
      simpleBatchRenderer.setSubmitDepth(kDepthGauge);
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
  const uint32_t revision = hudRevision.load(std::memory_order_acquire);
  if (revision == renderedHudRevision) {
    expireLingeringTimingText(currentMicros);
    return;
  }

  const auto judgement =
      static_cast<Judgement>(pendingJudge.load(std::memory_order_relaxed));
  const int score = pendingScore.load(std::memory_order_relaxed);
  const int combo = pendingCombo.load(std::memory_order_relaxed);
  const long long diffMicros =
      pendingJudgeDiffMicros.load(std::memory_order_relaxed);
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
    const long long absDiffMicros =
        diffMicros < 0 ? -diffMicros : diffMicros;
    const long long ms = (absDiffMicros + 500LL) / 1000LL;
    if (refreshedTimingText || !keepLingeringTimingText) {
      judgementTimingMsText->setVisible(showTimingMs);
      judgementTimingMsText->setText(showTimingMs ? std::to_string(ms) + "ms"
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
  publishJudgementCounterSnapshot({});
  renderedJudgementCounterSnapshot = {};

  for (auto &laneState : laneStatesByOrder) {
    laneState.lastPressedJudgement.store(None, std::memory_order_relaxed);
    laneState.lastPressedDiff.store(0, std::memory_order_relaxed);
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

void BMSRenderer::setLaneBeamClockUsesRenderTime(bool enabled) {
  useRenderTimeForLaneBeams = enabled;
}

void BMSRenderer::setShowInvisibleNotes(bool enabled) {
  showInvisibleNotes = enabled;
}

void BMSRenderer::setJudgementIndicatorConfig(bool enabled, float y,
                                              float widthScale, bool hudMode) {
  judgementIndicator.configure(enabled, y, widthScale, hudMode);
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

void BMSRenderer::setGaugeStatus(GaugeType gaugeType, bool gaugeAutoShift,
                                 float currentGauge) {
  currentGaugeType = gaugeType;
  currentGaugeAutoShift = gaugeAutoShift;
  currentGaugeValue = std::clamp(currentGauge, 0.0f, 100.0f);
  if (gaugeText == nullptr) {
    return;
  }

  const Color accent = gaugeAccentColor(currentGaugeType, currentGaugeValue);
  gaugeText->setText(formatGaugeBarLabel(currentGaugeType,
                                         currentGaugeAutoShift,
                                         currentGaugeValue));
  gaugeText->setColor(ui_theme::sdl(gaugeTextColor(accent)));
}

void BMSRenderer::setPlayOptionStatus(const std::string &label) {
  if (playOptionText == nullptr) {
    return;
  }

  playOptionText->setVisible(!label.empty());
  playOptionText->setText(label);
}

void BMSRenderer::setReplayData(const ReplayData *replayData) {
  replayGhostEvents.clear();
  replayMissMarkers.clear();
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
  double alpha;
  if (laneState.isPressed) {
    alpha = 0.2;
  } else {
    // fade out
    alpha = 0.2 - (time - laneState.lastStateTime) / 1000000.0 / 1.0;
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

void BMSRenderer::drawLaneCover() {
  const float coverHeight = upperBound - noteVisibleUpperBound;
  if (coverHeight <= 0.001f) {
    return;
  }

  drawRect(playAreaWidth, coverHeight, playAreaLeftX, noteVisibleUpperBound,
           Color(9, 12, 18, 255));

  const float edgeHeight = std::max(0.025f, noteRenderHeight * 0.12f);
  drawRect(playAreaWidth, edgeHeight, playAreaLeftX,
           noteVisibleUpperBound - edgeHeight * 0.5f,
           Color(214, 224, 236, 255));
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

rendering::TexBatchRenderer &BMSRenderer::noteTextureBatch(
    bgfx::TextureHandle texture, uint32_t submitDepth) {
  const uint64_t key = noteTextureBatchKey(texture, submitDepth);
  if (const auto it = noteTextureBatchLookup.find(key);
      it != noteTextureBatchLookup.end()) {
    return noteTextureBatchRenderers[it->second];
  }

  const size_t index = noteTextureBatchRenderers.size();
  auto &renderer = noteTextureBatchRenderers.emplace_back();
  renderer.setSubmitDepth(submitDepth);
  renderer.begin();
  noteTextureBatchLookup.emplace(key, index);
  return renderer;
}

rendering::TexBatchRenderer &BMSRenderer::sheetBatchFor(
    const NoteSheet &sheet) {
  return noteTextureBatch(sheet.texture, noteSheetSubmitDepth);
}

rendering::TexBatchRenderer &BMSRenderer::longBodyBatchFor(
    const NoteSheet &sheet, bool isHolding) {
  return noteTextureBatch(isHolding ? sheet.longBodyOnTexture
                                    : sheet.longBodyOffTexture,
                          longBodySubmitDepth);
}

void BMSRenderer::beginNoteTextureBatches(uint32_t bodyDepth,
                                          uint32_t sheetDepth) {
  longBodySubmitDepth = bodyDepth;
  noteSheetSubmitDepth = sheetDepth;
  for (auto &renderer : noteTextureBatchRenderers) {
    renderer.begin();
  }
}

void BMSRenderer::flushNoteTextureBatches() {
  for (auto &renderer : noteTextureBatchRenderers) {
    renderer.flush();
  }
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
