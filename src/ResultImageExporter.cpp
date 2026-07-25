#include "ResultImageExporter.h"

#include "PlayOptionUtils.h"
#include "RAII.h"
#include "analysis/JudgedPlaybackResultState.h"
#include "ResultPresentationUtils.h"
#include "Utils.h"
#include "path.h"
#include "rendering/Color.h"
#include "rendering/RenderPlan.h"
#include "rendering/SimpleBatchRenderer.h"
#include "rendering/common.h"
#include "scene/PracticeAnalyticsPresentation.h"
#include "scene/PracticeAnalyticsView.h"
#include "scene/ResultGaugeHistory.h"
#include "scene/ResultLayoutGeometry.h"
#include "skin/DefaultSkin.h"
#include "targets.h"
#include "view/TextView.h"
#include "view/UiTheme.h"
#include "view/View.h"
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
#include "iOSNatives.hpp"
#endif

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct ResultImageRenderGeometryState {
  int windowWidth = rendering::window_width;
  int windowHeight = rendering::window_height;
  int renderWidth = rendering::render_width;
  int renderHeight = rendering::render_height;
  float uiScaleX = rendering::ui_scale_x;
  float uiScaleY = rendering::ui_scale_y;
  int uiOffsetX = rendering::ui_offset_x;
  int uiOffsetY = rendering::ui_offset_y;
  int uiViewWidth = rendering::ui_view_width;
  int uiViewHeight = rendering::ui_view_height;
};

void restoreResultImageRenderGeometry(
    const ResultImageRenderGeometryState &state) {
  rendering::window_width = state.windowWidth;
  rendering::window_height = state.windowHeight;
  rendering::render_width = state.renderWidth;
  rendering::render_height = state.renderHeight;
  rendering::ui_scale_x = state.uiScaleX;
  rendering::ui_scale_y = state.uiScaleY;
  rendering::ui_offset_x = state.uiOffsetX;
  rendering::ui_offset_y = state.uiOffsetY;
  rendering::ui_view_width = state.uiViewWidth;
  rendering::ui_view_height = state.uiViewHeight;
}

void restorePrimaryRenderViews(ApplicationContext &context) {
  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  if (context.restoreGameplayRenderViews) {
    context.restoreGameplayRenderViews();
  }
}

std::optional<std::string>
ensureExportDirectoryError(const std::filesystem::path &path,
                           const char *failureMessage) {
  std::error_code error;
  if (Utils::EnsureDirectoryExists(path, error)) {
    return std::nullopt;
  }

  return std::string(failureMessage) + " (" +
         fspath_to_utf8(path) + "): " + error.message();
}

class ScopedResultImageBgfxAccess {
public:
  explicit ScopedResultImageBgfxAccess(ApplicationContext &context)
      : context(context), access(context.rendererAccess.acquireExport()) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    originalResetFlags = context.bgfxResetFlags.load(std::memory_order_relaxed);
    if ((originalResetFlags & BGFX_RESET_VSYNC) != 0) {
      bgfx::reset(rendering::render_width, rendering::render_height,
                  originalResetFlags & ~BGFX_RESET_VSYNC);
      restoreResetFlags = true;
    }
#endif
  }

  ~ScopedResultImageBgfxAccess() { release(); }

  void release() {
    if (released) {
      return;
    }
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
    if (restoreResetFlags) {
      bgfx::reset(rendering::render_width, rendering::render_height,
                  originalResetFlags);
    }
#endif
    restorePrimaryRenderViews(context);
    context.replayVideoExportUiFrameRequested.store(false,
                                                    std::memory_order_release);
    access.release();
    released = true;
  }

private:
  ApplicationContext &context;
  display::RendererAccessCoordinator::ExportReservation access;
  uint32_t originalResetFlags = 0;
  bool restoreResetFlags = false;
  bool released = false;
};

std::string makeTimestamp() {
  std::time_t rawTime = std::time(nullptr);
  std::tm timeInfo{};
#ifdef _WIN32
  localtime_s(&timeInfo, &rawTime);
#else
  localtime_r(&rawTime, &timeInfo);
#endif
  std::ostringstream stream;
  stream << std::put_time(&timeInfo, "%Y%m%d_%H%M%S");
  return stream.str();
}

std::string sanitizeFileNamePart(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else if (ch == ' ' || ch == '.' || ch == '[' || ch == ']') {
      result.push_back('_');
    }
  }

  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    return "result";
  }
  return result.substr(0, 80);
}

