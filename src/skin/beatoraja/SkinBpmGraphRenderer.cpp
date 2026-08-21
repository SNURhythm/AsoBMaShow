#include "SkinBpmGraphRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
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

int javaInt(double value) noexcept {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(value);
}

int javaAdd(int left, int right) noexcept {
  const auto sum = static_cast<std::uint32_t>(left) +
                   static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(sum);
}

std::optional<UiLogicalRect>
intersect(const std::optional<UiLogicalRect> &clip, const UiLogicalRect &bounds,
          bool &empty) noexcept {
  empty = false;
  if (!clip) {
    if (bounds.width <= 0.0 || bounds.height <= 0.0) {
      empty = true;
      return std::nullopt;
    }
    return bounds;
  }
  const double left = std::max(clip->x, bounds.x);
  const double top = std::max(clip->y, bounds.y);
  const double right = std::min(clip->x + clip->width, bounds.x + bounds.width);
  const double bottom =
      std::min(clip->y + clip->height, bounds.y + bounds.height);
  if (right <= left || bottom <= top) {
    empty = true;
    return std::nullopt;
  }
  return UiLogicalRect{
      .x = left, .y = top, .width = right - left, .height = bottom - top};
}

std::uint8_t modulatedByte(std::uint8_t source, float modulation) noexcept {
  return static_cast<std::uint8_t>(std::clamp(
      static_cast<float>(source) * modulation, 0.0F, 255.0F));
}

std::uint32_t modulatedColor(std::uint32_t abgr,
                             const std::array<float, 4> &rgba) noexcept {
  const auto red = modulatedByte(static_cast<std::uint8_t>(abgr), rgba[0]);
  const auto green = modulatedByte(static_cast<std::uint8_t>(abgr >> 8U),
                                   rgba[1]);
  const auto blue = modulatedByte(static_cast<std::uint8_t>(abgr >> 16U),
                                  rgba[2]);
  const auto alpha = modulatedByte(static_cast<std::uint8_t>(abgr >> 24U),
                                   rgba[3]);
  return static_cast<std::uint32_t>(red) |
         (static_cast<std::uint32_t>(green) << 8U) |
         (static_cast<std::uint32_t>(blue) << 16U) |
         (static_cast<std::uint32_t>(alpha) << 24U);
}

std::uint32_t rgbaToAbgr(std::uint32_t rgba) noexcept {
  return ((rgba >> 24U) & 0xffU) | ((rgba >> 8U) & 0x0000ff00U) |
         ((rgba << 8U) & 0x00ff0000U) | ((rgba << 24U) & 0xff000000U);
}

class RasterBuilder {
public:
  RasterBuilder(const SkinBpmGraphRenderRequest &request, int textureWidth,
                int textureHeight, SkinSourceRegionGeometry source,
                SkinStretchedDestinationGeometry stretched)
      : request_(request), textureWidth_(textureWidth),
        textureHeight_(textureHeight), source_(source),
        stretched_(stretched) {
    const auto projected = projectSkinDestinationToUi(
        request.geometry,
        {.textureWidth = textureWidth,
         .textureHeight = textureHeight,
         .region = {.x = 0,
                    .y = 0,
                    .w = textureWidth,
                    .h = textureHeight}},
        request.viewport);
    bool empty = false;
    clip_ = intersect(projected.clip,
                      projectedSkinScissorBounds(request.viewport), empty);
    clippedOut_ = empty;
    const double radians = request.geometry.angleDegrees * std::numbers::pi /
                           180.0;
    cosine_ = std::cos(radians);
    sine_ = std::sin(radians);
    pivotX_ = stretched_.rect.x +
              request.geometry.centerX * stretched_.rect.width;
    pivotY_ = stretched_.rect.y +
              request.geometry.centerY * stretched_.rect.height;
  }

