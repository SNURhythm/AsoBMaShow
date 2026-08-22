#include "SkinBpmGraphRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <cmath>
#include <utility>

namespace skin {
namespace {

constexpr double kMinimumRatio = 1.0 / 8.0;
constexpr double kMaximumRatio = 8.0;
const double kMinimumRatioLog = std::log10(kMinimumRatio);
const double kMaximumRatioLog = std::log10(kMaximumRatio);

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

int graphY(double speed, double mainBpm, int height, int lineWidth) noexcept {
  const double ratio = std::min(
      std::max(speed / mainBpm, kMinimumRatio), kMaximumRatio);
  return skinGeneratedTextureJavaInt(
      (std::log10(ratio) - kMinimumRatioLog) /
      (kMaximumRatioLog - kMinimumRatioLog) *
      static_cast<double>(height - lineWidth));
}

int graphX(std::int64_t micros, int width, int lastTimeMillis) noexcept {
  const double millis = static_cast<double>(micros) / 1000.0;
  return skinGeneratedTextureJavaInt(static_cast<double>(width) * millis /
                                     static_cast<double>(lastTimeMillis));
}

std::uint32_t lineColor(const SkinBpmGraphObject &graph, double speed,
                        SkinGameplayGraphStateView state) noexcept {
  if (speed == state.mainBpm) {
    return graph.mainRgba;
  }
  if (speed == state.minimumBpm) {
    return graph.minimumRgba;
  }
  if (speed == state.maximumBpm) {
    return graph.maximumRgba;
  }
  if (speed <= 0.0) {
    return graph.stopRgba;
  }
  return graph.otherRgba;
}

} // namespace

SkinBpmGraphRenderResult
renderSkinBpmGraph(const SkinBpmGraphRenderRequest &request) {
  if (request.geometry.rgba[3] <= 0.0F ||
      request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0) {
    return {};
  }
  if (request.graph.delayMillis < 0 || request.graph.lineWidth <= 0) {
    return {.failure = diagnostic(
                "skin.renderer.bpmgraph.invalid",
                "BPM graph has invalid normalized timing or line width.")};
  }

  std::size_t emittedCount = 0;
  const SkinBpmGraphPoint *first = nullptr;
  const SkinBpmGraphPoint *last = nullptr;
  for (const auto &point : request.state.bpmSeries) {
    if (!point.emitsGraphPoint) {
      continue;
    }
    if (first == nullptr) {
      first = &point;
    }
    last = &point;
    ++emittedCount;
  }
  if (emittedCount < 2 || request.state.mainBpm <= 0.0) {
    return {};
  }

  SkinGeneratedTextureRaster builder(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .elapsedMillis = request.elapsedMillis,
       .revealMillis = request.graph.delayMillis,
       .maximumCommands = request.maximumCommands,
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .diagnosticObject = "BPM graph",
       .cache = request.cache,
       .contentRevision = 1});
  if (!builder.drawable()) {
    return {};
  }
  const int textureWidth = builder.textureWidth();
  const int textureHeight = builder.textureHeight();

  int lastTimeMillis =
      skinGeneratedTextureJavaInt(
          static_cast<double>(last->chartTimeMicros) / 1000.0);
  if (!request.state.bpmSeries.empty()) {
    const int songLengthMillis = skinGeneratedTextureJavaInt(
        static_cast<double>(request.state.bpmSeries.back().chartTimeMicros) /
        1000.0);
    if (songLengthMillis < lastTimeMillis) {
      lastTimeMillis = songLengthMillis;
    }
  }
  lastTimeMillis = skinGeneratedTextureJavaAdd(lastTimeMillis, 1000);
  const SkinBpmGraphPoint *previous = first;
  for (const auto &current : request.state.bpmSeries) {
    if (!current.emitsGraphPoint || &current == first) {
      continue;
    }
    const int transitionX =
        graphX(current.chartTimeMicros, textureWidth, lastTimeMillis);
    const int previousY = graphY(previous->graphSpeed, request.state.mainBpm,
                                 textureHeight, request.graph.lineWidth);
    const int currentY = graphY(current.graphSpeed, request.state.mainBpm,
                                textureHeight, request.graph.lineWidth);
    const std::int64_t difference =
        std::llabs(static_cast<long long>(currentY) - previousY);
    if (difference - request.graph.lineWidth > 0 &&
        !builder.appendRectangle(
            transitionX, std::min(previousY, currentY) +
                             request.graph.lineWidth,
            request.graph.lineWidth,
            static_cast<int>(difference - request.graph.lineWidth),
            request.graph.transitionRgba)) {
      return builder.take();
    }

    const int previousX =
        graphX(previous->chartTimeMicros, textureWidth, lastTimeMillis);
    const std::int64_t horizontalWidth =
        static_cast<std::int64_t>(transitionX) - previousX +
        request.graph.lineWidth;
    if (horizontalWidth > 0 &&
        !builder.appendRectangle(
            previousX, previousY,
            static_cast<int>(std::min<std::int64_t>(
                horizontalWidth, std::numeric_limits<int>::max())),
            request.graph.lineWidth,
            lineColor(request.graph, previous->graphSpeed, request.state))) {
      return builder.take();
    }
    previous = &current;
  }

  const int finalX =
      graphX(last->chartTimeMicros, textureWidth, lastTimeMillis);
  const int finalY = graphY(last->graphSpeed, request.state.mainBpm,
                            textureHeight, request.graph.lineWidth);
  const std::int64_t finalWidth =
      static_cast<std::int64_t>(textureWidth) - finalX +
      request.graph.lineWidth;
  if (finalWidth > 0) {
    (void)builder.appendRectangle(
        finalX, finalY,
        static_cast<int>(std::min<std::int64_t>(
            finalWidth, std::numeric_limits<int>::max())),
        request.graph.lineWidth,
        lineColor(request.graph, last->graphSpeed, request.state));
  }
  return builder.take();
}

} // namespace skin

#endif