void writeBigEndianU32(std::ofstream &out, uint32_t value) {
  const std::array<unsigned char, 4> bytes = {
      static_cast<unsigned char>((value >> 24U) & 0xffU),
      static_cast<unsigned char>((value >> 16U) & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
      static_cast<unsigned char>(value & 0xffU),
  };
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

bool writePngChunk(std::ofstream &out, const char type[4],
                   const std::vector<unsigned char> &data) {
  if (data.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  writeBigEndianU32(out, static_cast<uint32_t>(data.size()));
  out.write(type, 4);
  if (!data.empty()) {
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  mz_ulong crc = mz_crc32(mz_crc32(0, nullptr, 0),
                          reinterpret_cast<const unsigned char *>(type), 4);
  if (!data.empty()) {
    crc = mz_crc32(crc, data.data(), data.size());
  }
  writeBigEndianU32(out, static_cast<uint32_t>(crc));
  return out.good();
}

bool writeBgraPng(const std::filesystem::path &path,
                  const std::vector<unsigned char> &bgra, int width,
                  int height, std::string &errorMessage) {
  if (width <= 0 || height <= 0 ||
      bgra.size() != static_cast<size_t>(width) * height * 4U) {
    errorMessage = "Invalid result image pixels";
    return false;
  }

  const size_t rowBytes = static_cast<size_t>(width) * 4U;
  const size_t filteredRowBytes = rowBytes + 1U;
  std::vector<unsigned char> rgba(filteredRowBytes * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    unsigned char *dst = rgba.data() + static_cast<size_t>(y) * filteredRowBytes;
    dst[0] = 0;
    const unsigned char *src =
        bgra.data() + static_cast<size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      dst[1 + static_cast<size_t>(x) * 4U + 0U] =
          src[static_cast<size_t>(x) * 4U + 2U];
      dst[1 + static_cast<size_t>(x) * 4U + 1U] =
          src[static_cast<size_t>(x) * 4U + 1U];
      dst[1 + static_cast<size_t>(x) * 4U + 2U] =
          src[static_cast<size_t>(x) * 4U + 0U];
      dst[1 + static_cast<size_t>(x) * 4U + 3U] =
          src[static_cast<size_t>(x) * 4U + 3U];
    }
  }

  if (rgba.size() > std::numeric_limits<mz_ulong>::max()) {
    errorMessage = "Result image is too large";
    return false;
  }
  mz_ulong compressedSize =
      mz_compressBound(static_cast<mz_ulong>(rgba.size()));
  std::vector<unsigned char> compressed(compressedSize);
  const int compressResult =
      mz_compress2(compressed.data(), &compressedSize, rgba.data(),
                   static_cast<mz_ulong>(rgba.size()), MZ_BEST_COMPRESSION);
  if (compressResult != MZ_OK) {
    errorMessage = "Failed to compress result image";
    return false;
  }
  compressed.resize(compressedSize);

  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    errorMessage = "Failed to create result image file";
    return false;
  }

  constexpr std::array<unsigned char, 8> kPngSignature = {
      0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  out.write(reinterpret_cast<const char *>(kPngSignature.data()),
            kPngSignature.size());

  std::vector<unsigned char> ihdr(13);
  ihdr[0] = static_cast<unsigned char>((static_cast<uint32_t>(width) >> 24U) &
                                       0xffU);
  ihdr[1] = static_cast<unsigned char>((static_cast<uint32_t>(width) >> 16U) &
                                       0xffU);
  ihdr[2] = static_cast<unsigned char>((static_cast<uint32_t>(width) >> 8U) &
                                       0xffU);
  ihdr[3] = static_cast<unsigned char>(static_cast<uint32_t>(width) & 0xffU);
  ihdr[4] = static_cast<unsigned char>((static_cast<uint32_t>(height) >> 24U) &
                                       0xffU);
  ihdr[5] = static_cast<unsigned char>((static_cast<uint32_t>(height) >> 16U) &
                                       0xffU);
  ihdr[6] = static_cast<unsigned char>((static_cast<uint32_t>(height) >> 8U) &
                                       0xffU);
  ihdr[7] = static_cast<unsigned char>(static_cast<uint32_t>(height) & 0xffU);
  ihdr[8] = 8;
  ihdr[9] = 6;
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;

  if (!writePngChunk(out, "IHDR", ihdr) ||
      !writePngChunk(out, "IDAT", compressed) ||
      !writePngChunk(out, "IEND", {})) {
    errorMessage = "Failed to write result image file";
    return false;
  }
  return true;
}

void forceBgraOpaque(std::vector<unsigned char> &bgra) {
  for (size_t i = 3; i < bgra.size(); i += 4) {
    bgra[i] = 255;
  }
}

void configureResultImageViews(int width, int height,
                               bgfx::FrameBufferHandle frameBuffer) {
  const auto viewWidth = static_cast<uint16_t>(width);
  const auto viewHeight = static_cast<uint16_t>(height);
  for (const auto view : rendering::kGameplayOutputViews) {
    bgfx::setViewFrameBuffer(view, frameBuffer);
  }
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);

  float ortho[16];
  bx::mtxOrtho(ortho, 0.0f, rendering::window_width, rendering::window_height,
               0.0f, 0.0f, 100.0f, 0.0f, bgfx::getCaps()->homogeneousDepth);
  for (const auto view : rendering::kGameplayOrthographicOutputViews) {
    bgfx::setViewTransform(view, nullptr, ortho);
    bgfx::setViewRect(view, 0, 0, viewWidth, viewHeight);
  }
  bgfx::setViewTransform(rendering::main_view, nullptr, ortho);
  bgfx::setViewRect(rendering::main_view, 0, 0, viewWidth, viewHeight);
  bgfx::setViewRect(rendering::readback_view, 0, 0, viewWidth, viewHeight);
  rendering::applyViewOrder(rendering::blur_view_h, rendering::blur_view_v,
                            rendering::final_view);
}

void drawResultGaugeGraphPrimitive(
    rendering::SimpleBatchRenderer &batch,
    const result_gauge_history::ResultGaugeGraph &graph, float x, float y,
    float w, float h) {
  batch.addRect(x, y, w, h, ui_theme::resultPanelSubtle().toABGR());

  const float padding = 8.0f;
  const float graphX = x + padding;
  const float graphY = y + padding;
  const float graphW = std::max(1.0f, w - padding * 2.0f);
  const float graphH = std::max(1.0f, h - padding * 2.0f);
  const auto pointX = [graphX, graphW](const auto &point) {
    return graphX + point.normalizedX * graphW;
  };
  const auto pointY = [graphY, graphH](const auto &point) {
    return graphY + point.normalizedY * graphH;
  };

  const uint32_t guideColor = ui_theme::hairlineSubtle().toABGR();
  const float guide80Y = graphY + graph.geometry.guide80Y * graphH;
  const float guide30Y = graphY + graph.geometry.guide30Y * graphH;
  batch.addLine(graphX, guide80Y, graphX + graphW, guide80Y, 1.0F,
                guideColor);
  batch.addLine(graphX, guide30Y, graphX + graphW, guide30Y, 1.0F,
                guideColor);

  for (const auto &segment : graph.geometry.segments) {
    batch.addLine(pointX(segment.from), pointY(segment.from),
                  pointX(segment.to), pointY(segment.to), 3.0F,
                  segment.to.color.toABGR());
  }

  const float markerRadius = graph.geometry.segments.empty() ? 3.5F : 2.5F;
  for (const auto &marker : graph.geometry.markers) {
    batch.addCircle(pointX(marker), pointY(marker), markerRadius,
                    marker.color.toABGR());
  }
}

class ResultImageGaugeGraphView final : public View {
public:
  explicit ResultImageGaugeGraphView(
      result_gauge_history::ResultGaugeGraph graph)
      : graph(std::move(graph)) {
    batch.setSubmitView(rendering::ui_view);
  }

protected:
  void renderImpl(RenderContext &context) override {
    if (getWidth() <= 0 || getHeight() <= 0) {
      return;
    }
    rendering::setScissorUI(context.scissor.x, context.scissor.y,
                            context.scissor.width, context.scissor.height);
    batch.begin(context.getTransformMatrix());
    drawResultGaugeGraphPrimitive(batch, graph, static_cast<float>(getX()),
                                  static_cast<float>(getY()),
                                  static_cast<float>(getWidth()),
                                  static_cast<float>(getHeight()));
    batch.end();
  }

private:
  result_gauge_history::ResultGaugeGraph graph;
  rendering::SimpleBatchRenderer batch;
};

void attachPresentationGaugeGraph(
    View *graphPlaceHolder,
    const result_gauge_history::ResultGaugeGraph &graph) {
  if (graphPlaceHolder == nullptr) {
    return;
  }
  auto *graphView = new ResultImageGaugeGraphView(graph);
  graphView->setWidthPercent(100.0F)->setFlex(1.0F);
  graphPlaceHolder->addView(graphView);

  if (!graph.label.has_value()) {
    return;
  }
  auto *gaugeLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
  gaugeLabel->setText(graph.label->text);
  gaugeLabel->setAlign(TextView::CENTER);
  gaugeLabel->setVAlign(TextView::MIDDLE);
  gaugeLabel->setPositionType(YGPositionTypeAbsolute);
  gaugeLabel->setPosition(Edge::Left, 12);
  gaugeLabel->setPosition(Edge::Top, 12);
  gaugeLabel->setWidth(142);
  gaugeLabel->setHeight(30);
  gaugeLabel->setCornerRadius(6);
  gaugeLabel->setZIndex(2);
  gaugeLabel->setName("resultGaugeLabel");
  gaugeLabel->setBackgroundColor(graph.label->background);
  gaugeLabel->setColor(ui_theme::sdl(ui_theme::textOn(graph.label->background)));
  graphPlaceHolder->addView(gaugeLabel);
}

void drawResultGaugeGraph(rendering::SimpleBatchRenderer &batch,
                          const std::optional<
                              result_gauge_history::ResultGaugeGraph> &graph,
                          const View *graphPlaceHolder) {
  if (graphPlaceHolder == nullptr || !graph.has_value()) {
    return;
  }

  const float x = graphPlaceHolder->getX();
  const float y = graphPlaceHolder->getY();
  const float w = graphPlaceHolder->getWidth();
  const float h = graphPlaceHolder->getHeight();
  if (w <= 0.0f || h <= 0.0f) {
    return;
  }

  batch.setSubmitView(rendering::ui_view);
  batch.setSubmitDepth(0);
  batch.begin();
  drawResultGaugeGraphPrimitive(batch, *graph, x, y, w, h);
  batch.end();
}

bms_parser::ChartMeta courseResultMetaForReplay(
    const JudgedCoursePlaybackData &replay,
    const std::vector<std::unique_ptr<bms_parser::Chart>> &charts) {
  int totalNotes = 0;
  long long playLength = 0;
  for (const auto &chart : charts) {
    if (chart == nullptr) {
      continue;
    }
    totalNotes += std::max(0, chart->Meta.TotalNotes);
    playLength += std::max(0LL, chart->Meta.PlayLength);
  }
  return result_presentation::courseResultMeta(
      replay.courseName, replay.courseGroupName, replay.stages.size(),
      totalNotes, playLength);
}

RhythmState courseResultStateForReplay(
    const JudgedCoursePlaybackData &replay,
    const std::vector<RhythmState> &stageStates) {
  RhythmState aggregate(nullptr, false);
  aggregate.configureGauge(replay.initialGaugeType, replay.gaugeAutoShift,
                           replay.gaugeProfile,
                           replay.gaugeAutoShiftLowerBound);
  aggregate.resetJudgeCounts();
  aggregate.comboBreak = 0;
  aggregate.maxCombo = 0;
  aggregate.fastCount = 0;
  aggregate.slowCount = 0;
  aggregate.gaugeHistory.clear();

  for (const auto &state : stageStates) {
    for (int i = 0; i < JudgementCount; ++i) {
      aggregate.addJudgeCountFrom(state, static_cast<Judgement>(i));
    }
    aggregate.comboBreak += state.comboBreak;
    aggregate.fastCount += state.fastCount;
    aggregate.slowCount += state.slowCount;
    aggregate.maxCombo = std::max(aggregate.maxCombo, state.maxCombo);
    aggregate.gaugeHistory.insert(aggregate.gaugeHistory.end(),
                                  state.gaugeHistory.begin(),
                                  state.gaugeHistory.end());
    aggregate.combo = state.combo;
    aggregate.currentGauge = state.currentGauge;
    aggregate.gaugeType = state.gaugeType;
    aggregate.gaugeValues = state.gaugeValues;
    aggregate.gaugeSurvivalFailed = state.gaugeSurvivalFailed;
  }
  aggregate.currentGauge = replay.finalGauge;
  aggregate.gaugeType = replay.initialGaugeType;
  if (!stageStates.empty()) {
    aggregate.gaugeType = stageStates.back().gaugeType;
  }
  const int gaugeIndex = gaugeTypeIndex(aggregate.gaugeType);
  if (gaugeIndex >= 0 &&
      gaugeIndex < static_cast<int>(aggregate.gaugeValues.size())) {
    aggregate.gaugeValues[gaugeIndex] = aggregate.currentGauge;
  }
  return aggregate;
}

ResultImageExportResult renderResultImageWithSkinData(
    ApplicationContext &context, ResultSkinData resultSkinData,
    const std::optional<result_gauge_history::ResultGaugeGraph> &gaugeGraph,
    const std::optional<practice::ResultModel> &analyticsModel,
    bool attachGaugeAsView, const std::filesystem::path &path) {
  const int width = rendering::render_width;
  const bool mobileTarget =
      TARGET_PLATFORM == iOS || TARGET_PLATFORM == Android;
  const auto layoutMetrics = result_layout::metricsFor(
      static_cast<float>(rendering::window_height), mobileTarget);
  const int height =
      analyticsModel.has_value()
          ? result_layout::photoCanvasPixelHeight(
                width, rendering::render_height,
                static_cast<float>(rendering::window_width), layoutMetrics)
          : rendering::render_height;
  if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) {
    return {.success = false,
            .outputPath = path,
            .message = "Result image size is invalid"};
  }

  ScopedResultImageBgfxAccess bgfxAccess(context);
  const ResultImageRenderGeometryState geometry;
  auto restoreGeometry = makeScopeExit([&]() {
    restoreResultImageRenderGeometry(geometry);
    restorePrimaryRenderViews(context);
  });

  if (analyticsModel.has_value()) {
    rendering::updateUIScale(width, height);
  }

  const auto outputTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(outputTexture)) {
    return {.success = false,
            .outputPath = path,
            .message = "Failed to create result image render target"};
  }

  bgfx::FrameBufferHandle outputFrameBuffer = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
  std::unique_ptr<View> resultRoot;
  auto cleanupBgfx = makeScopeExit([&]() {
    restoreGeometry.runNow();
    resultRoot.reset();
    if (bgfx::isValid(readbackTexture)) {
      bgfx::destroy(readbackTexture);
    }
    if (bgfx::isValid(outputFrameBuffer)) {
      bgfx::destroy(outputFrameBuffer);
    }
    if (bgfx::isValid(outputTexture)) {
      bgfx::destroy(outputTexture);
    }
  });

  outputFrameBuffer = bgfx::createFrameBuffer(1, &outputTexture, false);
  if (!bgfx::isValid(outputFrameBuffer)) {
    return {.success = false,
            .outputPath = path,
            .message = "Failed to create result image framebuffer"};
  }
  readbackTexture = bgfx::createTexture2D(
      static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1,
      bgfx::TextureFormat::BGRA8,
      BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  if (!bgfx::isValid(readbackTexture)) {
    return {.success = false,
            .outputPath = path,
            .message = "Failed to create result image readback texture"};
  }

  configureResultImageViews(width, height, outputFrameBuffer);

  View *graphPlaceHolder = nullptr;
  resultRoot = std::make_unique<View>(0, 0, rendering::window_width,
                                      rendering::window_height);
  resultSkinData.context = &context;
  resultSkinData.outGraphPlaceholder = &graphPlaceHolder;
  DefaultSkin resultSkin;
  resultSkin.buildLayout("Result", resultRoot.get(), &resultSkinData);
  if (analyticsModel.has_value()) {
    const float visualHeight = layoutMetrics.photoPrimaryHeight +
                               layoutMetrics.photoSecondaryHeight +
                               layoutMetrics.photoGridGap;
    auto *photoVisuals = new View();
    photoVisuals->setName("resultPhotoVisuals");
    photoVisuals->setWidthPercent(100.0f);
    photoVisuals->setHeight(visualHeight);
    photoVisuals->setMinHeight(visualHeight);
    photoVisuals->setFlexShrink(0.0f);
    photoVisuals->setFlexDirection(FlexDirection::Column);
    photoVisuals->setAlignItems(YGAlignStretch);
    photoVisuals->setGap(layoutMetrics.photoGridGap);

    auto makeRow = [&](float rowHeight) {
      auto *row = new View();
      row->setWidthPercent(100.0f);
      row->setHeight(rowHeight);
      row->setMinHeight(rowHeight);
      row->setFlexShrink(0.0f);
      row->setFlexDirection(FlexDirection::Row);
      row->setAlignItems(YGAlignStretch);
      row->setGap(layoutMetrics.photoGridGap);
      photoVisuals->addView(row);
      return row;
    };
    auto *primaryRow = makeRow(layoutMetrics.photoPrimaryHeight);
    primaryRow->setName("resultPhotoPrimaryRow");
    auto *secondaryRow = makeRow(layoutMetrics.photoSecondaryHeight);
    secondaryRow->setName("resultPhotoSecondaryRow");

    const auto prepareCell = [](View *cell) {
      cell->setWidth(0.0f);
      cell->setMinWidth(0.0f);
      cell->setFlexBasis(0.0f);
      cell->setFlexGrow(1.0f);
      cell->setFlexShrink(1.0f);
      return cell;
    };
    const auto makeGraphCell = [&]() {
      auto *graph = new View();
      graph->setName("graph");
      graph->setBackgroundColor(ui_theme::resultPanelSubtle());
      graph->setCornerRadius(ui_theme::panelRadius());
      graph->setShadow(ui_theme::cardShadow(), ui_theme::kCardShadow);
      graph->setBorderColor(ui_theme::hairlineSubtle());
      graph->setBorderWidth(1);
      return prepareCell(graph);
    };
    const auto makeAnalyticsCell = [&](PracticeAnalyticsMode mode) {
      auto *analyticsView = new PracticeAnalyticsView(*analyticsModel);
      analyticsView->setMode(mode);
      analyticsView->setPhotoExportPresentation(
          practice_analytics_presentation::
              photoExportShowsSharedInformation(mode));
      return prepareCell(analyticsView);
    };

    const auto visualOrder = result_layout::photoVisualOrder();
    for (std::size_t index = 0; index < visualOrder.size(); ++index) {
      View *cell = nullptr;
      switch (visualOrder[index]) {
      case result_layout::PhotoVisual::Gauge:
        cell = makeGraphCell();
        graphPlaceHolder = cell;
        break;
      case result_layout::PhotoVisual::Histogram:
        cell = makeAnalyticsCell(PracticeAnalyticsMode::Histogram);
        break;
      case result_layout::PhotoVisual::Lanes:
        cell = makeAnalyticsCell(PracticeAnalyticsMode::Lanes);
        break;
      case result_layout::PhotoVisual::Sections:
        cell = makeAnalyticsCell(PracticeAnalyticsMode::Sections);
        break;
      }
      if (index < 2) {
        primaryRow->addView(cell);
      } else {
        secondaryRow->addView(cell);
      }
    }
    resultRoot->addView(photoVisuals);
  }
  if (attachGaugeAsView && gaugeGraph.has_value()) {
    attachPresentationGaugeGraph(graphPlaceHolder, *gaugeGraph);
  }
  resultRoot->applyYogaLayout();

  RenderContext renderContext;
  rendering::SimpleBatchRenderer backdropBatch;
  rendering::SimpleBatchRenderer graphBatch;
  bgfx::touch(rendering::clear_view);
  bgfx::touch(rendering::ui_view);
  backdropBatch.setSubmitView(rendering::ui_view);
  backdropBatch.begin();
  backdropBatch.addRect(0.0f, 0.0f, static_cast<float>(rendering::window_width),
                        static_cast<float>(rendering::window_height),
                        ui_theme::backdrop().toABGR());
  backdropBatch.end();
  resultRoot->render(renderContext);
  if (!attachGaugeAsView) {
    drawResultGaugeGraph(graphBatch, gaugeGraph, graphPlaceHolder);
  }
  bgfx::blit(rendering::readback_view, readbackTexture, 0, 0, outputTexture);
  uint32_t currentFrame = bgfx::frame();

  std::vector<unsigned char> pixels(static_cast<size_t>(width) *
                                    static_cast<size_t>(height) * 4U);
  const uint32_t expectedFrame = bgfx::readTexture(readbackTexture,
                                                   pixels.data());
  while (currentFrame < expectedFrame) {
    currentFrame = bgfx::frame();
  }
  forceBgraOpaque(pixels);

  resultRoot.reset();
  cleanupBgfx.runNow();
  bgfxAccess.release();

  std::string errorMessage;
  if (!writeBgraPng(path, pixels, width, height, errorMessage)) {
    return {.success = false, .outputPath = path, .message = errorMessage};
  }

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  if (!SaveImageToIOSPhotos(fspath_to_utf8(path), errorMessage)) {
    return {.success = false, .outputPath = path, .message = errorMessage};
  }
  return {.success = true, .outputPath = path, .message = "Saved to Photos"};
#else
  return {.success = true, .outputPath = path, .message = "Exported"};
#endif
}

ResultImageExportResult renderResultImage(
    ApplicationContext &context, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const std::string &playModeLabel,
    const std::string &laneOrderLabel, const std::string &difficultyLabel,
    const std::optional<ResultPreviousBestData> &previousBest,
    const std::optional<std::string> &currentClearLabelOverride,
    const std::optional<int> &currentClearRankOverride,
    const std::optional<std::string> &headerDifficultyLabelOverride,
    const std::optional<ResultPacemakerData> &pacemaker,
    const std::optional<practice::ResultModel> &analyticsModel,
    const std::filesystem::path &path) {
  const auto series = result_gauge_history::seriesFor(state);
  const auto gaugeGraph = result_gauge_history::graphFor(series, 0);
  ResultSkinData resultSkinData = {&state, &meta, &context};
  resultSkinData.showControls = false;
  resultSkinData.showResultGraph = !analyticsModel.has_value();
  if (series.empty()) {
    resultSkinData.showResultGraph = false;
  }
  resultSkinData.playModeLabel = playModeLabel;
  resultSkinData.laneOrderLabel = laneOrderLabel;
  resultSkinData.difficultyLabel = difficultyLabel;
  resultSkinData.headerDifficultyLabelOverride = headerDifficultyLabelOverride;
  resultSkinData.currentClearLabelOverride = currentClearLabelOverride;
  resultSkinData.currentClearRankOverride = currentClearRankOverride;
  resultSkinData.previousBest = previousBest;
  resultSkinData.pacemaker = pacemaker;
  return renderResultImageWithSkinData(context, std::move(resultSkinData),
                                       gaugeGraph, analyticsModel, false, path);
}
} // namespace

