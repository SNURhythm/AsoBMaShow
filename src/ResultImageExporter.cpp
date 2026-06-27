#include "ResultImageExporter.h"

#include "ChartDBHelper.h"
#include "PlayOptionUtils.h"
#include "RAII.h"
#include "ReplayResultStateBuilder.h"
#include "ScoreDBHelper.h"
#include "Utils.h"
#include "path.h"
#include "rendering/Color.h"
#include "rendering/RenderPlan.h"
#include "rendering/SimpleBatchRenderer.h"
#include "rendering/common.h"
#include "skin/DefaultSkin.h"
#include "targets.h"
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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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

class ScopedResultImageBgfxAccess {
public:
  explicit ScopedResultImageBgfxAccess(ApplicationContext &context)
      : context(context), lock(context.bgfxRenderMutex, std::defer_lock) {
    context.replayVideoExportActive.store(true, std::memory_order_release);
    lock.lock();
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
    context.replayVideoExportActive.store(false, std::memory_order_release);
    if (lock.owns_lock()) {
      lock.unlock();
    }
    released = true;
  }

private:
  ApplicationContext &context;
  std::unique_lock<std::mutex> lock;
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

void drawResultGaugeGraph(rendering::SimpleBatchRenderer &batch,
                          const RhythmState &resultState,
                          const View *graphPlaceHolder) {
  if (graphPlaceHolder == nullptr || resultState.gaugeHistory.empty()) {
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
  drawResultGaugeLineGraph(batch, resultState, x, y, w, h);
  batch.end();
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

ResultImageExportResult renderResultImage(ApplicationContext &context,
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
                                          const std::filesystem::path &path) {
  const int width = rendering::render_width;
  const int height = rendering::render_height;
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
  ResultSkinData resultSkinData = {&state, &meta, &context};
  resultSkinData.outGraphPlaceholder = &graphPlaceHolder;
  resultSkinData.showControls = false;
  resultSkinData.playModeLabel = playModeLabel;
  resultSkinData.laneOrderLabel = laneOrderLabel;
  resultSkinData.difficultyLabel = difficultyLabel;
  resultSkinData.headerDifficultyLabelOverride = headerDifficultyLabelOverride;
  resultSkinData.currentClearLabelOverride = currentClearLabelOverride;
  resultSkinData.currentClearRankOverride = currentClearRankOverride;
  resultSkinData.previousBest = previousBest;
  DefaultSkin resultSkin;
  resultSkin.buildLayout("Result", resultRoot.get(), &resultSkinData);
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
  drawResultGaugeGraph(graphBatch, state, graphPlaceHolder);
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
  if (!SaveImageToIOSPhotos(path.string(), errorMessage)) {
    return {.success = false, .outputPath = path, .message = errorMessage};
  }
  return {.success = true, .outputPath = path, .message = "Saved to Photos"};
#else
  return {.success = true, .outputPath = path, .message = "Exported"};
#endif
}
} // namespace

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
                                &headerDifficultyLabelOverride) {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  std::string photosErrorMessage;
  if (!RequestIOSPhotoAddAuthorization(photosErrorMessage)) {
    return {.success = false,
            .message = photosErrorMessage.empty()
                           ? "Photos permission was not granted"
                           : photosErrorMessage};
  }
#endif

  std::error_code ec;
  const auto outputDir = Utils::GetDocumentsPath("result_exports");
  std::filesystem::create_directories(outputDir, ec);
  if (ec) {
    return {.success = false,
            .message = "Failed to create result export directory"};
  }

  const auto outputPath =
      outputDir / (sanitizeFileNamePart(meta.Title) + "_" + makeTimestamp() +
                   ".png");
  return renderResultImage(context, meta, state, playModeLabel, laneOrderLabel,
                           difficultyLabel, previousBest,
                           currentClearLabelOverride, currentClearRankOverride,
                           headerDifficultyLabelOverride, outputPath);
}

ResultImageExportResult
ResultImageExporter::ExportReplay(ApplicationContext &context,
                                  bms_parser::Chart &chart,
                                  const ReplayData &replay) {
  RhythmState state = replay_result::BuildResultState(chart, replay);
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<std::string> beforeCreatedAt;
  if (!replay.autoPlay && !replay.createdAt.empty()) {
    beforeCreatedAt = replay.createdAt;
  }
  if (const auto best =
          ScoreDBHelper::GetInstance().LoadBestScore(chart.Meta, beforeCreatedAt);
      best.has_value()) {
    previousBest = toResultPreviousBestData(*best);
  }
  std::string difficultyLabel;
  auto &dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *db = dbHelper.Connect();
  if (db != nullptr) {
    difficultyLabel = dbHelper.DifficultyTableLabelsForChart(db, chart.Meta);
    dbHelper.Close(db);
  }
  const play_options::PlayModeDisplayLabel display =
      play_options::formatPlayModeDisplayLabel(replay);
  return Export(context, chart.Meta, state, display.mode, display.laneOrder,
                difficultyLabel, previousBest);
}
