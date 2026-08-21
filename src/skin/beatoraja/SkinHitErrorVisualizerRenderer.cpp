#include "SkinHitErrorVisualizerRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <bit>
#include <cmath>
#include <limits>

namespace skin {
namespace {

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

std::uint32_t singleLineColor(std::uint32_t rgba, int age,
                              int windowLength) noexcept {
  const float alpha = static_cast<float>(rgba & 0xffU) / 255.0F;
  const float adjusted =
      alpha * static_cast<float>(age) /
      (static_cast<float>(windowLength) / 2.0F);
  const auto packedAlpha = static_cast<std::uint32_t>(adjusted * 255.0F);
  return (rgba & 0xffffff00U) | packedAlpha;
}

std::uint32_t judgeColor(const SkinHitErrorVisualizerObject &visualizer,
                         SkinGameplayGraphStateView state,
                         std::int64_t timing) noexcept {
  for (std::size_t grade = 0; grade < 4 && grade < state.judgeWindows.size();
       ++grade) {
    const auto &window = state.judgeWindows[grade];
    if (timing > window.minimumTimingMillis &&
        timing < window.maximumTimingMillis) {
      return visualizer.judgeRgba[grade];
    }
  }
  return visualizer.judgeRgba[4];
}

} // namespace

bool advanceSkinHitErrorVisualizerEma(
    const SkinHitErrorVisualizerObject &visualizer,
    SkinGameplayGraphStateView state,
    SkinHitErrorVisualizerPresentationState &presentation) noexcept {
  if (state.recentJudgeTimingsMillis.empty()) {
    return false;
  }
  const std::size_t index =
      state.recentJudgeTimingIndex % state.recentJudgeTimingsMillis.size();
  if (presentation.recentJudgeTimingIndex == index) {
    return false;
  }
  presentation.recentJudgeTimingIndex = index;
  if (visualizer.emaMode == 0 || state.judgeWindows.size() <= 3) {
    return true;
  }
  const std::int64_t sample = state.recentJudgeTimingsMillis[index];
  const auto &bad = state.judgeWindows[3];
  if (sample == kSkinEmptyJudgeTimingMillis ||
      sample <= bad.minimumTimingMillis || sample >= bad.maximumTimingMillis) {
    return true;
  }
  const float delta =
      visualizer.alpha * static_cast<float>(std::bit_cast<std::int64_t>(
                             static_cast<std::uint64_t>(sample) -
                             static_cast<std::uint64_t>(
                                 presentation.emaMillis)));
  std::int64_t increment = 0;
  if (std::isnan(delta)) {
    increment = 0;
  } else if (delta >= static_cast<float>(
                          std::numeric_limits<std::int64_t>::max())) {
    increment = std::numeric_limits<std::int64_t>::max();
  } else if (delta <= static_cast<float>(
                          std::numeric_limits<std::int64_t>::min())) {
    increment = std::numeric_limits<std::int64_t>::min();
  } else {
    increment = static_cast<std::int64_t>(delta);
  }
  presentation.emaMillis = std::bit_cast<std::int64_t>(
      static_cast<std::uint64_t>(presentation.emaMillis) +
      static_cast<std::uint64_t>(increment));
  return true;
}

SkinHitErrorVisualizerRenderResult renderSkinHitErrorVisualizer(
    const SkinHitErrorVisualizerRenderRequest &request) {
  if (request.geometry.rgba[3] <= 0.0F || request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0) {
    return {};
  }
  if (request.visualizer.width <= 0 || request.visualizer.lineWidth < 1 ||
      request.visualizer.lineWidth > 4 ||
      request.visualizer.windowLength < 1 ||
      request.visualizer.windowLength > 100) {
    return {.failure = diagnostic(
                "skin.renderer.hiterrorvisualizer.invalid",
                "Hit-error visualizer has an invalid pinned geometry "
                "configuration.")};
  }
  const int denominator = std::bit_cast<std::int32_t>(
      static_cast<std::uint32_t>(request.visualizer.judgeWidthMillis) * 2U +
      1U);
  const int sourceWidth = request.visualizer.width;
  const int sourceHeight = request.visualizer.windowLength * 2;
  SkinGeneratedTextureRaster builder(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Primary,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .maximumCommands = request.maximumCommands,
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = sourceWidth,
       .sourceHeight = sourceHeight,
       .verticalFlip = false,
       .diagnosticObject = "Hit-error visualizer"});
  if (!builder.drawable()) return builder.take();

  const auto recent = request.state.recentJudgeTimingsMillis;
  const int center = request.visualizer.judgeWidthMillis;
  const float judgeWidthRate =
      static_cast<float>(request.visualizer.width) /
      static_cast<float>(denominator);
  const int centerX =
      (request.visualizer.width - request.visualizer.lineWidth) / 2;
  const auto sourceX = [&](std::int64_t timing) {
    const int minimum = std::bit_cast<std::int32_t>(
        0U - static_cast<std::uint32_t>(center));
    const std::int64_t clamped = timing < minimum
                                     ? minimum
                                 : timing > center ? center
                                                   : timing;
    return skinGeneratedTextureJavaAdd(
        centerX, skinGeneratedTextureJavaInt(static_cast<double>(
                     static_cast<float>(clamped) * -judgeWidthRate)));
  };

  if (request.visualizer.hitErrorMode && !recent.empty()) {
    const std::size_t index = request.state.recentJudgeTimingIndex % recent.size();
    for (int age = request.visualizer.windowLength; age > 0; --age) {
      const auto backwards =
          static_cast<std::size_t>(request.visualizer.windowLength - age) %
          recent.size();
      const std::size_t sampleIndex =
          (index + recent.size() - backwards) % recent.size();
      const std::int64_t timing = recent[sampleIndex];
      if (timing == kSkinEmptyJudgeTimingMillis) {
        continue;
      }
      const std::uint32_t color = request.visualizer.colorMode
                                      ? judgeColor(request.visualizer,
                                                   request.state, timing)
                                      : singleLineColor(
                                            request.visualizer.lineRgba, age,
                                            request.visualizer.windowLength);
      const int y = request.visualizer.drawDecay
                        ? request.visualizer.windowLength - age
                        : 0;
      const int height = request.visualizer.drawDecay
                             ? age * 2
                             : static_cast<int>(recent.size() * 2U);
      builder.appendRectangle(sourceX(timing), y,
                              request.visualizer.lineWidth, height, color);
    }
  }

  builder.appendRectangle(centerX, 0, request.visualizer.lineWidth,
                          sourceHeight, request.visualizer.centerRgba);

  if (request.visualizer.emaMode != 0) {
    int x = sourceX(request.emaMillis);
    if (request.visualizer.emaMode == 1 || request.visualizer.emaMode == 3)
      builder.appendRectangle(x, 0, request.visualizer.lineWidth,
                              sourceHeight, request.visualizer.emaRgba);
    if (request.visualizer.emaMode == 2 || request.visualizer.emaMode == 3) {
      x += request.visualizer.lineWidth / 2;
      int width = static_cast<int>(request.visualizer.width * 0.01);
      if (width % 2 != 0) {
        ++width;
      }
      builder.appendTriangle(x, sourceHeight / 3, x + width, 0, x - width, 0,
                             request.visualizer.emaRgba);
    }
  }
  return builder.take();
}

} // namespace skin

#endif