  bool appendRectangle(int x, int y, int width, int height,
                       std::uint32_t rgba) {
    if (clippedOut_ || width <= 0 || height <= 0 || source_.region.w == 0 ||
        source_.region.h == 0) {
      return true;
    }
    const double textureLeft = 0.0;
    const double textureTop = 0.0;
    const double textureRight = textureWidth_;
    const double textureBottom = textureHeight_;
    const double regionLeft = std::min<double>(
        stretched_.region.x, stretched_.region.x + stretched_.region.w);
    const double regionTop = std::min<double>(
        stretched_.region.y, stretched_.region.y + stretched_.region.h);
    const double regionRight = std::max<double>(
        stretched_.region.x, stretched_.region.x + stretched_.region.w);
    const double regionBottom = std::max<double>(
        stretched_.region.y, stretched_.region.y + stretched_.region.h);
    const double left = std::max({static_cast<double>(x), textureLeft,
                                  regionLeft});
    const double top = std::max({static_cast<double>(y), textureTop,
                                 regionTop});
    const double right = std::min(
        {static_cast<double>(x) + width, textureRight, regionRight});
    const double bottom = std::min(
        {static_cast<double>(y) + height, textureBottom, regionBottom});
    if (right <= left || bottom <= top) {
      return true;
    }

    const std::uint32_t color =
        modulatedColor(rgbaToAbgr(rgba), request_.geometry.rgba);
    if (color >> 24U == 0U) {
      return true;
    }
    constexpr std::size_t vertexCount = 4;
    if (!fits(vertexCount)) {
      return false;
    }

    const auto destinationX = [&](double sourceX) {
      return stretched_.rect.x +
             (sourceX - stretched_.region.x) /
                 static_cast<double>(stretched_.region.w) *
                 stretched_.rect.width;
    };
    const auto destinationY = [&](double sourceY) {
      return stretched_.rect.y + stretched_.rect.height -
             (sourceY - stretched_.region.y) /
                 static_cast<double>(stretched_.region.h) *
                 stretched_.rect.height;
    };
    const double x0 = destinationX(left);
    const double x1 = destinationX(right);
    const double y0 = destinationY(top);
    const double y1 = destinationY(bottom);

    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::SolidQuad;
    primitive.state = {.blend = request_.geometry.blend,
                       .filter = request_.geometry.filter,
                       .scissor = clip_};
    primitive.vertices.reserve(vertexCount);
    for (const auto point : std::array<std::array<double, 2>, 4>{
             {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}}) {
      if (!appendVertex(primitive, point, color)) {
        return false;
      }
    }
    result_.primitiveVertices += primitive.vertices.size();
    result_.commands.push_back(
        {.authoredOrdinal = request_.authoredOrdinal,
         .sourceObject = request_.sourceObject,
         .payload = std::move(primitive)});
    return true;
  }

  SkinBpmGraphRenderResult take() { return std::move(result_); }

private:
  bool fits(std::size_t vertices) {
    if (result_.commands.size() >= request_.maximumCommands ||
        vertices > request_.maximumPrimitiveVertices -
                       std::min(result_.primitiveVertices,
                                request_.maximumPrimitiveVertices)) {
      result_.commands.clear();
      result_.primitiveVertices = 0;
      result_.failure = diagnostic(
          "skin.renderer.command.limit",
          "BPM graph exceeds the fixed frame command or vertex limits.");
      return false;
    }
    return true;
  }

