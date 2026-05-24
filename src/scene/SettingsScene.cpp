#include "SettingsScene.h"
#include "../context.h"
#include "../rendering/Color.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct LayoutMetrics {
  SafeAreaInsets safe;
  bool compact = false;
  bool ultraCompact = false;
  bool stackBody = false;
  bool useDualCardRow = true;
  int contentWidth = 0;
  int horizontalPadding = 52;
  int verticalPadding = 60;
  int rootGap = 28;
  int headerGap = 10;
  int bodyGap = 28;
  int secondaryGap = 22;
  int summaryWidth = 400;
  int cardsWidth = 0;
  int secondaryCardWidth = 0;
  int titleSize = 72;
  int subtitleSize = 26;
  int sectionTitleSize = 34;
  int bodyTextSize = 22;
  int summaryValueSize = 24;
  int smallTextSize = 20;
  int cardPadding = 28;
  int cardGap = 22;
  int offsetButtonWidthLarge = 110;
  int offsetButtonWidthSmall = 96;
  int offsetValueWidth = 270;
  int resetButtonWidth = 140;
  int actionButtonWidth = 290;
  int actionButtonHeight = 72;
  int backButtonWidth = 180;
  int backButtonHeight = 64;
  int offsetCardHeight = 190;
  int visibleTimeCardHeight = 250;
  int modeCardHeight = 180;
  int lanePreviewHeight = 340;
};

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  insets.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  insets.bottom = static_cast<int>(std::lround(
      normalized.bottom * static_cast<float>(rendering::window_height)));
  insets.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
#endif
  return insets;
}

LayoutMetrics resolveLayoutMetrics() {
  LayoutMetrics metrics;
  metrics.safe = getSafeAreaInsetsUi();
  metrics.compact = rendering::window_height < 980;
  metrics.ultraCompact = rendering::window_height < 860;

  if (metrics.ultraCompact) {
    metrics.horizontalPadding = 26;
    metrics.verticalPadding = 18;
    metrics.rootGap = 16;
    metrics.headerGap = 6;
    metrics.bodyGap = 16;
    metrics.secondaryGap = 16;
    metrics.summaryWidth = 320;
    metrics.titleSize = 52;
    metrics.subtitleSize = 18;
    metrics.sectionTitleSize = 27;
    metrics.bodyTextSize = 18;
    metrics.summaryValueSize = 20;
    metrics.smallTextSize = 18;
    metrics.cardPadding = 18;
    metrics.cardGap = 14;
    metrics.offsetButtonWidthLarge = 92;
    metrics.offsetButtonWidthSmall = 82;
    metrics.offsetValueWidth = 208;
    metrics.resetButtonWidth = 112;
    metrics.actionButtonWidth = 240;
    metrics.actionButtonHeight = 58;
    metrics.backButtonWidth = 146;
    metrics.backButtonHeight = 56;
    metrics.offsetCardHeight = 148;
    metrics.visibleTimeCardHeight = 208;
    metrics.modeCardHeight = 148;
    metrics.lanePreviewHeight = 260;
  } else if (metrics.compact) {
    metrics.horizontalPadding = 32;
    metrics.verticalPadding = 24;
    metrics.rootGap = 20;
    metrics.headerGap = 8;
    metrics.bodyGap = 20;
    metrics.secondaryGap = 18;
    metrics.summaryWidth = 340;
    metrics.titleSize = 58;
    metrics.subtitleSize = 20;
    metrics.sectionTitleSize = 30;
    metrics.bodyTextSize = 20;
    metrics.summaryValueSize = 22;
    metrics.smallTextSize = 18;
    metrics.cardPadding = 22;
    metrics.cardGap = 16;
    metrics.offsetButtonWidthLarge = 96;
    metrics.offsetButtonWidthSmall = 84;
    metrics.offsetValueWidth = 220;
    metrics.resetButtonWidth = 116;
    metrics.actionButtonWidth = 250;
    metrics.actionButtonHeight = 62;
    metrics.backButtonWidth = 156;
    metrics.backButtonHeight = 58;
    metrics.offsetCardHeight = 156;
    metrics.visibleTimeCardHeight = 224;
    metrics.modeCardHeight = 156;
    metrics.lanePreviewHeight = 290;
  }

  const int availableWidth =
      std::max(0, rendering::window_width - metrics.safe.left -
                      metrics.safe.right - metrics.horizontalPadding * 2);
  metrics.contentWidth = availableWidth;
  metrics.stackBody = metrics.compact || availableWidth < 1500;
  if (metrics.stackBody) {
    metrics.summaryWidth = availableWidth;
    metrics.cardsWidth = availableWidth;
  } else {
    metrics.cardsWidth =
        std::max(0, availableWidth - metrics.summaryWidth - metrics.bodyGap);
  }
  metrics.useDualCardRow =
      !metrics.compact && !metrics.stackBody && metrics.cardsWidth >= 980;
  metrics.secondaryCardWidth =
      metrics.useDualCardRow
          ? std::max(0, (metrics.cardsWidth - metrics.secondaryGap) / 2)
          : metrics.cardsWidth;

  return metrics;
}

TextView *makeText(const std::string &text, int size, const Color &color,
                   TextView::TextAlign align = TextView::LEFT,
                   TextView::TextVAlign valign = TextView::TOP) {
  auto *view = new TextView(kFontPath, size);
  view->setText(text);
  view->setColor({color.r, color.g, color.b, color.a});
  view->setAlign(align);
  view->setVAlign(valign);
  return view;
}

TextView *makeWrappedText(const std::string &text, int size, const Color &color,
                          TextView::TextAlign align = TextView::LEFT,
                          TextView::TextVAlign valign = TextView::TOP) {
  auto *view = makeText(text, size, color, align, valign);
  view->setWrap(true);
  return view;
}

Button *makeButton(int width, int height, TextView *label,
                   const Color &normalBackground, const Color &hoverBackground,
                   const Color &pressedBackground, const Color &normalBorder,
                   const Color &hoverBorder, const Color &pressedBorder,
                   int borderWidth = 2) {
  auto *button = new Button(0, 0, width, height);
  label->setAlign(TextView::CENTER);
  label->setVAlign(TextView::MIDDLE);
  button->setContentView(label);
  button->setBackgroundColors(normalBackground, hoverBackground,
                              pressedBackground);
  button->setBorderColors(normalBorder, hoverBorder, pressedBorder);
  button->setStyledBorderWidth(borderWidth);
  return button;
}

Button *makeStepButton(const LayoutMetrics &metrics, int width,
                       const std::string &label) {
  return makeButton(width, metrics.actionButtonHeight,
                    makeText(label, metrics.bodyTextSize + 4,
                             Color(239, 244, 251), TextView::CENTER,
                             TextView::MIDDLE),
                    Color(28, 40, 58, 255), Color(36, 52, 75, 255),
                    Color(61, 87, 118, 255), Color(84, 107, 139, 255),
                    Color(108, 136, 174, 255), Color(139, 172, 217, 255));
}

Button *makeResetButton(const LayoutMetrics &metrics) {
  return makeButton(metrics.resetButtonWidth, metrics.actionButtonHeight,
                    makeText("Reset", metrics.bodyTextSize + 4,
                             Color(248, 241, 236), TextView::CENTER,
                             TextView::MIDDLE),
                    Color(96, 57, 44, 255), Color(117, 72, 55, 255),
                    Color(153, 96, 74, 255), Color(165, 105, 79, 255),
                    Color(193, 124, 93, 255), Color(219, 145, 108, 255));
}

TextInputBox *makeNumericInput(const LayoutMetrics &metrics) {
  auto *input = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
  input->setText("");
  input->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
  input->setBackgroundColor(Color(0, 0, 0, 0));
  input->setBorderWidth(0);
  input->setAlign(TextView::CENTER);
  input->setVAlign(TextView::MIDDLE);
  input->setColor({244, 248, 255, 255});
  return input;
}

View *makeInputFrame(const LayoutMetrics &metrics, TextInputBox *input) {
  auto *value = new View();
  value->setWidth(static_cast<float>(metrics.offsetValueWidth));
  value->setHeight(static_cast<float>(metrics.actionButtonHeight));
  value->setBackgroundColor(Color(10, 17, 28, 255));
  value->setBorderColor(Color(78, 105, 140, 255));
  value->setBorderWidth(2);
  value->addView(input);
  return value;
}

