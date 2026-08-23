#include "SkinTimingVisualizerRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <utility>

namespace skin {
namespace {

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
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

class PrimitiveBuilder {
public:
  PrimitiveBuilder(const SkinTimingVisualizerRenderRequest &request,
                   AuthoredDestinationGeometry geometry, int sourceWidth)
      : request_(request) {
    setGeometry(std::move(geometry), sourceWidth);
  }

  void setGeometry(AuthoredDestinationGeometry geometry, int sourceWidth) {
    geometry_ = std::move(geometry);
    const SkinSourceRegionGeometry source{
        .textureWidth = sourceWidth,
        .textureHeight = 1,
        .region = {.x = 0, .y = 0, .w = sourceWidth, .h = 1}};
    const auto projected =
        projectSkinDestinationToUi(geometry_, source, request_.viewport);
    bool empty = false;
    clip_ = intersect(projected.clip,
                      projectedSkinScissorBounds(request_.viewport), empty);
    clippedOut_ = empty;
    const double radians = geometry_.angleDegrees * std::numbers::pi / 180.0;
    cosine_ = std::cos(radians);
    sine_ = std::sin(radians);
    pivotX_ = geometry_.rect.x + geometry_.centerX * geometry_.rect.width;
    pivotY_ = geometry_.rect.y + geometry_.centerY * geometry_.rect.height;
  }

  bool appendQuad(double x, double y, double width, double height,
                  std::uint32_t color) {
    if (width == 0.0 || height == 0.0 || color >> 24U == 0U) {
      return true;
    }
    auto lineGeometry = request_.geometry;
    lineGeometry.rect = {.x = x, .y = y, .width = width, .height = height};
    // SkinTimingVisualizer calls the explicit draw overload for recent
    // lines: the per-line color and angle zero replace destination tint and
    // rotation, while blend/filter/clip/stretch remain authored state.
    lineGeometry.angleDegrees = 0.0;
    lineGeometry.rgba = {1.0F, 1.0F, 1.0F, 1.0F};
    const int sourceWidth = std::max(
        1, skinGeneratedTextureJavaInt(std::abs(width)));
    const auto projected = projectSkinDestinationToUi(
        lineGeometry,
        {.textureWidth = sourceWidth,
         .textureHeight = 1,
         .region = {.x = 0, .y = 0, .w = sourceWidth, .h = 1}},
        request_.viewport);
    bool empty = false;
    const auto clip = intersect(projected.clip,
                                projectedSkinScissorBounds(request_.viewport),
                                empty);
    if (empty) {
      return true;
    }
    constexpr std::size_t vertexCount = 4;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::SolidQuad;
    primitive.state = {.blend = projected.blend,
                       .filter = projected.filter,
                       .scissor = clip};
    primitive.vertices.reserve(vertexCount);
    for (const auto &point : projected.vertices) {
      if (!finite(point)) {
        failGeometry();
        return false;
      }
      primitive.vertices.push_back(
          {.x = static_cast<float>(point[0]),
           .y = static_cast<float>(point[1]),
           .rgba = color});
    }
    append(std::move(primitive));
    return true;
  }

  bool appendLine(double x0, double y0, double x1, double y1,
                  std::uint32_t color) {
    color = modulatedColor(color, geometry_.rgba);
    if (clippedOut_ || color >> 24U == 0U) {
      return true;
    }
    constexpr std::size_t vertexCount = 2;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::LineStrip;
    primitive.state = {.blend = geometry_.blend,
                       .filter = geometry_.filter,
                       .scissor = clip_};
    primitive.vertices.reserve(vertexCount);
    for (const auto point :
         std::array<std::array<double, 2>, 2>{{{x0, y0}, {x1, y1}}}) {
      const auto projected = project(point[0], point[1]);
      if (!finite(projected)) {
        failGeometry();
        return false;
      }
      primitive.vertices.push_back(
          {.x = static_cast<float>(projected[0]),
           .y = static_cast<float>(projected[1]),
           .rgba = color});
    }
    append(std::move(primitive));
    return true;
  }

