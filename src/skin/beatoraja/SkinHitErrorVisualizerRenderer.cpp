#include "SkinHitErrorVisualizerRenderer.h"

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

struct Point {
  double x = 0.0;
  double y = 0.0;
};

class PrimitiveBuilder {
public:
  PrimitiveBuilder(const SkinHitErrorVisualizerRenderRequest &request,
                   AuthoredDestinationGeometry geometry, int sourceWidth,
                   int sourceHeight)
      : request_(request), geometry_(std::move(geometry)) {
    const auto projected = projectSkinDestinationToUi(
        geometry_,
        {.textureWidth = sourceWidth,
         .textureHeight = sourceHeight,
         .region = {.x = 0, .y = 0, .w = sourceWidth, .h = sourceHeight}},
        request_.viewport);
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
    color = modulatedColor(color, geometry_.rgba);
    if (clippedOut_ || width == 0.0 || height == 0.0 ||
        color >> 24U == 0U) {
      return true;
    }
    constexpr std::size_t vertexCount = 4;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::SolidQuad;
    primitive.state = {.blend = geometry_.blend,
                       .filter = geometry_.filter,
                       .scissor = clip_};
    primitive.vertices.reserve(vertexCount);
    currentColor_ = color;
    for (const Point point : std::array<Point, 4>{
             Point{x, y}, Point{x + width, y}, Point{x + width, y + height},
             Point{x, y + height}}) {
      if (!appendVertex(primitive, point)) {
        return false;
      }
    }
    append(std::move(primitive));
    return true;
  }

  bool appendTriangle(Point first, Point second, Point third,
                      std::uint32_t color) {
    color = modulatedColor(color, geometry_.rgba);
    if (clippedOut_ || color >> 24U == 0U) {
      return true;
    }
    constexpr std::size_t vertexCount = 3;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::TriangleStrip;
    primitive.state = {.blend = geometry_.blend,
                       .filter = geometry_.filter,
                       .scissor = clip_};
    primitive.vertices.reserve(vertexCount);
    currentColor_ = color;
    if (!appendVertex(primitive, first) || !appendVertex(primitive, second) ||
        !appendVertex(primitive, third)) {
      return false;
    }
    append(std::move(primitive));
    return true;
  }

  SkinHitErrorVisualizerRenderResult take() { return std::move(result_); }

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
          "Hit-error visualizer exceeds the fixed frame limits.");
      return false;
    }
    return true;
  }

  bool appendVertex(SkinPrimitiveCommand &primitive, Point point) {
    const double relativeX = point.x - pivotX_;
    const double relativeY = point.y - pivotY_;
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
          "Projected hit-error visualizer geometry is outside float range.");
      return false;
    }
    primitive.vertices.push_back(
        {.x = static_cast<float>(projected[0]),
         .y = static_cast<float>(projected[1]),
         .rgba = currentColor_});
    return true;
  }

  void append(SkinPrimitiveCommand primitive) {
    result_.primitiveVertices += primitive.vertices.size();
    result_.commands.push_back(
        {.authoredOrdinal = request_.authoredOrdinal,
         .sourceObject = request_.sourceObject,
         .payload = std::move(primitive)});
  }