View *makeCard(const LayoutMetrics &metrics, const std::string &title,
               const std::string &description, View *body, int minHeight,
               int width = 0) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column);
  card->setGap(metrics.cardGap);
  card->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  card->setBackgroundColor(Color(19, 30, 46, 245));
  card->setBorderColor(Color(76, 104, 136, 255));
  card->setBorderWidth(2);
  card->setMinHeight(static_cast<float>(minHeight));
  if (width > 0) {
    card->setWidth(static_cast<float>(width));
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Column);
  header->setGap(metrics.compact ? 6.0f : 8.0f);
  auto *titleText =
      makeWrappedText(title, metrics.sectionTitleSize, Color(244, 248, 255));
  header->addView(titleText);
  auto *descriptionText =
      makeWrappedText(description, metrics.bodyTextSize, Color(168, 186, 209));
  header->addView(descriptionText);
  card->addView(header);
  card->addView(body);
  return card;
}

View *makeSummaryRow(const LayoutMetrics &metrics, const std::string &label,
                     TextView **valueOut) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setJustifyContent(YGJustifySpaceBetween);
  row->setAlignItems(YGAlignCenter);

  row->addView(makeText(label, metrics.summaryValueSize, Color(164, 186, 206)));
  auto *valueText = makeText("", metrics.summaryValueSize, Color(244, 248, 255),
                             TextView::RIGHT);
  row->addView(valueText);
  if (valueOut != nullptr) {
    *valueOut = valueText;
  }
  return row;
}

int clampOffset(int value) {
  return std::clamp(value, AppSettings::kMinInputOffsetMs,
                    AppSettings::kMaxInputOffsetMs);
}

int clampVisualOffset(int value) {
  return std::clamp(value, AppSettings::kMinVisualOffsetMs,
                    AppSettings::kMaxVisualOffsetMs);
}

int clampVisibleTimeGreenNumber(int value) {
  return std::clamp(value, AppSettings::kMinVisibleTimeGreenNumber,
                    AppSettings::kMaxVisibleTimeGreenNumber);
}

int clampBgaBrightness(int value) {
  return std::clamp(value, AppSettings::kMinBgaBrightnessPercent,
                    AppSettings::kMaxBgaBrightnessPercent);
}

float clampBgaBlur(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultBgaBlurStrength;
  }
  return std::clamp(value, AppSettings::kMinBgaBlurStrength,
                    AppSettings::kMaxBgaBlurStrength);
}

float clampLaneAngle(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneAngleDegrees;
  }
  return std::clamp(value, AppSettings::kMinLaneAngleDegrees,
                    AppSettings::kMaxLaneAngleDegrees);
}

float clampLaneLength(float value) {
  if (!std::isfinite(value)) {
    return AppSettings::kDefaultLaneLength;
  }
  return std::clamp(value, AppSettings::kMinLaneLength,
                    AppSettings::kMaxLaneLength);
}

int greenNumberToMilliseconds(int greenNumber) {
  return static_cast<int>(
      std::lround(static_cast<double>(greenNumber) * 1000.0 / 600.0));
}

int millisecondsToGreenNumber(int milliseconds) {
  return static_cast<int>(
      std::lround(static_cast<double>(milliseconds) * 600.0 / 1000.0));
}

int adjustVisibleTimeGreenNumber(int currentGreenNumber, bool useMilliseconds,
                                 int delta) {
  if (!useMilliseconds) {
    return clampVisibleTimeGreenNumber(currentGreenNumber + delta);
  }

  const int currentMilliseconds = greenNumberToMilliseconds(currentGreenNumber);
  const int nextMilliseconds =
      std::clamp(currentMilliseconds + delta, AppSettings::kMinVisibleTimeMs,
                 AppSettings::kMaxVisibleTimeMs);
  return clampVisibleTimeGreenNumber(
      millisecondsToGreenNumber(nextMilliseconds));
}

std::string formatOffsetLabel(int offsetMs) {
  return (offsetMs > 0 ? "+" : "") + std::to_string(offsetMs) + " ms";
}

std::string formatOffsetInputValue(int offsetMs) {
  return std::to_string(offsetMs);
}

std::string formatVisibleTimeLabel(int greenNumber, bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber)) + " ms";
  }
  return std::to_string(greenNumber) + " green";
}

std::string formatVisibleTimeInputValue(int greenNumber, bool useMilliseconds) {
  if (useMilliseconds) {
    return std::to_string(greenNumberToMilliseconds(greenNumber));
  }
  return std::to_string(greenNumber);
}

std::string formatFloatValue(float value, int precision = 1) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

std::string formatBgaBrightnessLabel(int percent) {
  return std::to_string(percent) + "%";
}

std::string formatBgaBlurLabel(float strength) {
  return formatFloatValue(strength, 1);
}

std::string formatLaneAngleLabel(float degrees) {
  return formatFloatValue(degrees, 1) + " deg";
}

std::string formatLaneLengthLabel(float length) {
  return formatFloatValue(length, 1);
}

std::string formatBgaDisplayModeLabel(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return "Fit";
  case AppSettings::BgaDisplayMode::Fill:
    return "Fill";
  case AppSettings::BgaDisplayMode::Stretch:
    return "Stretch";
  }
  return "Fit";
}

AppSettings::BgaDisplayMode
nextBgaDisplayMode(AppSettings::BgaDisplayMode mode) {
  switch (mode) {
  case AppSettings::BgaDisplayMode::Fit:
    return AppSettings::BgaDisplayMode::Fill;
  case AppSettings::BgaDisplayMode::Fill:
    return AppSettings::BgaDisplayMode::Stretch;
  case AppSettings::BgaDisplayMode::Stretch:
    return AppSettings::BgaDisplayMode::Fit;
  }
  return AppSettings::BgaDisplayMode::Fit;
}

class LanePreviewView : public View {
public:
  explicit LanePreviewView(ApplicationContext &context) : context(context) {}

private:
  ApplicationContext &context;

