#pragma once

#include "PlaySkinViewport.h"
#include "SkinDestinationEvaluator.h"
#include "SkinDrawCommand.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {

struct SkinGeneratedTextureRasterRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::int64_t elapsedMillis = 0;
  int revealMillis = 0;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
  std::optional<float> sourceRevealWidth;
  std::string_view diagnosticObject;
};

struct SkinGeneratedTextureRasterResult {
  std::vector<SkinDrawCommand> commands;
  std::size_t primitiveVertices = 0;
  std::optional<SkinDiagnostic> failure;
};

[[nodiscard]] inline int skinGeneratedTextureJavaInt(double value) noexcept {
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

[[nodiscard]] inline int skinGeneratedTextureJavaAdd(int left,
                                                     int right) noexcept {
  const auto sum = static_cast<std::uint32_t>(left) +
                   static_cast<std::uint32_t>(right);
  return std::bit_cast<std::int32_t>(sum);
}

class SkinGeneratedTextureRaster {
public:
  explicit SkinGeneratedTextureRaster(
      const SkinGeneratedTextureRasterRequest &request)
      : request_(request),
        textureWidth_(skinGeneratedTextureJavaInt(
            std::abs(request.geometry.rect.width))),
        textureHeight_(skinGeneratedTextureJavaInt(
            std::abs(request.geometry.rect.height))) {
    if (request.geometry.rgba[3] <= 0.0F || textureWidth_ <= 0 ||
        textureHeight_ <= 0 || request.revealMillis < 0) {
      return;
    }
    const float reveal = request.elapsedMillis >= request.revealMillis
                             ? 1.0F
                             : static_cast<float>(request.elapsedMillis) /
                                   static_cast<float>(request.revealMillis);
    const float sourceRevealWidth = request.sourceRevealWidth.value_or(
        static_cast<float>(textureWidth_));
    const int visibleWidth = skinGeneratedTextureJavaInt(
        static_cast<double>(sourceRevealWidth * reveal));
    const int drawWidth = skinGeneratedTextureJavaInt(
        static_cast<double>(static_cast<float>(request.geometry.rect.width) *
                            reveal));
    if (visibleWidth == 0 || drawWidth == 0) {
      return;
    }
    source_ = {
        .textureWidth = textureWidth_,
        .textureHeight = textureHeight_,
        .region = {.x = 0, .y = 0, .w = visibleWidth, .h = textureHeight_},
    };
    auto drawGeometry = request.geometry;
    drawGeometry.rect = {.x = request.geometry.rect.x,
                         .y = request.geometry.rect.y +
                              request.geometry.rect.height,
                         .width = static_cast<double>(drawWidth),
                         .height = -request.geometry.rect.height};
    stretched_ = stretchSkinDestinationAuthored(drawGeometry, source_);
    if (stretched_.rect.width == 0.0 || stretched_.rect.height == 0.0 ||
        stretched_.region.w == 0 || stretched_.region.h == 0) {
      return;
    }

    const auto projected = projectSkinDestinationToUi(
        request.geometry,
        {.textureWidth = textureWidth_,
         .textureHeight = textureHeight_,
         .region = {.x = 0,
                    .y = 0,
                    .w = textureWidth_,
                    .h = textureHeight_}},
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
    drawable_ = true;
  }

  [[nodiscard]] bool drawable() const noexcept { return drawable_; }
  [[nodiscard]] int textureWidth() const noexcept { return textureWidth_; }
  [[nodiscard]] int textureHeight() const noexcept { return textureHeight_; }

  bool appendRectangle(int x, int y, int width, int height,
                       std::uint32_t rgba) {
    if (!drawable_ || clippedOut_ || width <= 0 || height <= 0 ||
        source_.region.w == 0 || source_.region.h == 0) {
      return true;
    }
    const double regionLeft = std::min<double>(
        stretched_.region.x, stretched_.region.x + stretched_.region.w);
    const double regionTop = std::min<double>(
        stretched_.region.y, stretched_.region.y + stretched_.region.h);
    const double regionRight = std::max<double>(
        stretched_.region.x, stretched_.region.x + stretched_.region.w);
    const double regionBottom = std::max<double>(
        stretched_.region.y, stretched_.region.y + stretched_.region.h);
    const double left =
        std::max({static_cast<double>(x), 0.0, regionLeft});
    const double top =
        std::max({static_cast<double>(y), 0.0, regionTop});
    const double right = std::min({static_cast<double>(x) + width,
                                   static_cast<double>(textureWidth_),
                                   regionRight});
    const double bottom = std::min({static_cast<double>(y) + height,
                                    static_cast<double>(textureHeight_),
                                    regionBottom});
    if (right <= left || bottom <= top) {
      return true;
    }

    const std::uint32_t color = modulatedColor(rgbaToAbgr(rgba));
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

  [[nodiscard]] SkinGeneratedTextureRasterResult take() {
    return std::move(result_);
  }

private:
  static std::optional<UiLogicalRect>
  intersect(const std::optional<UiLogicalRect> &clip,
            const UiLogicalRect &bounds, bool &empty) noexcept {
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
    const double right =
        std::min(clip->x + clip->width, bounds.x + bounds.width);
    const double bottom =
        std::min(clip->y + clip->height, bounds.y + bounds.height);
    if (right <= left || bottom <= top) {
      empty = true;
      return std::nullopt;
    }
    return UiLogicalRect{
        .x = left, .y = top, .width = right - left, .height = bottom - top};
  }

  static std::uint8_t modulatedByte(std::uint8_t source,
                                    float modulation) noexcept {
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<float>(source) * modulation, 0.0F, 255.0F));
  }

  [[nodiscard]] std::uint32_t
  modulatedColor(std::uint32_t abgr) const noexcept {
    const auto &rgba = request_.geometry.rgba;
    const auto red = modulatedByte(static_cast<std::uint8_t>(abgr), rgba[0]);
    const auto green = modulatedByte(
        static_cast<std::uint8_t>(abgr >> 8U), rgba[1]);
    const auto blue = modulatedByte(
        static_cast<std::uint8_t>(abgr >> 16U), rgba[2]);
    const auto alpha = modulatedByte(
        static_cast<std::uint8_t>(abgr >> 24U), rgba[3]);
    return static_cast<std::uint32_t>(red) |
           (static_cast<std::uint32_t>(green) << 8U) |
           (static_cast<std::uint32_t>(blue) << 16U) |
           (static_cast<std::uint32_t>(alpha) << 24U);
  }

  static std::uint32_t rgbaToAbgr(std::uint32_t rgba) noexcept {
    return ((rgba >> 24U) & 0xffU) | ((rgba >> 8U) & 0x0000ff00U) |
           ((rgba << 8U) & 0x00ff0000U) |
           ((rgba << 24U) & 0xff000000U);
  }

  bool fits(std::size_t vertices) {
    if (result_.commands.size() >= request_.maximumCommands ||
        vertices > request_.maximumPrimitiveVertices -
                       std::min(result_.primitiveVertices,
                                request_.maximumPrimitiveVertices)) {
      result_.commands.clear();
      result_.primitiveVertices = 0;
      result_.failure = SkinDiagnostic{
          .code = "skin.renderer.command.limit",
          .message = std::string(request_.diagnosticObject) +
                     " exceeds the fixed frame command or vertex limits.",
          .severity = DiagnosticSeverity::Error};
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
      result_.failure = SkinDiagnostic{
          .code = "skin.renderer.geometry.invalid",
          .message = "Projected " + std::string(request_.diagnosticObject) +
                     " geometry is outside float range.",
          .severity = DiagnosticSeverity::Error};
      return false;
    }
    primitive.vertices.push_back({.x = static_cast<float>(projected[0]),
                                  .y = static_cast<float>(projected[1]),
                                  .rgba = color});
    return true;
  }

  SkinGeneratedTextureRasterRequest request_;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  SkinSourceRegionGeometry source_{};
  SkinStretchedDestinationGeometry stretched_{};
  SkinGeneratedTextureRasterResult result_;
  std::optional<UiLogicalRect> clip_;
  bool drawable_ = false;
  bool clippedOut_ = false;
  double cosine_ = 1.0;
  double sine_ = 0.0;
  double pivotX_ = 0.0;
  double pivotY_ = 0.0;
};

} // namespace skin