  SkinTimingVisualizerRenderResult take() { return std::move(result_); }

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
          "Timing visualizer exceeds the fixed frame limits.");
      return false;
    }
    return true;
  }

  std::array<double, 2> project(double x, double y) const noexcept {
    const double relativeX = x - pivotX_;
    const double relativeY = y - pivotY_;
    const double rotatedX =
        pivotX_ + relativeX * cosine_ - relativeY * sine_;
    const double rotatedY =
        pivotY_ + relativeX * sine_ + relativeY * cosine_;
    return {
        request_.viewport.authoredToUi.m00 * rotatedX +
            request_.viewport.authoredToUi.m01 * rotatedY +
            request_.viewport.authoredToUi.tx,
        request_.viewport.authoredToUi.m10 * rotatedX +
            request_.viewport.authoredToUi.m11 * rotatedY +
            request_.viewport.authoredToUi.ty,
    };
  }

  static bool finite(const std::array<double, 2> &point) noexcept {
    return std::isfinite(point[0]) && std::isfinite(point[1]) &&
           point[0] >= -std::numeric_limits<float>::max() &&
           point[0] <= std::numeric_limits<float>::max() &&
           point[1] >= -std::numeric_limits<float>::max() &&
           point[1] <= std::numeric_limits<float>::max();
  }

  void failGeometry() {
    result_.commands.clear();
    result_.primitiveVertices = 0;
    result_.failure = diagnostic(
        "skin.renderer.geometry.invalid",
        "Projected timing visualizer geometry is outside float range.");
  }

  void append(SkinPrimitiveCommand primitive) {
    result_.primitiveVertices += primitive.vertices.size();
    result_.commands.push_back(
        {.authoredOrdinal = request_.authoredOrdinal,
         .sourceObject = request_.sourceObject,
         .payload = std::move(primitive)});
  }

  const SkinTimingVisualizerRenderRequest &request_;
  AuthoredDestinationGeometry geometry_;
  SkinTimingVisualizerRenderResult result_;
  std::optional<UiLogicalRect> clip_;
  bool clippedOut_ = false;
  double cosine_ = 1.0;
  double sine_ = 0.0;
  double pivotX_ = 0.0;
  double pivotY_ = 0.0;
};

} // namespace