  void submitQuad(const RenderContext &renderContext, const Color &color,
                  float x0, float y0, float x1, float y1, float x2, float y2,
                  float x3, float y3) {
    bgfx::TransientVertexBuffer tvb{};
    bgfx::TransientIndexBuffer tib{};
    if (bgfx::getAvailTransientVertexBuffer(
            4, rendering::PosColorVertex::ms_decl) < 4 ||
        bgfx::getAvailTransientIndexBuffer(6) < 6) {
      return;
    }
    bgfx::allocTransientVertexBuffer(&tvb, 4,
                                     rendering::PosColorVertex::ms_decl);
    bgfx::allocTransientIndexBuffer(&tib, 6);
    auto *vertices = reinterpret_cast<rendering::PosColorVertex *>(tvb.data);
    const uint32_t abgr = color.toABGR();
    vertices[0] = {x0, y0, 0.0f, abgr};
    vertices[1] = {x1, y1, 0.0f, abgr};
    vertices[2] = {x2, y2, 0.0f, abgr};
    vertices[3] = {x3, y3, 0.0f, abgr};

    auto *indices = reinterpret_cast<uint16_t *>(tib.data);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    indices[3] = 2;
    indices[4] = 3;
    indices[5] = 0;

    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setIndexBuffer(&tib);
    rendering::setScissorUI(renderContext.scissor.x, renderContext.scissor.y,
                            renderContext.scissor.width,
                            renderContext.scissor.height);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_BLEND_ALPHA);
    static const bgfx::ProgramHandle kProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_SIMPLE);
    bgfx::submit(rendering::ui_view, kProgram);
  }

  void renderImpl(RenderContext &renderContext) override {
    const float x = static_cast<float>(getX());
    const float y = static_cast<float>(getY());
    const float width = static_cast<float>(getWidth());
    const float height = static_cast<float>(getHeight());
    if (width <= 0.0f || height <= 0.0f) {
      return;
    }

    ScissorScope scissor(renderContext, getX(), getY(), getWidth(),
                         getHeight());

    submitQuad(renderContext, Color(10, 16, 26, 255), x, y, x + width, y,
               x + width, y + height, x, y + height);

    const float angleNorm =
        (context.settings.laneAngleDegrees -
         AppSettings::kMinLaneAngleDegrees) /
        (AppSettings::kMaxLaneAngleDegrees - AppSettings::kMinLaneAngleDegrees);
    const float lengthNorm =
        (context.settings.laneLength - AppSettings::kMinLaneLength) /
        (AppSettings::kMaxLaneLength - AppSettings::kMinLaneLength);
    const float bottomY = y + height - 22.0f;
    const float topY = y + height * (0.12f + (1.0f - lengthNorm) * 0.14f);
    const float bottomHalf = width * 0.42f;
    const float topHalf = width * (0.16f + angleNorm * 0.13f);
    const float centerX = x + width * 0.5f;
    const float leftBottom = centerX - bottomHalf;
    const float rightBottom = centerX + bottomHalf;
    const float leftTop = centerX - topHalf;
    const float rightTop = centerX + topHalf;

    submitQuad(renderContext, Color(22, 33, 48, 255), leftTop, topY, rightTop,
               topY, rightBottom, bottomY, leftBottom, bottomY);

    constexpr int kLaneCount = 8;
    for (int i = 0; i < kLaneCount; ++i) {
      const float t0 = static_cast<float>(i) / static_cast<float>(kLaneCount);
      const float t1 =
          static_cast<float>(i + 1) / static_cast<float>(kLaneCount);
      const float laneLeftTop = leftTop + (rightTop - leftTop) * t0;
      const float laneRightTop = leftTop + (rightTop - leftTop) * t1;
      const float laneLeftBottom = leftBottom + (rightBottom - leftBottom) * t0;
      const float laneRightBottom =
          leftBottom + (rightBottom - leftBottom) * t1;
      const Color laneColor = i == 0 ? Color(107, 70, 31, 210)
                                     : (i % 2 == 0 ? Color(59, 73, 92, 210)
                                                   : Color(37, 85, 130, 210));
      submitQuad(renderContext, laneColor, laneLeftTop + 1.0f, topY,
                 laneRightTop - 1.0f, topY, laneRightBottom - 2.0f, bottomY,
                 laneLeftBottom + 2.0f, bottomY);
    }

    const float judgeHeight = 7.0f;
    submitQuad(renderContext, Color(239, 246, 255, 245), leftBottom,
               bottomY - judgeHeight, rightBottom, bottomY - judgeHeight,
               rightBottom, bottomY, leftBottom, bottomY);

    for (int i = 1; i < 4; ++i) {
      const float depth = static_cast<float>(i) / 4.0f;
      const float lineY = bottomY + (topY - bottomY) * depth;
      const float half = bottomHalf + (topHalf - bottomHalf) * depth;
      submitQuad(renderContext, Color(214, 224, 238, 72), centerX - half, lineY,
                 centerX + half, lineY, centerX + half, lineY + 2.0f,
                 centerX - half, lineY + 2.0f);
    }
  }
};
} // namespace

void SettingsScene::init() { ensureLayoutUpToDate(); }

void SettingsScene::resetViewState() {
  for (auto *view : views) {
    delete view;
  }
  views.clear();
  rootLayout = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
}

void SettingsScene::ensureLayoutUpToDate() {
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  if (rendering::window_width == lastLayoutWidth &&
      rendering::window_height == lastLayoutHeight && safe.top == lastSafeTop &&
      safe.left == lastSafeLeft && safe.bottom == lastSafeBottom &&
      safe.right == lastSafeRight && rootLayout != nullptr) {
    return;
  }

  resetViewState();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  initView();
}