ResultImageExportResult ResultImageExporter::Export(
    ApplicationContext &context,
    const ResultPresentationModel &presentation) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  return Export(
      presentation,
      {.outputDirectory = Utils::GetDocumentsPath("result_exports"),
       .timestamp = makeTimestamp()},
      [&context](ResultSkinData skinData,
                 std::optional<result_gauge_history::ResultGaugeGraph> gauge,
                 const std::filesystem::path &outputPath) {
        return renderResultImageWithSkinData(
            context, std::move(skinData), gauge, std::nullopt, true,
            outputPath);
      });
}

ResultImageExportResult
ResultImageExporter::Export(ApplicationContext &context,
                            const bms_parser::ChartMeta &meta,
                            const RhythmState &state,
                            const std::string &playModeLabel,
                            const std::string &laneOrderLabel,
                            const std::string &difficultyLabel,
                            const std::optional<ResultPreviousBestData>
                                &previousBest,
                            const std::optional<std::string>
                                &currentClearLabelOverride,
                            const std::optional<int>
                                &currentClearRankOverride,
                            const std::optional<std::string>
                                &headerDifficultyLabelOverride,
                            const std::optional<ResultPacemakerData>
                                &pacemaker,
                            const std::optional<practice::ResultModel>
                                &analyticsModel) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  const auto outputDir = Utils::GetDocumentsPath("result_exports");
  if (const auto error = ensureExportDirectoryError(
          outputDir, "Failed to create result export directory")) {
    return {.success = false, .message = *error};
  }

  const auto outputPath =
      outputDir / (sanitizeFileNamePart(meta.Title) + "_" + makeTimestamp() +
                   ".png");
  return renderResultImage(context, meta, state, playModeLabel, laneOrderLabel,
                           difficultyLabel, previousBest,
                           currentClearLabelOverride, currentClearRankOverride,
                           headerDifficultyLabelOverride, pacemaker,
                           analyticsModel,
                           outputPath);
}