private:
  const SkinHitErrorVisualizerRenderRequest &request_;
  AuthoredDestinationGeometry geometry_;
  SkinHitErrorVisualizerRenderResult result_;
  std::optional<UiLogicalRect> clip_;
  bool clippedOut_ = false;
  double cosine_ = 1.0;
  double sine_ = 0.0;
  double pivotX_ = 0.0;
  double pivotY_ = 0.0;
  std::uint32_t currentColor_ = 0xffffffffU;
};

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
  if (request.visualizer.width <= 0 ||
      request.visualizer.judgeWidthMillis < 0 ||
      request.visualizer.lineWidth < 1 || request.visualizer.lineWidth > 4 ||
      request.visualizer.windowLength < 1 ||
      request.visualizer.windowLength > 100) {
    return {.failure = diagnostic(
                "skin.renderer.hiterrorvisualizer.invalid",
                "Hit-error visualizer has an invalid pinned geometry "
                "configuration.")};
  }
  const std::int64_t denominator =
      static_cast<std::int64_t>(request.visualizer.judgeWidthMillis) * 2 + 1;
  if (denominator <= 0) {
    return {.failure = diagnostic(
                "skin.renderer.geometry.invalid",
                "Hit-error visualizer judge width is outside the fixed "
                "geometry range.")};
  }
  const int sourceWidth = request.visualizer.width;
  const int sourceHeight = request.visualizer.windowLength * 2;
  const auto stretched = stretchSkinDestinationAuthored(
      request.geometry,
      {.textureWidth = sourceWidth,
       .textureHeight = sourceHeight,
       .region = {.x = 0, .y = 0, .w = sourceWidth, .h = sourceHeight}});
  if (stretched.rect.width == 0.0 || stretched.rect.height == 0.0 ||
      stretched.region.w <= 0 || stretched.region.h <= 0) {
    return {};
  }
  auto geometry = request.geometry;
  geometry.rect = stretched.rect;
  geometry.stretch = SkinStretchMode::Stretch;
  PrimitiveBuilder builder(request, geometry, sourceWidth, sourceHeight);
  const double cropLeft = stretched.region.x;
  const double cropBottom = stretched.region.y;
  const double cropRight = cropLeft + stretched.region.w;
  const double cropTop = cropBottom + stretched.region.h;
  const double scaleX =
      stretched.rect.width / static_cast<double>(stretched.region.w);
  const double scaleY =
      stretched.rect.height / static_cast<double>(stretched.region.h);
  const auto destinationPoint = [&](Point source) {
    return Point{stretched.rect.x + (source.x - cropLeft) * scaleX,
                 stretched.rect.y + (source.y - cropBottom) * scaleY};
  };
  const auto appendQuad = [&](double x, double y, double width, double height,
                              std::uint32_t rgba) {
    const double left = std::max(x, cropLeft);
    const double bottom = std::max(y, cropBottom);
    const double right = std::min(x + width, cropRight);
    const double top = std::min(y + height, cropTop);
    if (right <= left || top <= bottom || (rgba & 0xffU) == 0U) {
      return true;
    }
    const std::uint32_t color = rgbaToAbgr(rgba);
    return builder.appendQuad(
        stretched.rect.x + (left - cropLeft) * scaleX,
        stretched.rect.y + (bottom - cropBottom) * scaleY,
        (right - left) * scaleX, (top - bottom) * scaleY, color);
  };

  const auto clipPolygon = [&](std::vector<Point> polygon) {
    const auto clipEdge = [](std::vector<Point> input, auto inside,
                             auto intersection) {
      std::vector<Point> output;
      if (input.empty()) {
        return output;
      }
      Point previous = input.back();
      bool previousInside = inside(previous);
      for (const Point current : input) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
          output.push_back(intersection(previous, current));
        }
        if (currentInside) {
          output.push_back(current);
        }
        previous = current;
        previousInside = currentInside;
      }
      return output;
    };
    polygon = clipEdge(
        std::move(polygon), [&](Point value) { return value.x >= cropLeft; },
        [&](Point a, Point b) {
          const double t = (cropLeft - a.x) / (b.x - a.x);
          return Point{cropLeft, a.y + (b.y - a.y) * t};
        });
    polygon = clipEdge(
        std::move(polygon), [&](Point value) { return value.x <= cropRight; },
        [&](Point a, Point b) {
          const double t = (cropRight - a.x) / (b.x - a.x);
          return Point{cropRight, a.y + (b.y - a.y) * t};
        });
    polygon = clipEdge(
        std::move(polygon), [&](Point value) { return value.y >= cropBottom; },
        [&](Point a, Point b) {
          const double t = (cropBottom - a.y) / (b.y - a.y);
          return Point{a.x + (b.x - a.x) * t, cropBottom};
        });
    return clipEdge(
        std::move(polygon), [&](Point value) { return value.y <= cropTop; },
        [&](Point a, Point b) {
          const double t = (cropTop - a.y) / (b.y - a.y);
          return Point{a.x + (b.x - a.x) * t, cropTop};
        });
  };

  const auto appendTriangle = [&](Point first, Point second, Point third,
                                  std::uint32_t rgba) {
    if ((rgba & 0xffU) == 0U) {
      return true;
    }
    auto polygon = clipPolygon({first, second, third});
    if (polygon.size() < 3) {
      return true;
    }
    const std::uint32_t color = rgbaToAbgr(rgba);
    for (std::size_t index = 1; index + 1 < polygon.size(); ++index) {
      if (!builder.appendTriangle(destinationPoint(polygon[0]),
                                  destinationPoint(polygon[index]),
                                  destinationPoint(polygon[index + 1]),
                                  color)) {
        return false;
      }
    }
    return true;
  };

  const auto recent = request.state.recentJudgeTimingsMillis;
  const int center = request.visualizer.judgeWidthMillis;
  const float judgeWidthRate =
      static_cast<float>(request.visualizer.width) /
      static_cast<float>(denominator);
  const int centerX =
      (request.visualizer.width - request.visualizer.lineWidth) / 2;
  const auto sourceX = [&](std::int64_t timing) {
    const auto clamped = std::clamp<std::int64_t>(timing, -center, center);
    return centerX +
           static_cast<int>(static_cast<float>(clamped) * -judgeWidthRate);
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
      if (!appendQuad(sourceX(timing), y, request.visualizer.lineWidth,
                      height, color)) {
        return builder.take();
      }
    }
  }

  if (!appendQuad(centerX, 0, request.visualizer.lineWidth, sourceHeight,
                  request.visualizer.centerRgba)) {
    return builder.take();
  }

  if (request.visualizer.emaMode != 0) {
    int x = sourceX(request.emaMillis);
    if ((request.visualizer.emaMode == 1 ||
         request.visualizer.emaMode == 3) &&
        !appendQuad(x, 0, request.visualizer.lineWidth, sourceHeight,
                    request.visualizer.emaRgba)) {
      return builder.take();
    }
    if (request.visualizer.emaMode == 2 || request.visualizer.emaMode == 3) {
      x += request.visualizer.lineWidth / 2;
      int width = static_cast<int>(request.visualizer.width * 0.01);
      if (width % 2 != 0) {
        ++width;
      }
      if (!appendTriangle({static_cast<double>(x),
                           static_cast<double>(sourceHeight / 3)},
                          {static_cast<double>(x + width), 0.0},
                          {static_cast<double>(x - width), 0.0},
                          request.visualizer.emaRgba)) {
        return builder.take();
      }
    }
  }
  return builder.take();
}

} // namespace skin

#endif