void SettingsScene::initView() {
  const LayoutMetrics metrics = resolveLayoutMetrics();
  View::LayoutBatchScope layoutBatch;

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout->setFlexDirection(FlexDirection::Column);
  rootLayout->setPadding(
      Edge::Top,
      static_cast<float>(metrics.safe.top + metrics.verticalPadding));
  rootLayout->setPadding(
      Edge::Left,
      static_cast<float>(metrics.safe.left + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Right,
      static_cast<float>(metrics.safe.right + metrics.horizontalPadding));
  rootLayout->setPadding(
      Edge::Bottom,
      static_cast<float>(metrics.safe.bottom + metrics.verticalPadding));
  rootLayout->setGap(static_cast<float>(metrics.rootGap));
  rootLayout->setBackgroundColor(Color(10, 18, 30));

  if (!metrics.compact) {
    auto *accentA = new View(110, 86, 480, 180);
    accentA->setPositionType(YGPositionTypeAbsolute);
    accentA->setBackgroundColor(Color(39, 101, 160, 96));
    rootLayout->addView(accentA);

    auto *accentB = new View(rendering::window_width - 520,
                             rendering::window_height - 250, 420, 160);
    accentB->setPositionType(YGPositionTypeAbsolute);
    accentB->setBackgroundColor(Color(207, 110, 62, 72));
    rootLayout->addView(accentB);
  }

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row);
  header->setAlignItems(YGAlignCenter);
  header->setJustifyContent(YGJustifySpaceBetween);

  auto *headerText = new View();
  headerText->setFlexDirection(FlexDirection::Column);
  headerText->setGap(static_cast<float>(metrics.headerGap));
  headerText->addView(
      makeText("Settings", metrics.titleSize, Color(244, 248, 255)));
  headerText->addView(makeWrappedText(
      metrics.compact
          ? "Timing, keysound, and visual preferences."
          : "Persistent player preferences for timing, keysounds, and visual "
            "load.",
      metrics.subtitleSize, Color(162, 183, 205)));
  header->addView(headerText);

  auto *backLabel =
      makeText("Back", metrics.bodyTextSize + 6, Color(237, 243, 252),
               TextView::CENTER, TextView::MIDDLE);
  auto *backButton =
      makeButton(metrics.backButtonWidth, metrics.backButtonHeight, backLabel,
                 Color(22, 33, 49, 255), Color(31, 46, 67, 255),
                 Color(53, 78, 110, 255), Color(96, 121, 156, 255),
                 Color(120, 151, 190, 255), Color(148, 186, 231, 255));
  backButton->setOnClickListener(
      [this]() { context.sceneManager->changeScene("MainMenu"); });
  header->addView(backButton);
  rootLayout->addView(header);

  scrollView = new ScrollView();
  scrollView->setFlex(1.0f);

  auto *scrollContent = new View();
  scrollContent->setFlexDirection(FlexDirection::Column);
  scrollContent->setGap(static_cast<float>(metrics.rootGap));

  auto *body = new View();
  body->setFlexDirection(metrics.stackBody ? FlexDirection::Column
                                           : FlexDirection::Row);
  body->setGap(static_cast<float>(metrics.bodyGap));
  body->setAlignItems(YGAlignFlexStart);

  auto *summaryCard = new View();
  summaryCard->setWidth(static_cast<float>(
      metrics.stackBody ? metrics.contentWidth : metrics.summaryWidth));
  summaryCard->setPadding(Edge::All, static_cast<float>(metrics.cardPadding));
  summaryCard->setGap(metrics.compact ? 12.0f : 18.0f);
  summaryCard->setFlexDirection(FlexDirection::Column);
  summaryCard->setBackgroundColor(Color(17, 27, 42, 245));
  summaryCard->setBorderColor(Color(68, 94, 123, 255));
  summaryCard->setBorderWidth(2);
  summaryCard->addView(makeText("Profile Snapshot", metrics.sectionTitleSize,
                                Color(244, 248, 255)));
  summaryCard->addView(
      makeWrappedText(metrics.compact ? "Saved immediately."
                                      : "Saved immediately for new charts.",
                      metrics.bodyTextSize, Color(160, 181, 204)));
  summaryCard->addView(
      makeSummaryRow(metrics, "Judgement Offset", &summaryOffsetValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "Visual Offset", &summaryVisualOffsetValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "Visible Time", &summaryVisibleTimeValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "Input Keysounds", &summaryKeysoundValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "BGA Playback", &summaryBgaValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "BGA Display", &summaryBgaDisplayValueText));
  summaryCard->addView(makeSummaryRow(metrics, "BGA Brightness",
                                      &summaryBgaBrightnessValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "BGA Blur", &summaryBgaBlurValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "Lane Angle", &summaryLaneAngleValueText));
  summaryCard->addView(
      makeSummaryRow(metrics, "Lane Length", &summaryLaneLengthValueText));
  if (!metrics.ultraCompact) {
    summaryCard->addView(makeWrappedText(
        metrics.compact
            ? "Judgement offset shifts timing windows. Visual offset delays "
              "notes and BGA for late audio paths."
            : "Judgement offset shifts timing windows. Visual offset delays "
              "notes and BGA, while visible time controls how long notes stay "
              "on screen.",
        metrics.smallTextSize, Color(131, 151, 176)));
  }
  body->addView(summaryCard);

  auto *cardsColumn = new View();
  cardsColumn->setFlexDirection(FlexDirection::Column);
  cardsColumn->setGap(static_cast<float>(metrics.secondaryGap));
  cardsColumn->setWidth(static_cast<float>(metrics.cardsWidth));

  auto *tabControls = new View();
  tabControls->setFlexDirection(FlexDirection::Row);
  tabControls->setFlexWrap(YGWrapWrap);
  tabControls->setGap(metrics.compact ? 8.0f : 12.0f);
  auto makeTabButton = [&](SettingsTab tab, const std::string &label) {
    auto *button = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        makeText(label, metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    button->setOnClickListener([this, tab]() {
      if (activeTab == tab) {
        return;
      }
      activeTab = tab;
      lastLayoutWidth = -1;
    });
    return button;
  };
  timingTabButton = makeTabButton(SettingsTab::Timing, "Timing");
  visualTabButton = makeTabButton(SettingsTab::Visual, "Visual");
  laneTabButton = makeTabButton(SettingsTab::Lane, "Lane");
  tabControls->addView(timingTabButton);
  tabControls->addView(visualTabButton);
  tabControls->addView(laneTabButton);
  cardsColumn->addView(tabControls);

  if (activeTab == SettingsTab::Timing) {
    auto *offsetControls = new View();
    offsetControls->setFlexDirection(FlexDirection::Row);
    offsetControls->setFlexWrap(YGWrapWrap);
    offsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
    offsetControls->setAlignItems(YGAlignFlexStart);

    auto updateOffset = [this](int delta) {
      context.settings.inputOffsetMs =
          clampOffset(context.settings.inputOffsetMs + delta);
      persistSettings();
      syncOffsetInputText(true);
    };

    auto *minusTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusTen->setOnClickListener([updateOffset]() { updateOffset(-10); });
    offsetControls->addView(minusTen);

    auto *minusOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusOne->setOnClickListener([updateOffset]() { updateOffset(-1); });
    offsetControls->addView(minusOne);

    auto *offsetValue = new View();
    offsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
    offsetValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
    offsetValue->setBackgroundColor(Color(10, 17, 28, 255));
    offsetValue->setBorderColor(Color(78, 105, 140, 255));
    offsetValue->setBorderWidth(2);
    offsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
    offsetInput->setText("");
    offsetInput->setSize(metrics.offsetValueWidth, metrics.actionButtonHeight);
    offsetInput->setBackgroundColor(Color(0, 0, 0, 0));
    offsetInput->setBorderWidth(0);
    offsetInput->setAlign(TextView::CENTER);
    offsetInput->setVAlign(TextView::MIDDLE);
    offsetInput->setColor({244, 248, 255, 255});
    offsetInput->onEditingFinished(
        [this](const std::string &) { commitOffsetInput(); });
    offsetValue->addView(offsetInput);
    offsetControls->addView(offsetValue);

    auto *plusOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusOne->setOnClickListener([updateOffset]() { updateOffset(1); });
    offsetControls->addView(plusOne);

    auto *plusTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusTen->setOnClickListener([updateOffset]() { updateOffset(10); });
    offsetControls->addView(plusTen);

    auto *resetOffset = makeButton(
        metrics.resetButtonWidth, metrics.actionButtonHeight,
        makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
                 TextView::CENTER, TextView::MIDDLE),
        Color(96, 57, 44, 255), Color(117, 72, 55, 255),
        Color(153, 96, 74, 255), Color(165, 105, 79, 255),
        Color(193, 124, 93, 255), Color(219, 145, 108, 255));
    resetOffset->setOnClickListener([this]() {
      context.settings.inputOffsetMs = 0;
      persistSettings();
      syncOffsetInputText(true);
    });
    offsetControls->addView(resetOffset);

    cardsColumn->addView(
        makeCard(metrics, "Judgement Offset",
                 metrics.compact
                     ? "Positive values judge later when your hits feel early."
                     : "Positive values judge later. Use this when your hits "
                       "consistently "
                       "feel early relative to the music.",
                 offsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *visualOffsetControls = new View();
    visualOffsetControls->setFlexDirection(FlexDirection::Row);
    visualOffsetControls->setFlexWrap(YGWrapWrap);
    visualOffsetControls->setGap(metrics.compact ? 8.0f : 12.0f);
    visualOffsetControls->setAlignItems(YGAlignFlexStart);

    auto updateVisualOffset = [this](int delta) {
      context.settings.visualOffsetMs =
          clampVisualOffset(context.settings.visualOffsetMs + delta);
      persistSettings();
      syncVisualOffsetInputText(true);
    };

    auto *minusVisualTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisualTen->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(-10); });
    visualOffsetControls->addView(minusVisualTen);

    auto *minusVisualOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisualOne->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(-1); });
    visualOffsetControls->addView(minusVisualOne);

    auto *visualOffsetValue = new View();
    visualOffsetValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
    visualOffsetValue->setHeight(
        static_cast<float>(metrics.actionButtonHeight));
    visualOffsetValue->setBackgroundColor(Color(10, 17, 28, 255));
    visualOffsetValue->setBorderColor(Color(78, 105, 140, 255));
    visualOffsetValue->setBorderWidth(2);
    visualOffsetInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
    visualOffsetInput->setText("");
    visualOffsetInput->setSize(metrics.offsetValueWidth,
                               metrics.actionButtonHeight);
    visualOffsetInput->setBackgroundColor(Color(0, 0, 0, 0));
    visualOffsetInput->setBorderWidth(0);
    visualOffsetInput->setAlign(TextView::CENTER);
    visualOffsetInput->setVAlign(TextView::MIDDLE);
    visualOffsetInput->setColor({244, 248, 255, 255});
    visualOffsetInput->onEditingFinished(
        [this](const std::string &) { commitVisualOffsetInput(); });
    visualOffsetValue->addView(visualOffsetInput);
    visualOffsetControls->addView(visualOffsetValue);

    auto *plusVisualOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisualOne->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(1); });
    visualOffsetControls->addView(plusVisualOne);

    auto *plusVisualTen = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisualTen->setOnClickListener(
        [updateVisualOffset]() { updateVisualOffset(10); });
    visualOffsetControls->addView(plusVisualTen);

    auto *resetVisualOffset = makeButton(
        metrics.resetButtonWidth, metrics.actionButtonHeight,
        makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
                 TextView::CENTER, TextView::MIDDLE),
        Color(96, 57, 44, 255), Color(117, 72, 55, 255),
        Color(153, 96, 74, 255), Color(165, 105, 79, 255),
        Color(193, 124, 93, 255), Color(219, 145, 108, 255));
    resetVisualOffset->setOnClickListener([this]() {
      context.settings.visualOffsetMs = 0;
      persistSettings();
      syncVisualOffsetInputText(true);
    });
    visualOffsetControls->addView(resetVisualOffset);

    cardsColumn->addView(makeCard(
        metrics, "Visual Offset",
        metrics.compact
            ? "Positive values delay notes and BGA to match late audio output."
            : "Positive values delay note rendering and BGA playback. Use this "
              "for late audio paths such as Bluetooth headphones.",
        visualOffsetControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *visibleTimeControls = new View();
    visibleTimeControls->setFlexDirection(FlexDirection::Column);
    visibleTimeControls->setGap(metrics.compact ? 12.0f : 16.0f);
    visibleTimeControls->setAlignItems(YGAlignFlexStart);
    visibleTimeControls->addView(makeWrappedText(
        metrics.compact
            ? "600 green = 1000 ms. This controls how long notes stay visible."
            : "Green Number is the legacy BMS unit for note visible time. "
              "600 green equals 60 frames on a 60 FPS system, which is 1000 "
              "ms.",
        metrics.bodyTextSize, Color(150, 171, 193)));

    visibleTimeModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    visibleTimeModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        visibleTimeModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    visibleTimeModeButton->setOnClickListener([this]() {
      context.settings.visibleTimeUseMilliseconds =
          !context.settings.visibleTimeUseMilliseconds;
      persistSettings();
      syncVisibleTimeInputText(true);
    });
    visibleTimeControls->addView(visibleTimeModeButton);

    auto *visibleTimeValueControls = new View();
    visibleTimeValueControls->setFlexDirection(FlexDirection::Row);
    visibleTimeValueControls->setFlexWrap(YGWrapWrap);
    visibleTimeValueControls->setGap(metrics.compact ? 8.0f : 12.0f);
    visibleTimeValueControls->setAlignItems(YGAlignFlexStart);

    auto updateVisibleTime = [this](int delta) {
      context.settings.visibleTimeGreenNumber = adjustVisibleTimeGreenNumber(
          context.settings.visibleTimeGreenNumber,
          context.settings.visibleTimeUseMilliseconds, delta);
      persistSettings();
      syncVisibleTimeInputText(true);
    };

    auto *minusVisibleTimeLarge = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("-100", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisibleTimeLarge->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-100); });
    visibleTimeValueControls->addView(minusVisibleTimeLarge);

    auto *minusVisibleTimeOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisibleTimeOne->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-1); });
    visibleTimeValueControls->addView(minusVisibleTimeOne);

    auto *minusVisibleTimeSmall = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("-10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    minusVisibleTimeSmall->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(-10); });
    visibleTimeValueControls->addView(minusVisibleTimeSmall);

    auto *visibleTimeValue = new View();
    visibleTimeValue->setWidth(static_cast<float>(metrics.offsetValueWidth));
    visibleTimeValue->setHeight(static_cast<float>(metrics.actionButtonHeight));
    visibleTimeValue->setBackgroundColor(Color(10, 17, 28, 255));
    visibleTimeValue->setBorderColor(Color(78, 105, 140, 255));
    visibleTimeValue->setBorderWidth(2);
    visibleTimeInput = new TextInputBox(kFontPath, metrics.bodyTextSize + 6);
    visibleTimeInput->setText("");
    visibleTimeInput->setSize(metrics.offsetValueWidth,
                              metrics.actionButtonHeight);
    visibleTimeInput->setBackgroundColor(Color(0, 0, 0, 0));
    visibleTimeInput->setBorderWidth(0);
    visibleTimeInput->setAlign(TextView::CENTER);
    visibleTimeInput->setVAlign(TextView::MIDDLE);
    visibleTimeInput->setColor({244, 248, 255, 255});
    visibleTimeInput->onEditingFinished(
        [this](const std::string &) { commitVisibleTimeInput(); });
    visibleTimeValue->addView(visibleTimeInput);
    visibleTimeValueControls->addView(visibleTimeValue);

    auto *plusVisibleTimeOne = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+1", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisibleTimeOne->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(1); });
    visibleTimeValueControls->addView(plusVisibleTimeOne);

    auto *plusVisibleTimeSmall = makeButton(
        metrics.offsetButtonWidthSmall, metrics.actionButtonHeight,
        makeText("+10", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisibleTimeSmall->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(10); });
    visibleTimeValueControls->addView(plusVisibleTimeSmall);

    auto *plusVisibleTimeLarge = makeButton(
        metrics.offsetButtonWidthLarge, metrics.actionButtonHeight,
        makeText("+100", metrics.bodyTextSize + 4, Color(239, 244, 251),
                 TextView::CENTER, TextView::MIDDLE),
        Color(28, 40, 58, 255), Color(36, 52, 75, 255), Color(61, 87, 118, 255),
        Color(84, 107, 139, 255), Color(108, 136, 174, 255),
        Color(139, 172, 217, 255));
    plusVisibleTimeLarge->setOnClickListener(
        [updateVisibleTime]() { updateVisibleTime(100); });
    visibleTimeValueControls->addView(plusVisibleTimeLarge);

    auto *resetVisibleTime = makeButton(
        metrics.resetButtonWidth, metrics.actionButtonHeight,
        makeText("Reset", metrics.bodyTextSize + 4, Color(248, 241, 236),
                 TextView::CENTER, TextView::MIDDLE),
        Color(96, 57, 44, 255), Color(117, 72, 55, 255),
        Color(153, 96, 74, 255), Color(165, 105, 79, 255),
        Color(193, 124, 93, 255), Color(219, 145, 108, 255));
    resetVisibleTime->setOnClickListener([this]() {
      context.settings.visibleTimeGreenNumber = 400;
      persistSettings();
      syncVisibleTimeInputText(true);
    });
    visibleTimeValueControls->addView(resetVisibleTime);

    visibleTimeControls->addView(visibleTimeValueControls);
    cardsColumn->addView(makeCard(
        metrics, "Visible Time",
        metrics.compact
            ? "Controls how long notes stay on screen before the judgement "
              "line."
            : "Controls how long notes stay visible before reaching the "
              "judgement line. Switch units if you prefer legacy green number "
              "or direct milliseconds.",
        visibleTimeControls, metrics.visibleTimeCardHeight,
        metrics.cardsWidth));

    auto *secondaryCards = new View();
    secondaryCards->setFlexDirection(
        metrics.useDualCardRow ? FlexDirection::Row : FlexDirection::Column);
    secondaryCards->setGap(static_cast<float>(metrics.secondaryGap));

    auto *keysoundControls = new View();
    keysoundControls->setFlexDirection(FlexDirection::Column);
    keysoundControls->setGap(metrics.compact ? 12.0f : 16.0f);
    keysoundControls->setAlignItems(YGAlignFlexStart);
    keysoundControls->addView(makeWrappedText(
        metrics.compact
            ? "Switch between manual hits and chart-timed playback."
            : "Tap to switch modes. The current selection is shown on "
              "the right.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    keysoundModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    keysoundModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight, keysoundModeText,
        Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    keysoundModeButton->setOnClickListener([this]() {
      context.settings.inputKeysoundEnabled =
          !context.settings.inputKeysoundEnabled;
      persistSettings();
    });
    keysoundControls->addView(keysoundModeButton);
    secondaryCards->addView(makeCard(
        metrics, "Input Keysounds",
        metrics.compact
            ? "Manual hits keep classic feedback. Auto timed follows chart "
              "timing."
            : "Keep manual key clicks for classic BMS feedback, or switch to "
              "auto-timed playback for cleaner timing practice.",
        keysoundControls, metrics.modeCardHeight, metrics.secondaryCardWidth));

    cardsColumn->addView(secondaryCards);
  }

  if (activeTab == SettingsTab::Visual) {
    auto *bgaControls = new View();
    bgaControls->setFlexDirection(FlexDirection::Column);
    bgaControls->setGap(metrics.compact ? 12.0f : 16.0f);
    bgaControls->setAlignItems(YGAlignFlexStart);
    bgaControls->addView(makeWrappedText(
        metrics.compact ? "Toggle BGA rendering for previews and gameplay."
                        : "Tap to switch BGA rendering on or off for future "
                          "previews and charts.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    bgaModeText = makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                           TextView::CENTER, TextView::MIDDLE);
    bgaModeButton =
        makeButton(metrics.actionButtonWidth, metrics.actionButtonHeight,
                   bgaModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
                   Color(59, 98, 147, 255), Color(92, 131, 177, 255),
                   Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    bgaModeButton->setOnClickListener([this]() {
      context.settings.bgaEnabled = !context.settings.bgaEnabled;
      persistSettings();
    });
    bgaControls->addView(bgaModeButton);
    cardsColumn->addView(makeCard(
        metrics, "BGA Playback",
        metrics.compact
            ? "Disable background animation for lower distraction or lighter "
              "rendering."
            : "Disable background animation if you want lower distraction or a "
              "lighter render path on slower hardware.",
        bgaControls, metrics.modeCardHeight, metrics.cardsWidth));

    auto *bgaDisplayControls = new View();
    bgaDisplayControls->setFlexDirection(FlexDirection::Column);
    bgaDisplayControls->setGap(metrics.compact ? 12.0f : 16.0f);
    bgaDisplayControls->setAlignItems(YGAlignFlexStart);
    bgaDisplayControls->addView(makeWrappedText(
        metrics.compact
            ? "Fit preserves the full image. Fill crops. Stretch ignores "
              "aspect."
            : "Fit preserves the whole BGA with letterboxing. Fill preserves "
              "aspect and crops edges. Stretch fills the screen without "
              "preserving aspect.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    bgaDisplayModeText =
        makeText("", metrics.bodyTextSize + 6, Color(245, 248, 252),
                 TextView::CENTER, TextView::MIDDLE);
    bgaDisplayModeButton = makeButton(
        metrics.actionButtonWidth, metrics.actionButtonHeight,
        bgaDisplayModeText, Color(33, 56, 87, 255), Color(43, 72, 110, 255),
        Color(59, 98, 147, 255), Color(92, 131, 177, 255),
        Color(118, 163, 217, 255), Color(139, 189, 244, 255));
    bgaDisplayModeButton->setOnClickListener([this]() {
      context.settings.bgaDisplayMode =
          nextBgaDisplayMode(context.settings.bgaDisplayMode);
      persistSettings();
    });
    bgaDisplayControls->addView(bgaDisplayModeButton);
    cardsColumn->addView(makeCard(
        metrics, "BGA Aspect",
        metrics.compact ? "Choose how BGA fits the playfield."
                        : "Choose how BGA media is fitted to the playfield.",
        bgaDisplayControls, metrics.modeCardHeight, metrics.cardsWidth));

    auto *brightnessControls = new View();
    brightnessControls->setFlexDirection(FlexDirection::Row);
    brightnessControls->setFlexWrap(YGWrapWrap);
    brightnessControls->setGap(metrics.compact ? 8.0f : 12.0f);
    brightnessControls->setAlignItems(YGAlignFlexStart);
    auto updateBgaBrightness = [this](int delta) {
      context.settings.bgaBrightnessPercent =
          clampBgaBrightness(context.settings.bgaBrightnessPercent + delta);
      persistSettings();
      syncBgaBrightnessInputText(true);
    };
    auto *minusBrightnessTen =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-10");
    minusBrightnessTen->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(-10); });
    brightnessControls->addView(minusBrightnessTen);
    auto *minusBrightnessOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusBrightnessOne->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(-1); });
    brightnessControls->addView(minusBrightnessOne);
    bgaBrightnessInput = makeNumericInput(metrics);
    bgaBrightnessInput->onEditingFinished(
        [this](const std::string &) { commitBgaBrightnessInput(); });
    brightnessControls->addView(makeInputFrame(metrics, bgaBrightnessInput));
    auto *plusBrightnessOne =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusBrightnessOne->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(1); });
    brightnessControls->addView(plusBrightnessOne);
    auto *plusBrightnessTen =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+10");
    plusBrightnessTen->setOnClickListener(
        [updateBgaBrightness]() { updateBgaBrightness(10); });
    brightnessControls->addView(plusBrightnessTen);
    auto *resetBrightness = makeResetButton(metrics);
    resetBrightness->setOnClickListener([this]() {
      context.settings.bgaBrightnessPercent =
          AppSettings::kDefaultBgaBrightnessPercent;
      persistSettings();
      syncBgaBrightnessInputText(true);
    });
    brightnessControls->addView(resetBrightness);
    cardsColumn->addView(makeCard(
        metrics, "BGA Brightness",
        metrics.compact ? "Dim or restore the blurred BGA behind the lane."
                        : "Dim the BGA composite behind the lane when the "
                          "background competes with notes.",
        brightnessControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *blurControls = new View();
    blurControls->setFlexDirection(FlexDirection::Row);
    blurControls->setFlexWrap(YGWrapWrap);
    blurControls->setGap(metrics.compact ? 8.0f : 12.0f);
    blurControls->setAlignItems(YGAlignFlexStart);
    auto updateBgaBlur = [this](float delta) {
      context.settings.bgaBlurStrength =
          clampBgaBlur(context.settings.bgaBlurStrength + delta);
      persistSettings();
      syncBgaBlurInputText(true);
    };
    auto *minusBlurLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
    minusBlurLarge->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(-1.0f); });
    blurControls->addView(minusBlurLarge);
    auto *minusBlurSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusBlurSmall->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(-0.5f); });
    blurControls->addView(minusBlurSmall);
    bgaBlurInput = makeNumericInput(metrics);
    bgaBlurInput->onEditingFinished(
        [this](const std::string &) { commitBgaBlurInput(); });
    blurControls->addView(makeInputFrame(metrics, bgaBlurInput));
    auto *plusBlurSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusBlurSmall->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(0.5f); });
    blurControls->addView(plusBlurSmall);
    auto *plusBlurLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
    plusBlurLarge->setOnClickListener(
        [updateBgaBlur]() { updateBgaBlur(1.0f); });
    blurControls->addView(plusBlurLarge);
    auto *resetBlur = makeResetButton(metrics);
    resetBlur->setOnClickListener([this]() {
      context.settings.bgaBlurStrength = AppSettings::kDefaultBgaBlurStrength;
      persistSettings();
      syncBgaBlurInputText(true);
    });
    blurControls->addView(resetBlur);
    cardsColumn->addView(makeCard(
        metrics, "BGA Blur Strength",
        metrics.compact ? "Higher values soften background motion."
                        : "Higher values soften background motion before it is "
                          "composited behind the lane.",
        blurControls, metrics.offsetCardHeight, metrics.cardsWidth));
  }

  if (activeTab == SettingsTab::Lane) {
    auto *angleControls = new View();
    angleControls->setFlexDirection(FlexDirection::Row);
    angleControls->setFlexWrap(YGWrapWrap);
    angleControls->setGap(metrics.compact ? 8.0f : 12.0f);
    angleControls->setAlignItems(YGAlignFlexStart);
    auto updateLaneAngle = [this](float delta) {
      context.settings.laneAngleDegrees =
          clampLaneAngle(context.settings.laneAngleDegrees + delta);
      persistSettings();
      syncLaneAngleInputText(true);
    };
    auto *minusAngleLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-5");
    minusAngleLarge->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-5.0f); });
    angleControls->addView(minusAngleLarge);
    auto *minusAngleSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-1");
    minusAngleSmall->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(-1.0f); });
    angleControls->addView(minusAngleSmall);
    laneAngleInput = makeNumericInput(metrics);
    laneAngleInput->onEditingFinished(
        [this](const std::string &) { commitLaneAngleInput(); });
    angleControls->addView(makeInputFrame(metrics, laneAngleInput));
    auto *plusAngleSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+1");
    plusAngleSmall->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(1.0f); });
    angleControls->addView(plusAngleSmall);
    auto *plusAngleLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+5");
    plusAngleLarge->setOnClickListener(
        [updateLaneAngle]() { updateLaneAngle(5.0f); });
    angleControls->addView(plusAngleLarge);
    auto *resetAngle = makeResetButton(metrics);
    resetAngle->setOnClickListener([this]() {
      context.settings.laneAngleDegrees = AppSettings::kDefaultLaneAngleDegrees;
      persistSettings();
      syncLaneAngleInputText(true);
    });
    angleControls->addView(resetAngle);
    cardsColumn->addView(makeCard(
        metrics, "Lane Angle",
        metrics.compact ? "Adjust visual lane tilt and touch mapping together."
                        : "Adjust the gameplay camera pitch. Touch lane "
                          "conversion uses the same lane plane, so this stays "
                          "aligned for touch play.",
        angleControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *lengthControls = new View();
    lengthControls->setFlexDirection(FlexDirection::Row);
    lengthControls->setFlexWrap(YGWrapWrap);
    lengthControls->setGap(metrics.compact ? 8.0f : 12.0f);
    lengthControls->setAlignItems(YGAlignFlexStart);
    auto updateLaneLength = [this](float delta) {
      context.settings.laneLength =
          clampLaneLength(context.settings.laneLength + delta);
      persistSettings();
      syncLaneLengthInputText(true);
    };
    auto *minusLengthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "-1");
    minusLengthLarge->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-1.0f); });
    lengthControls->addView(minusLengthLarge);
    auto *minusLengthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "-0.5");
    minusLengthSmall->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(-0.5f); });
    lengthControls->addView(minusLengthSmall);
    laneLengthInput = makeNumericInput(metrics);
    laneLengthInput->onEditingFinished(
        [this](const std::string &) { commitLaneLengthInput(); });
    lengthControls->addView(makeInputFrame(metrics, laneLengthInput));
    auto *plusLengthSmall =
        makeStepButton(metrics, metrics.offsetButtonWidthSmall, "+0.5");
    plusLengthSmall->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(0.5f); });
    lengthControls->addView(plusLengthSmall);
    auto *plusLengthLarge =
        makeStepButton(metrics, metrics.offsetButtonWidthLarge, "+1");
    plusLengthLarge->setOnClickListener(
        [updateLaneLength]() { updateLaneLength(1.0f); });
    lengthControls->addView(plusLengthLarge);
    auto *resetLength = makeResetButton(metrics);
    resetLength->setOnClickListener([this]() {
      context.settings.laneLength = AppSettings::kDefaultLaneLength;
      persistSettings();
      syncLaneLengthInputText(true);
    });
    lengthControls->addView(resetLength);
    cardsColumn->addView(makeCard(
        metrics, "Lane Length",
        metrics.compact ? "Adjust how far the visible lane reaches."
                        : "Adjust how far the visible lane reaches toward the "
                          "top of the screen.",
        lengthControls, metrics.offsetCardHeight, metrics.cardsWidth));

    auto *previewBody = new View();
    previewBody->setFlexDirection(FlexDirection::Column);
    previewBody->setGap(metrics.compact ? 10.0f : 14.0f);
    previewBody->addView(makeWrappedText(
        metrics.compact
            ? "Preview updates with angle and length."
            : "This preview uses the same angle and length values as "
              "gameplay camera setup.",
        metrics.bodyTextSize, Color(150, 171, 193)));
    auto *preview = new LanePreviewView(context);
    preview->setHeight(static_cast<float>(metrics.lanePreviewHeight));
    preview->setBackgroundColor(Color(10, 16, 26, 255));
    preview->setBorderColor(Color(64, 89, 118, 255));
    preview->setBorderWidth(2);
    previewBody->addView(preview);
    cardsColumn->addView(makeCard(
        metrics, "Preview Lane",
        metrics.compact
            ? "Visual check for lane geometry."
            : "Visual check for the lane geometry before entering a "
              "chart.",
        previewBody, metrics.lanePreviewHeight + metrics.modeCardHeight,
        metrics.cardsWidth));
  }
  body->addView(cardsColumn);
  scrollContent->addView(body);

  auto *footer = new View();
  footer->setPadding(Edge::All, static_cast<float>(metrics.cardPadding - 4));
  footer->setBackgroundColor(Color(14, 22, 34, 220));
  footer->setBorderColor(Color(59, 80, 108, 255));
  footer->setBorderWidth(2);
  footer->addView(makeWrappedText(
      metrics.compact
          ? "Settings save automatically in the app documents directory."
          : "Settings are saved automatically in the app documents directory.",
      metrics.bodyTextSize, Color(165, 185, 205)));
  scrollContent->addView(footer);

  scrollView->setContentView(scrollContent);
  rootLayout->addView(scrollView);

  addView(rootLayout);
  rootLayout->applyYogaLayout();
  refreshSettingsText();
}