  bool appendVertex(SkinPrimitiveCommand &primitive,
                    const std::array<double, 2> &point,
                    std::uint32_t color) {
    const double relativeX = point[0] - pivotX_;
    const double relativeY = point[1] - pivotY_;
    const double rotatedX =
        pivotX_ + relativeX * cosine_ - relativeY * sine_;
    const double rotatedY =
        pivotY_ + relativeX * sine_ + relativeY * cosine_;
    const std::array projected{
        request_.viewport.authoredToUi.m00 * rotatedX +
            request_.viewport.authoredToUi.m01 * rotatedY +
            request_.viewport.authoredToUi.tx,
        request_.viewport.authoredToUi.m10 * rotatedX +
            request_.viewport.authoredToUi.m11 * rotatedY +
            request_.viewport.authoredToUi.ty,
    };
    if (!std::isfinite(projected[0]) || !std::isfinite(projected[1]) ||
        projected[0] < -std::numeric_limits<float>::max() ||
        projected[0] > std::numeric_limits<float>::max() ||
        projected[1] < -std::numeric_limits<float>::max() ||
        projected[1] > std::numeric_limits<float>::max()) {
      result_.commands.clear();
      result_.primitiveVertices = 0;
      result_.failure = diagnostic(
          "skin.renderer.geometry.invalid",
          "Projected BPM graph geometry is outside float range.");
      return false;
    }
    primitive.vertices.push_back({.x = static_cast<float>(projected[0]),
                                  .y = static_cast<float>(projected[1]),
                                  .rgba = color});
    return true;
  }

  const SkinBpmGraphRenderRequest &request_;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  SkinSourceRegionGeometry source_;
  SkinStretchedDestinationGeometry stretched_;
  SkinBpmGraphRenderResult result_;
  std::optional<UiLogicalRect> clip_;
  bool clippedOut_ = false;
  double cosine_ = 1.0;
  double sine_ = 0.0;
  double pivotX_ = 0.0;
  double pivotY_ = 0.0;
};

int graphY(double speed, double mainBpm, int height, int lineWidth) noexcept {
  const double ratio = std::min(
      std::max(speed / mainBpm, kMinimumRatio), kMaximumRatio);
  return javaInt((std::log10(ratio) - kMinimumRatioLog) /
                 (kMaximumRatioLog - kMinimumRatioLog) *
                 static_cast<double>(height - lineWidth));
}

int graphX(std::int64_t micros, int width, int lastTimeMillis) noexcept {
  const double millis = static_cast<double>(micros) / 1000.0;
  return javaInt(static_cast<double>(width) * millis /
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

  const int textureWidth = javaInt(std::abs(request.geometry.rect.width));
  const int textureHeight = javaInt(std::abs(request.geometry.rect.height));
  if (textureWidth <= 0 || textureHeight <= 0) {
    return {};
  }
  float reveal;
  if (request.elapsedMillis >= request.graph.delayMillis) {
    reveal = 1.0F;
  } else {
    reveal = static_cast<float>(request.elapsedMillis) /
             static_cast<float>(request.graph.delayMillis);
  }
  const int visibleWidth =
      javaInt(static_cast<double>(static_cast<float>(textureWidth) * reveal));
  const int drawWidth = javaInt(
      static_cast<double>(static_cast<float>(request.geometry.rect.width) *
                          reveal));
  if (visibleWidth == 0 || drawWidth == 0) {
    return {};
  }

  SkinSourceRegionGeometry source{
      .textureWidth = textureWidth,
      .textureHeight = textureHeight,
      .region = {.x = 0, .y = 0, .w = visibleWidth, .h = textureHeight},
  };
  auto drawGeometry = request.geometry;
  drawGeometry.rect = {.x = request.geometry.rect.x,
                       .y = request.geometry.rect.y +
                            request.geometry.rect.height,
                       .width = static_cast<double>(drawWidth),
                       .height = -request.geometry.rect.height};
  const auto stretched = stretchSkinDestinationAuthored(drawGeometry, source);
  if (stretched.rect.width == 0.0 || stretched.rect.height == 0.0 ||
      stretched.region.w == 0 || stretched.region.h == 0) {
    return {};
  }
  RasterBuilder builder(request, textureWidth, textureHeight, source,
                        stretched);

  int lastTimeMillis =
      javaInt(static_cast<double>(last->chartTimeMicros) / 1000.0);
  if (!request.state.bpmSeries.empty()) {
    const int songLengthMillis = javaInt(
        static_cast<double>(request.state.bpmSeries.back().chartTimeMicros) /
        1000.0);
    if (songLengthMillis < lastTimeMillis) {
      lastTimeMillis = songLengthMillis;
    }
  }
  lastTimeMillis = javaAdd(lastTimeMillis, 1000);
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