SkinTimingVisualizerRenderResult
renderSkinTimingVisualizer(const SkinTimingVisualizerRenderRequest &request) {
  if (request.geometry.rgba[3] <= 0.0F || request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0) {
    return {};
  }
  if (request.visualizer.judgeWidthMillis < 0 ||
      request.visualizer.lineWidth < 1 || request.visualizer.lineWidth > 4) {
    return {.failure = diagnostic(
        "skin.renderer.timingvisualizer.invalid",
        "Timing visualizer has an invalid pinned geometry configuration.")};
  }
  const std::int64_t pixelWidth64 =
      static_cast<std::int64_t>(request.visualizer.judgeWidthMillis) * 2 + 1;
  if (pixelWidth64 <= 0 ||
      pixelWidth64 > std::numeric_limits<int>::max()) {
    return {.failure = diagnostic(
        "skin.renderer.geometry.invalid",
        "Timing visualizer judge width is outside the fixed geometry range.")};
  }
  const int pixelWidth = static_cast<int>(pixelWidth64);
  SkinGeneratedTextureRaster background(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Background,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .maximumCommands = request.maximumCommands,
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = pixelWidth,
       .sourceHeight = 1,
       .verticalFlip = false,
       .diagnosticObject = "Timing visualizer background",
       .cache = request.cache,
       .contentRevision = 1});
  if (!background.drawable()) return background.take();
  const std::int64_t center = request.visualizer.judgeWidthMillis;
  background.appendRectangle(static_cast<int>(center), 0, 1, 1,
                             request.visualizer.centerRgba);
  std::int64_t beforeX1 = center;
  std::int64_t beforeX2 = center + 1;
  const auto windows = request.state.judgeWindows;
  for (std::size_t i = 0; i < request.visualizer.judgeRgba.size() &&
                          i < windows.size();
       ++i) {
    const auto &window = windows[i];
    const std::int64_t x1 = center + std::clamp<std::int64_t>(
                                          window.minimumTimingMillis, -center,
                                          center);
    const std::int64_t x2 = center + std::clamp<std::int64_t>(
                                          window.maximumTimingMillis, -center,
                                          center) +
                            1;
    std::uint32_t color = request.visualizer.judgeRgba[i];
    if (i == request.visualizer.judgeRgba.size() - 1 &&
        request.visualizer.transparent) {
      color = 0U;
    }
    if (beforeX1 > x1)
      background.appendRectangle(static_cast<int>(x1), 0,
                                 static_cast<int>(beforeX1 - x1), 1, color);
    if (beforeX1 > x1) {
      beforeX1 = x1;
    }
    if (x2 > beforeX2)
      background.appendRectangle(static_cast<int>(beforeX2), 0,
                                 static_cast<int>(x2 - beforeX2), 1, color);
    if (x2 > beforeX2) {
      beforeX2 = x2;
    }
  }
  constexpr std::uint32_t gridColor = 0x0000003fU;
  const std::int64_t firstGrid = center % 10;
  for (std::int64_t pixel = firstGrid; pixel < pixelWidth;) {
    if (pixel >= 0)
      background.appendLine(static_cast<int>(pixel), 0,
                            static_cast<int>(pixel), 1, gridColor);
    if (pixel > std::numeric_limits<std::int64_t>::max() - 10) {
      break;
    }
    pixel += 10;
  }
  auto result = background.take();
  if (result.failure) return result;
  const auto recent = request.state.recentJudgeTimingsMillis;
  if (recent.empty()) {
    return result;
  }
  const std::size_t index = request.state.recentJudgeTimingIndex % recent.size();
  const std::uint32_t baseLineColor = rgbaToAbgr(request.visualizer.lineRgba);
  const std::uint8_t baseAlpha =
      static_cast<std::uint8_t>(baseLineColor >> 24U);
  auto lineRequest = request;
  lineRequest.maximumCommands -=
      std::min(lineRequest.maximumCommands, result.commands.size());
  PrimitiveBuilder builder(lineRequest, request.geometry, pixelWidth);
  const double judgeWidthRate =
      static_cast<double>(request.visualizer.width) /
      static_cast<double>(pixelWidth);
  for (std::size_t i = 0; i < recent.size(); ++i) {
    const std::size_t sampleIndex = (index + i + 1) % recent.size();
    const std::int64_t timing = recent[sampleIndex];
    if (timing == kSkinEmptyJudgeTimingMillis || timing < -center ||
        timing > center) {
      continue;
    }
    const auto alpha = static_cast<std::uint8_t>(
        static_cast<unsigned int>(baseAlpha) * (i + 1U) / 100U);
    const std::uint32_t color = (baseLineColor & 0x00ffffffU) |
                                (static_cast<std::uint32_t>(alpha) << 24U);
    const double x = request.geometry.rect.x +
                     (request.geometry.rect.width -
                      request.visualizer.lineWidth) /
                         2.0 +
                     static_cast<double>(timing) * judgeWidthRate;
    const double y = request.visualizer.drawDecay
                         ? request.geometry.rect.y + request.geometry.rect.height *
                               static_cast<double>(recent.size() - i) /
                               static_cast<double>(recent.size()) / 2.0
                         : request.geometry.rect.y;
    const double height = request.visualizer.drawDecay
                              ? request.geometry.rect.height *
                                    static_cast<double>(i) /
                                    static_cast<double>(recent.size())
                              : request.geometry.rect.height;
    if (!builder.appendQuad(x, y, request.visualizer.lineWidth, height,
                            color)) {
      auto failed = builder.take();
      if (failed.failure) return failed;
      return result;
    }
  }
  auto lines = builder.take();
  if (lines.failure) return lines;
  result.primitiveVertices = lines.primitiveVertices;
  result.commands.insert(result.commands.end(),
                         std::make_move_iterator(lines.commands.begin()),
                         std::make_move_iterator(lines.commands.end()));
  return result;
}