void SettingsScene::refreshSettingsText() {
  const int offsetMs = context.settings.inputOffsetMs;
  const int visualOffsetMs = context.settings.visualOffsetMs;
  const int visibleTimeGreenNumber = context.settings.visibleTimeGreenNumber;
  const std::string offsetLabel = formatOffsetLabel(offsetMs);
  const std::string visualOffsetLabel = formatOffsetLabel(visualOffsetMs);
  const std::string visibleTimeLabel = formatVisibleTimeLabel(
      visibleTimeGreenNumber, context.settings.visibleTimeUseMilliseconds);
  const std::string keysoundLabel =
      context.settings.inputKeysoundEnabled ? "Input Trigger" : "Auto Timed";
  const std::string bgaLabel =
      context.settings.bgaEnabled ? "Enabled" : "Disabled";
  const std::string bgaDisplayLabel =
      formatBgaDisplayModeLabel(context.settings.bgaDisplayMode);
  const std::string bgaBrightnessLabel =
      formatBgaBrightnessLabel(context.settings.bgaBrightnessPercent);
  const std::string bgaBlurLabel =
      formatBgaBlurLabel(context.settings.bgaBlurStrength);
  const std::string laneAngleLabel =
      formatLaneAngleLabel(context.settings.laneAngleDegrees);
  const std::string laneLengthLabel =
      formatLaneLengthLabel(context.settings.laneLength);

  syncOffsetInputText();
  if (summaryOffsetValueText != nullptr) {
    summaryOffsetValueText->setText(offsetLabel);
  }
  syncVisualOffsetInputText();
  if (summaryVisualOffsetValueText != nullptr) {
    summaryVisualOffsetValueText->setText(visualOffsetLabel);
  }
  syncVisibleTimeInputText();
  if (summaryVisibleTimeValueText != nullptr) {
    summaryVisibleTimeValueText->setText(visibleTimeLabel);
  }
  if (summaryKeysoundValueText != nullptr) {
    summaryKeysoundValueText->setText(keysoundLabel);
  }
  if (summaryBgaValueText != nullptr) {
    summaryBgaValueText->setText(bgaLabel);
  }
  if (summaryBgaDisplayValueText != nullptr) {
    summaryBgaDisplayValueText->setText(bgaDisplayLabel);
  }
  syncBgaBrightnessInputText();
  if (summaryBgaBrightnessValueText != nullptr) {
    summaryBgaBrightnessValueText->setText(bgaBrightnessLabel);
  }
  syncBgaBlurInputText();
  if (summaryBgaBlurValueText != nullptr) {
    summaryBgaBlurValueText->setText(bgaBlurLabel);
  }
  syncLaneAngleInputText();
  if (summaryLaneAngleValueText != nullptr) {
    summaryLaneAngleValueText->setText(laneAngleLabel);
  }
  syncLaneLengthInputText();
  if (summaryLaneLengthValueText != nullptr) {
    summaryLaneLengthValueText->setText(laneLengthLabel);
  }
  if (keysoundModeText != nullptr) {
    keysoundModeText->setText(keysoundLabel);
  }
  if (bgaModeText != nullptr) {
    bgaModeText->setText(bgaLabel);
  }
  if (bgaDisplayModeText != nullptr) {
    bgaDisplayModeText->setText(bgaDisplayLabel);
  }
  if (visibleTimeModeText != nullptr) {
    visibleTimeModeText->setText(context.settings.visibleTimeUseMilliseconds
                                     ? "Milliseconds"
                                     : "Green Number");
  }

  if (visibleTimeModeButton != nullptr) {
    if (context.settings.visibleTimeUseMilliseconds) {
      visibleTimeModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                                 Color(45, 88, 80, 255),
                                                 Color(63, 118, 107, 255));
      visibleTimeModeButton->setBorderColors(Color(97, 157, 142, 255),
                                             Color(120, 187, 169, 255),
                                             Color(145, 214, 195, 255));
    } else {
      visibleTimeModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                                 Color(43, 72, 110, 255),
                                                 Color(59, 98, 147, 255));
      visibleTimeModeButton->setBorderColors(Color(92, 131, 177, 255),
                                             Color(118, 163, 217, 255),
                                             Color(139, 189, 244, 255));
    }
  }

  if (keysoundModeButton != nullptr) {
    if (context.settings.inputKeysoundEnabled) {
      keysoundModeButton->setBackgroundColors(Color(33, 56, 87, 255),
                                              Color(43, 72, 110, 255),
                                              Color(59, 98, 147, 255));
      keysoundModeButton->setBorderColors(Color(92, 131, 177, 255),
                                          Color(118, 163, 217, 255),
                                          Color(139, 189, 244, 255));
    } else {
      keysoundModeButton->setBackgroundColors(Color(73, 56, 35, 255),
                                              Color(96, 72, 45, 255),
                                              Color(127, 95, 59, 255));
      keysoundModeButton->setBorderColors(Color(165, 120, 74, 255),
                                          Color(194, 141, 88, 255),
                                          Color(224, 163, 103, 255));
    }
  }

  if (bgaModeButton != nullptr) {
    if (context.settings.bgaEnabled) {
      bgaModeButton->setBackgroundColors(Color(35, 68, 62, 255),
                                         Color(45, 88, 80, 255),
                                         Color(63, 118, 107, 255));
      bgaModeButton->setBorderColors(Color(97, 157, 142, 255),
                                     Color(120, 187, 169, 255),
                                     Color(145, 214, 195, 255));
    } else {
      bgaModeButton->setBackgroundColors(Color(56, 42, 40, 255),
                                         Color(75, 55, 52, 255),
                                         Color(104, 75, 71, 255));
      bgaModeButton->setBorderColors(Color(141, 103, 98, 255),
                                     Color(176, 127, 121, 255),
                                     Color(209, 150, 143, 255));
    }
  }

  auto applyTabStyle = [this](Button *button, SettingsTab tab) {
    if (button == nullptr) {
      return;
    }
    if (activeTab == tab) {
      button->setBackgroundColors(Color(35, 68, 62, 255),
                                  Color(45, 88, 80, 255),
                                  Color(63, 118, 107, 255));
      button->setBorderColors(Color(97, 157, 142, 255),
                              Color(120, 187, 169, 255),
                              Color(145, 214, 195, 255));
    } else {
      button->setBackgroundColors(Color(28, 40, 58, 255),
                                  Color(36, 52, 75, 255),
                                  Color(61, 87, 118, 255));
      button->setBorderColors(Color(84, 107, 139, 255),
                              Color(108, 136, 174, 255),
                              Color(139, 172, 217, 255));
    }
  };
  applyTabStyle(timingTabButton, SettingsTab::Timing);
  applyTabStyle(visualTabButton, SettingsTab::Visual);
  applyTabStyle(laneTabButton, SettingsTab::Lane);

  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::persistSettings() {
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save settings");
  }
  context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
  context.jukebox.setVisualOffsetMs(context.settings.visualOffsetMs);
  context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
  refreshSettingsText();
}