ResultImageExportResult
ResultImageExporter::ExportReplay(ApplicationContext &context,
                                  bms_parser::Chart &chart,
                                  const JudgedPlaybackData &replay,
                                  const std::string &pacemakerTarget) {
  RhythmState state = analysis::BuildResultState(chart, replay);
  std::optional<ResultPreviousBestData> previousBest =
      result_presentation::previousBestForReplayChart(
          context.scoreRepository, chart.Meta, replay);
  std::optional<ResultPacemakerData> pacemaker;
  if (!replay.autoPlay) {
    const std::string target =
        pacemakerTarget.empty() ? context.settings.selectedPacemakerTarget
                                : pacemakerTarget;
    pacemaker = result_presentation::pacemakerDataForReplayResult(
        context.replayRepository, chart, state, replay, target, previousBest);
  }
  std::string difficultyLabel =
      result_presentation::difficultyLabelForChart(context.chartRepository,
                                                    chart.Meta);
  const play_options::PlayModeDisplayLabel display =
      play_options::formatPlayModeDisplayLabel(replay);
  const std::span<const JudgedPlaybackData> attempts(&replay, 1);
  const std::optional<practice::ResultModel> analyticsModel(
      std::in_place, chart, attempts, 0);
  return Export(context, chart.Meta, state, display.mode, display.laneOrder,
                difficultyLabel, previousBest, std::nullopt, std::nullopt,
                std::nullopt, pacemaker, analyticsModel);
}