SkinTimingDistributionGraphRenderResult renderSkinTimingDistributionGraph(
    const SkinTimingDistributionGraphRenderRequest &request) {
  if (request.geometry.rgba[3] <= 0.0F || request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0 || request.state.timingDistribution.empty()) {
    return {};
  }
  const int width = std::max(1, request.graph.width);
  const int lineWidth = std::clamp(request.graph.lineWidth, 1, width);
  const int graphWidth = std::max(1, width / lineWidth);
  const int center = graphWidth / 2;
  const auto scaledX = [center, lineWidth](int millis) {
    return center + static_cast<int>(std::lround(
                        static_cast<double>(millis) / lineWidth));
  };
  const int sourceCenter = request.state.timingDistributionCenter;
  const auto aggregatedBucket = [&](int offset) {
    int value = 0;
    const int first = sourceCenter + offset * lineWidth;
    for (int column = 0; column < lineWidth; ++column) {
      const int source = first + column;
      if (source >= 0 && static_cast<std::size_t>(source) <
                             request.state.timingDistribution.size()) {
        value += request.state.timingDistribution[static_cast<std::size_t>(source)];
      }
    }
    return value;
  };
  int maximum = 10;
  for (int offset = -center; offset < graphWidth - center; ++offset) {
    const int value = aggregatedBucket(offset);
    if (maximum < value) maximum = value / 10 * 10 + 10;
  }
  std::uint64_t revision = 1469598103934665603ULL;
  for (const int value : request.state.timingDistribution) {
    revision ^= static_cast<std::uint32_t>(value);
    revision *= 1099511628211ULL;
  }
  revision ^= static_cast<std::uint64_t>(maximum);
  SkinGeneratedTextureRaster raster(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Background,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .maximumCommands = request.maximumCommands,
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = graphWidth,
       .sourceHeight = maximum,
       .verticalFlip = false,
       .diagnosticObject = "Timing distribution graph",
       .cache = request.cache,
       .contentRevision = revision == 0 ? 1 : revision});
  if (!raster.drawable()) return raster.take();

  raster.appendRectangle(center, 0, 1, maximum, request.graph.judgeRgba[0]);
  int beforeLeft = center;
  int beforeRight = center + 1;
  for (std::size_t index = 0; index < request.graph.judgeRgba.size() &&
                              index < request.state.judgeWindows.size();
       ++index) {
    const auto &window = request.state.judgeWindows[index];
    const int left = std::clamp(scaledX(window.minimumTimingMillis), 0,
                                graphWidth);
    const int right = std::clamp(scaledX(window.maximumTimingMillis) + 1, 0,
                                 graphWidth);
    if (beforeLeft > left) {
      raster.appendRectangle(left, 0, beforeLeft - left, maximum,
                             request.graph.judgeRgba[index]);
      beforeLeft = left;
    }
    if (right > beforeRight) {
      raster.appendRectangle(beforeRight, 0, right - beforeRight, maximum,
                             request.graph.judgeRgba[index]);
      beforeRight = right;
    }
  }
  constexpr std::uint32_t gridColor = 0x0000003fU;
  for (int x = center % 10; x < graphWidth; x += 10) {
    raster.appendLine(x, 0, x, 1, gridColor);
  }
  if (request.graph.drawAverage && request.state.timingDistributionAverageMillis) {
    const int x = scaledX(static_cast<int>(std::lround(
        *request.state.timingDistributionAverageMillis)));
    raster.appendLine(x, 0, x, maximum, request.graph.averageRgba);
  }
  if (request.graph.drawDev && request.state.timingDistributionAverageMillis &&
      request.state.timingDistributionStandardDeviationMillis) {
    const int average = static_cast<int>(std::round(
        *request.state.timingDistributionAverageMillis));
    const int deviation = static_cast<int>(std::round(
        *request.state.timingDistributionStandardDeviationMillis));
    raster.appendLine(scaledX(average + deviation), 0,
                      scaledX(average + deviation), maximum, request.graph.devRgba);
    raster.appendLine(scaledX(average - deviation), 0,
                      scaledX(average - deviation), maximum, request.graph.devRgba);
  }
  for (int offset = -center; offset < graphWidth - center; ++offset) {
    const int value = aggregatedBucket(offset);
    if (value > 0) {
      raster.appendRectangle(center + offset, maximum - value, 1, value,
                             request.graph.graphRgba);
    }
  }
  return raster.take();
}

} // namespace skin

#endif