void SettingsScene::syncOffsetInputText(bool force) {
  if (offsetInput == nullptr) {
    return;
  }
  if (!force && offsetInput->getSelected()) {
    return;
  }
  offsetInput->setEditingText(
      formatOffsetInputValue(context.settings.inputOffsetMs));
}

void SettingsScene::syncVisualOffsetInputText(bool force) {
  if (visualOffsetInput == nullptr) {
    return;
  }
  if (!force && visualOffsetInput->getSelected()) {
    return;
  }
  visualOffsetInput->setEditingText(
      formatOffsetInputValue(context.settings.visualOffsetMs));
}

void SettingsScene::syncVisibleTimeInputText(bool force) {
  if (visibleTimeInput == nullptr) {
    return;
  }
  if (!force && visibleTimeInput->getSelected()) {
    return;
  }
  visibleTimeInput->setEditingText(
      formatVisibleTimeInputValue(context.settings.visibleTimeGreenNumber,
                                  context.settings.visibleTimeUseMilliseconds));
}

void SettingsScene::syncBgaBrightnessInputText(bool force) {
  if (bgaBrightnessInput == nullptr) {
    return;
  }
  if (!force && bgaBrightnessInput->getSelected()) {
    return;
  }
  bgaBrightnessInput->setEditingText(
      std::to_string(context.settings.bgaBrightnessPercent));
}