ResultImageExportResult
ResultImageExporter::ExportCourseReplay(ApplicationContext &context,
                                        const JudgedCoursePlaybackData &replay) {
  if (replay.stages.empty()) {
    return {.success = false, .message = "No Course Replay"};
  }

#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  const auto outputRoot = Utils::GetDocumentsPath("result_exports");
  if (const auto error = ensureExportDirectoryError(
          outputRoot, "Failed to create result export directory")) {
    return {.success = false, .message = *error};
  }

  const std::string timestamp = makeTimestamp();
  const std::string courseName =
      replay.courseName.empty() ? "Course Replay" : replay.courseName;
  const auto outputDir =
      outputRoot / (sanitizeFileNamePart(courseName) + "_" + timestamp);
  if (const auto error = ensureExportDirectoryError(
          outputDir, "Failed to create course result export directory")) {
    return {.success = false, .message = *error};
  }

  std::vector<std::unique_ptr<bms_parser::Chart>> charts;
  std::vector<RhythmState> stageStates;
  charts.reserve(replay.stages.size());
  stageStates.reserve(replay.stages.size());
  for (size_t i = 0; i < replay.stages.size(); ++i) {
    const JudgedPlaybackData &stageReplay = replay.stages[i].replay;
    std::atomic_bool parseCancelled = false;
    auto chart = play_options::prepareReplayChart(stageReplay.chartMeta.BmsPath,
                                                  stageReplay, parseCancelled);
    if (chart == nullptr || parseCancelled) {
      return {.success = false,
              .outputPath = outputDir,
              .message = "Failed to load course replay stage"};
    }

    RhythmState state = analysis::BuildResultState(
        *chart, stageReplay, replay.gaugeProfile);
    const play_options::PlayModeDisplayLabel display =
        play_options::formatPlayModeDisplayLabel(stageReplay);
    const std::string filename =
        "stage_" + std::to_string(i + 1) + "_" +
        sanitizeFileNamePart(chart->Meta.Title) + ".png";
    const std::span<const JudgedPlaybackData> attempts(&stageReplay, 1);
    const std::optional<practice::ResultModel> analyticsModel(
        std::in_place, *chart, attempts, 0);
    const auto result = renderResultImage(
        context, chart->Meta, state, display.mode, display.laneOrder,
        result_presentation::difficultyLabelForChart(context.chartRepository,
                                                      chart->Meta),
        result_presentation::previousBestForReplayChart(
            context.scoreRepository, chart->Meta, stageReplay),
        "NO PLAY", kNoClearTypeRank, std::nullopt, std::nullopt,
        analyticsModel,
        outputDir / filename);
    if (!result.success) {
      return result;
    }

    charts.push_back(std::move(chart));
    stageStates.push_back(std::move(state));
  }

  bms_parser::ChartMeta courseMeta = courseResultMetaForReplay(replay, charts);
  RhythmState courseState = courseResultStateForReplay(replay, stageStates);
  const play_options::PlayModeDisplayLabel display =
      replay.stages.empty()
          ? play_options::PlayModeDisplayLabel{}
          : play_options::formatPlayModeDisplayLabel(replay.stages.back().replay);
  std::optional<std::string> clearLabelOverride;
  std::optional<int> clearRankOverride;
  const bool fullCombo = result_presentation::isFullComboCourseResult(
      replay.completedCharts, replay.totalCharts, replay.stages.size(),
      courseState, courseMeta);
  const int clearRank = clear_policy::fullComboRankForPlayback(
      replay.clearType, fullCombo, replay.context.playback);
  clearLabelOverride = clearTypeRankToLabel(clearRank);
  clearRankOverride = clearRank;
  const auto courseResult = renderResultImage(
      context, courseMeta, courseState, display.mode, display.laneOrder,
      "Course", std::nullopt, clearLabelOverride, clearRankOverride, "COURSE",
      std::nullopt, std::nullopt, outputDir / "course_result.png");
  if (!courseResult.success) {
    return courseResult;
  }

  const int exportedCount = static_cast<int>(stageStates.size()) + 1;
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  return {.success = true,
          .outputPath = outputDir,
          .message = "Saved to Photos"};
#else
  return {.success = true,
          .outputPath = outputDir,
          .message = "Exported " + std::to_string(exportedCount) + " photos"};
#endif
}
