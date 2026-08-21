#include "SkinGaugeGraphRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>

namespace skin {
namespace {

constexpr int kRevealMillis = 1500;
constexpr int kLineWidth = 2;
constexpr std::array kGaugeTypeTable{0, 1, 2, 3, 4, 5, 3, 4, 5};

int graphX(std::size_t index, std::size_t count, float width) noexcept {
  return skinGeneratedTextureJavaInt(static_cast<double>(
      width * static_cast<float>(index) / static_cast<float>(count)));
}

int graphY(float gauge, float maximum, float height) noexcept {
  return skinGeneratedTextureJavaInt(static_cast<double>(
      gauge / maximum * (height - static_cast<float>(kLineWidth))));
}

SkinGeneratedTextureRaster rasterFor(
    const SkinGaugeGraphRenderRequest &request, int revealMillis,
    std::size_t maximumCommands, std::size_t maximumPrimitiveVertices) {
  return SkinGeneratedTextureRaster(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .elapsedMillis = request.elapsedMillis,
       .revealMillis = revealMillis,
       .maximumCommands = maximumCommands,
       .maximumPrimitiveVertices = maximumPrimitiveVertices,
       .sourceRevealWidth =
           revealMillis == kRevealMillis
               ? std::optional<float>{
                     static_cast<float>(request.geometry.rect.width)}
               : std::nullopt,
       .diagnosticObject = "Gauge graph"});
}

SkinGaugeGraphRenderResult combine(
    SkinGaugeGraphRenderResult background,
    SkinGaugeGraphRenderResult shape) {
  if (shape.failure) {
    return {.failure = std::move(shape.failure)};
  }
  background.primitiveVertices += shape.primitiveVertices;
  background.commands.insert(background.commands.end(),
                             std::make_move_iterator(shape.commands.begin()),
                             std::make_move_iterator(shape.commands.end()));
  return background;
}

} // namespace

SkinGaugeGraphRenderResult
renderSkinGaugeGraph(const SkinGaugeGraphRenderRequest &request) {
  if (!request.state.gaugeSupported || request.state.gaugeType < 0 ||
      static_cast<std::size_t>(request.state.gaugeType) >=
          kGaugeTypeTable.size() ||
      !std::isfinite(request.state.gaugeMaximum) ||
      request.state.gaugeMaximum <= 0.0F ||
      !std::isfinite(request.state.gaugeBorder)) {
    return {};
  }

  const auto &colors =
      request.graph.rgba[kGaugeTypeTable[request.state.gaugeType]];
  auto backgroundRaster = rasterFor(
      request, 0, request.maximumCommands, request.maximumPrimitiveVertices);
  if (!backgroundRaster.drawable()) {
    return {};
  }
  const int width = backgroundRaster.textureWidth();
  const int height = backgroundRaster.textureHeight();
  const float authoredWidth =
      static_cast<float>(request.geometry.rect.width);
  const float authoredHeight =
      static_cast<float>(request.geometry.rect.height);
  if (!backgroundRaster.appendRectangle(0, 0, width, height, colors[3])) {
    return backgroundRaster.take();
  }
  const int borderY = skinGeneratedTextureJavaInt(
      static_cast<double>(authoredHeight *
                          request.state.gaugeBorder /
                          request.state.gaugeMaximum));
  const int borderHeight = skinGeneratedTextureJavaInt(
      static_cast<double>(authoredHeight *
                          (request.state.gaugeMaximum -
                           request.state.gaugeBorder) /
                          request.state.gaugeMaximum));
  if (!backgroundRaster.appendRectangle(0, borderY, width, borderHeight,
                                        colors[1])) {
    return backgroundRaster.take();
  }
  auto background = backgroundRaster.take();
  if (request.state.gaugeHistory.size() < 2) {
    return background;
  }

  const std::size_t remainingCommands =
      request.maximumCommands -
      std::min(request.maximumCommands, background.commands.size());
  const std::size_t remainingVertices =
      request.maximumPrimitiveVertices -
      std::min(request.maximumPrimitiveVertices,
               background.primitiveVertices);
  auto shapeRaster =
      rasterFor(request, kRevealMillis, remainingCommands, remainingVertices);
  if (!shapeRaster.drawable()) {
    return background;
  }

  const int borderLineY =
      graphY(request.state.gaugeBorder, request.state.gaugeMaximum,
             authoredHeight);
  float previousGauge = request.state.gaugeHistory.front();
  int lastX = -1;
  int lastY = -1;
  for (std::size_t index = 1; index < request.state.gaugeHistory.size();
       ++index) {
    const float currentGauge = request.state.gaugeHistory[index];
    const int x1 = graphX(index - 1, request.state.gaugeHistory.size(),
                          authoredWidth);
    const int y1 = graphY(previousGauge, request.state.gaugeMaximum,
                          authoredHeight);
    const int x2 = graphX(index, request.state.gaugeHistory.size(),
                          authoredWidth);
    const int y2 = graphY(currentGauge, request.state.gaugeMaximum,
                          authoredHeight);
    lastX = x2;
    lastY = y2;

    if (previousGauge < request.state.gaugeBorder) {
      if (currentGauge < request.state.gaugeBorder) {
        if (!shapeRaster.appendRectangle(
                x1, std::min(y1, y2), kLineWidth,
                std::abs(y2 - y1) + kLineWidth, colors[2]) ||
            !shapeRaster.appendRectangle(x1, y2, x2 - x1, kLineWidth,
                                         colors[2])) {
          return combine(std::move(background), shapeRaster.take());
        }
      } else {
        if (!shapeRaster.appendRectangle(x1, y1, kLineWidth,
                                         borderLineY - y1, colors[2]) ||
            !shapeRaster.appendRectangle(
                x1, borderLineY, kLineWidth,
                y2 - borderLineY + kLineWidth, colors[0]) ||
            !shapeRaster.appendRectangle(x1, y2, x2 - x1, kLineWidth,
                                         colors[0])) {
          return combine(std::move(background), shapeRaster.take());
        }
      }
    } else if (currentGauge >= request.state.gaugeBorder) {
      if (!shapeRaster.appendRectangle(
              x1, std::min(y1, y2), kLineWidth,
              std::abs(y2 - y1) + kLineWidth, colors[0]) ||
          !shapeRaster.appendRectangle(x1, y2, x2 - x1, kLineWidth,
                                       colors[0])) {
        return combine(std::move(background), shapeRaster.take());
      }
    } else {
      if (!shapeRaster.appendRectangle(
              x1, borderLineY, kLineWidth,
              y1 - borderLineY + kLineWidth, colors[0]) ||
          !shapeRaster.appendRectangle(x1, y2, kLineWidth,
                                       borderLineY - y2, colors[2]) ||
          !shapeRaster.appendRectangle(x1, y2, x2 - x1, kLineWidth,
                                       colors[2])) {
        return combine(std::move(background), shapeRaster.take());
      }
    }
    previousGauge = currentGauge;
  }

  if (lastX >= 0) {
    (void)shapeRaster.appendRectangle(
        lastX, lastY,
        skinGeneratedTextureJavaInt(
            static_cast<double>(authoredWidth - static_cast<float>(lastX))),
        kLineWidth,
                                      previousGauge < request.state.gaugeBorder
                                          ? colors[2]
                                          : colors[0]);
  }
  return combine(std::move(background), shapeRaster.take());
}

} // namespace skin

#endif