void SettingsScene::syncBgaBlurInputText(bool force) {
  if (bgaBlurInput == nullptr) {
    return;
  }
  if (!force && bgaBlurInput->getSelected()) {
    return;
  }
  bgaBlurInput->setEditingText(
      formatFloatValue(context.settings.bgaBlurStrength));
}

void SettingsScene::syncLaneAngleInputText(bool force) {
  if (laneAngleInput == nullptr) {
    return;
  }
  if (!force && laneAngleInput->getSelected()) {
    return;
  }
  laneAngleInput->setEditingText(
      formatFloatValue(context.settings.laneAngleDegrees));
}

void SettingsScene::syncLaneLengthInputText(bool force) {
  if (laneLengthInput == nullptr) {
    return;
  }
  if (!force && laneLengthInput->getSelected()) {
    return;
  }
  laneLengthInput->setEditingText(
      formatFloatValue(context.settings.laneLength));
}

void SettingsScene::commitOffsetInput() {
  if (offsetInput == nullptr) {
    return;
  }

  const std::string rawText = offsetInput->getText();
  if (rawText.empty()) {
    syncOffsetInputText(true);
    return;
  }

  try {
    context.settings.inputOffsetMs = clampOffset(std::stoi(rawText));
    persistSettings();
    syncOffsetInputText(true);
  } catch (const std::exception &) {
    syncOffsetInputText(true);
  }
}

void SettingsScene::commitVisualOffsetInput() {
  if (visualOffsetInput == nullptr) {
    return;
  }

  const std::string rawText = visualOffsetInput->getText();
  if (rawText.empty()) {
    syncVisualOffsetInputText(true);
    return;
  }

  try {
    context.settings.visualOffsetMs = clampVisualOffset(std::stoi(rawText));
    persistSettings();
    syncVisualOffsetInputText(true);
  } catch (const std::exception &) {
    syncVisualOffsetInputText(true);
  }
}

void SettingsScene::commitVisibleTimeInput() {
  if (visibleTimeInput == nullptr) {
    return;
  }

  const std::string rawText = visibleTimeInput->getText();
  if (rawText.empty()) {
    syncVisibleTimeInputText(true);
    return;
  }

  try {
    const int parsedValue = std::stoi(rawText);
    if (context.settings.visibleTimeUseMilliseconds) {
      const int milliseconds =
          std::clamp(parsedValue, AppSettings::kMinVisibleTimeMs,
                     AppSettings::kMaxVisibleTimeMs);
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(millisecondsToGreenNumber(milliseconds));
    } else {
      context.settings.visibleTimeGreenNumber =
          clampVisibleTimeGreenNumber(parsedValue);
    }
    persistSettings();
    syncVisibleTimeInputText(true);
  } catch (const std::exception &) {
    syncVisibleTimeInputText(true);
  }
}

void SettingsScene::commitBgaBrightnessInput() {
  if (bgaBrightnessInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBrightnessInput->getText();
  if (rawText.empty()) {
    syncBgaBrightnessInputText(true);
    return;
  }

  try {
    context.settings.bgaBrightnessPercent =
        clampBgaBrightness(std::stoi(rawText));
    persistSettings();
    syncBgaBrightnessInputText(true);
  } catch (const std::exception &) {
    syncBgaBrightnessInputText(true);
  }
}

void SettingsScene::commitBgaBlurInput() {
  if (bgaBlurInput == nullptr) {
    return;
  }

  const std::string rawText = bgaBlurInput->getText();
  if (rawText.empty()) {
    syncBgaBlurInputText(true);
    return;
  }

  try {
    context.settings.bgaBlurStrength = clampBgaBlur(std::stof(rawText));
    persistSettings();
    syncBgaBlurInputText(true);
  } catch (const std::exception &) {
    syncBgaBlurInputText(true);
  }
}

void SettingsScene::commitLaneAngleInput() {
  if (laneAngleInput == nullptr) {
    return;
  }

  const std::string rawText = laneAngleInput->getText();
  if (rawText.empty()) {
    syncLaneAngleInputText(true);
    return;
  }

  try {
    context.settings.laneAngleDegrees = clampLaneAngle(std::stof(rawText));
    persistSettings();
    syncLaneAngleInputText(true);
  } catch (const std::exception &) {
    syncLaneAngleInputText(true);
  }
}

void SettingsScene::commitLaneLengthInput() {
  if (laneLengthInput == nullptr) {
    return;
  }

  const std::string rawText = laneLengthInput->getText();
  if (rawText.empty()) {
    syncLaneLengthInputText(true);
    return;
  }

  try {
    context.settings.laneLength = clampLaneLength(std::stof(rawText));
    persistSettings();
    syncLaneLengthInputText(true);
  } catch (const std::exception &) {
    syncLaneLengthInputText(true);
  }
}

void SettingsScene::update(float dt) {
  (void)dt;
  ensureLayoutUpToDate();
}

void SettingsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
}

void SettingsScene::cleanupScene() {
  rootLayout = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}
