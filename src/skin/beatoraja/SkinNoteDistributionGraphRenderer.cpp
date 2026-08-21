#include "SkinNoteDistributionGraphRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>

namespace skin {
namespace {

constexpr std::array<std::array<std::uint32_t, 10>, 3> kGraphColors{{
    {0xff44ff44U, 0xff228822U, 0xff4444ffU, 0xffff4444U, 0xff882222U,
     0xffccccccU, 0xff000088U, 0U, 0U, 0U},
    {0xff555555U, 0xffff8800U, 0xff88ff00U, 0xff00ffffU, 0xff0088ffU,
     0xff0000ffU, 0U, 0U, 0U, 0U},
    {0xff555555U, 0xff44ff44U, 0xffff8800U, 0xffcc6600U, 0xff884400U,
     0xff442200U, 0xff0088ffU, 0xff0066ccU, 0xff004488U, 0xff002244U},
}};

constexpr std::array<std::array<std::uint32_t, 10>, 3> kPmsGraphColors{{
    kGraphColors[0],
    {0xff555555U, 0xffb05effU, 0xff32beffU, 0xff3c46dcU, 0xffffc66cU,
     0xffffc66cU, 0U, 0U, 0U, 0U},
    {0xff555555U, 0xffb05effU, 0xffff8800U, 0xffcc6600U, 0xff884400U,
     0xff442200U, 0xff0088ffU, 0xff0066ccU, 0xff004488U, 0xff002244U},
}};

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

class PrimitiveBuilder {
public:
  explicit PrimitiveBuilder(
      const SkinNoteDistributionGraphRenderRequest &request)
      : request_(request) {
    const auto projected = projectSkinDestinationToUi(
        request.geometry,
        {.textureWidth = 1,
         .textureHeight = 1,
         .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
        request.viewport);
    bool empty = false;
    clip_ = intersect(projected.clip,
                      projectedSkinScissorBounds(request.viewport), empty);
    clippedOut_ = empty;
    const double radians =
        request.geometry.angleDegrees * std::numbers::pi / 180.0;
    cosine_ = std::cos(radians);
    sine_ = std::sin(radians);
    pivotX_ = request.geometry.rect.x +
              request.geometry.centerX * request.geometry.rect.width;
    pivotY_ = request.geometry.rect.y +
              request.geometry.centerY * request.geometry.rect.height;
  }

  bool appendQuad(double x, double y, double width, double height,
                  std::uint32_t color) {
    if (clippedOut_ || width == 0.0 || height == 0.0) {
      return true;
    }
    constexpr std::size_t vertexCount = 4;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::SolidQuad;
    primitive.state = {.blend = request_.geometry.blend,
                       .filter = request_.geometry.filter,
                       .scissor = clip_};
    primitive.vertices.reserve(vertexCount);
    const std::array<std::array<double, 2>, 4> points{{
        {x, y},
        {x + width, y},
        {x + width, y + height},
        {x, y + height},
    }};
    for (const auto point : points) {
      const auto projected = project(point[0], point[1]);
      if (!finite(projected)) {
        failGeometry();
        return false;
      }
      primitive.vertices.push_back(
          {.x = static_cast<float>(projected[0]),
           .y = static_cast<float>(projected[1]),
           .rgba = modulatedColor(color, request_.geometry.rgba)});
    }
    append(std::move(primitive));
    return true;
  }

  bool appendLine(double x0, double y0, double x1, double y1,
                  std::uint32_t color) {
    if (clippedOut_) {
      return true;
    }
    constexpr std::size_t vertexCount = 2;
    if (!fits(vertexCount)) {
      return false;
    }
    SkinPrimitiveCommand primitive;
    primitive.kind = SkinPrimitiveKind::LineStrip;
    primitive.state = {.blend = request_.geometry.blend,
                       .filter = request_.geometry.filter,
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
           .rgba = modulatedColor(color, request_.geometry.rgba)});
    }
    append(std::move(primitive));
    return true;
  }

  SkinNoteDistributionGraphRenderResult take() { return std::move(result_); }

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
          "Note distribution graph exceeds the fixed frame limits.");
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
        "Projected note distribution graph geometry is outside float range.");
  }

  void append(SkinPrimitiveCommand primitive) {
    result_.primitiveVertices += primitive.vertices.size();
    result_.commands.push_back(
        {.authoredOrdinal = request_.authoredOrdinal,
         .sourceObject = request_.sourceObject,
         .payload = std::move(primitive)});
  }

  const SkinNoteDistributionGraphRenderRequest &request_;
  SkinNoteDistributionGraphRenderResult result_;
  std::optional<UiLogicalRect> clip_;
  bool clippedOut_ = false;
  double cosine_ = 1.0;
  double sine_ = 0.0;
  double pivotX_ = 0.0;
  double pivotY_ = 0.0;
};

int maximumHeight(auto data) noexcept {
  int maximum = 20;
  for (const auto &second : data) {
    std::int64_t count = 0;
    for (const int bucket : second) {
      count = std::min<std::int64_t>(
          1'000'000, count + std::max<std::int64_t>(bucket, 0));
    }
    if (maximum < count) {
      maximum = static_cast<int>(
          std::min<std::int64_t>((count / 10) * 10 + 10, 100));
    }
  }
  return maximum;
}

std::int64_t cursorPixel(std::int64_t millis, int pixelWidth,
                         std::size_t seconds) noexcept {
  const long double numerator = static_cast<long double>(millis) *
                                static_cast<long double>(pixelWidth);
  const long double denominator =
      static_cast<long double>(seconds) * 1000.0L;
  const long double value = numerator / denominator;
  if (value <= static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(value);
}

template <typename Distribution>
SkinNoteDistributionGraphRenderResult renderDistribution(
    const SkinNoteDistributionGraphRenderRequest &request,
    std::span<const Distribution> data) {
  PrimitiveBuilder builder(request);
  if (data.empty() || request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0) {
    return builder.take();
  }

  const int maximum = maximumHeight(data);
  if (data.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max()) / 5U) {
    auto result = builder.take();
    result.failure = diagnostic(
        "skin.renderer.geometry.invalid",
        "Note distribution graph duration exceeds the fixed geometry range.");
    return result;
  }
  const std::size_t pixelWidthSize = data.size() * 5U;
  const int pixelWidth = static_cast<int>(pixelWidthSize);
  const int pixelHeight = maximum * 5;
  const double backgroundScaleX =
      request.geometry.rect.width / static_cast<double>(pixelWidth);
  const double scaleY =
      request.geometry.rect.height / static_cast<double>(pixelHeight);

  if (!request.graph.backgroundTextureOff) {
    if (!builder.appendQuad(request.geometry.rect.x, request.geometry.rect.y,
                            request.geometry.rect.width,
                            request.geometry.rect.height, 0xcc000000U)) {
      return builder.take();
    }
    for (int row = 10; row < maximum; row += 10) {
      const auto component = static_cast<std::uint8_t>(0.007F * row * 255.0F);
      const std::uint32_t color = 0xff000000U |
                                  (static_cast<std::uint32_t>(component) << 8U) |
                                  static_cast<std::uint32_t>(component);
      if (!builder.appendQuad(
              request.geometry.rect.x,
              request.geometry.rect.y + row * 5.0 * scaleY,
              request.geometry.rect.width, 50.0 * scaleY, color)) {
        return builder.take();
      }
    }
    for (std::size_t second = 0; second < data.size(); second += 10) {
      const bool minute = second % 60 == 0;
      const std::uint32_t color = minute ? 0xff3f3f3fU : 0xff1f1f1fU;
      const double x = request.geometry.rect.x +
                       static_cast<double>(second * 5U) * backgroundScaleX;
      if (!builder.appendLine(x, request.geometry.rect.y, x,
                              request.geometry.rect.y +
                                  request.geometry.rect.height,
                              color)) {
        return builder.take();
      }
    }
  }

  float reveal = 1.0F;
  if (request.elapsedMillis < request.graph.delayMillis &&
      request.graph.delayMillis != 0) {
    reveal = static_cast<float>(request.elapsedMillis) /
             static_cast<float>(request.graph.delayMillis);
  }
  reveal = std::clamp(reveal, 0.0F, 1.0F);
  const int revealedPixels = static_cast<int>(
      static_cast<float>(pixelWidth) * reveal);
  if (revealedPixels > 0) {
    const double revealedWidth =
        request.geometry.rect.width * static_cast<double>(reveal);
    const double shapeScaleX =
        revealedWidth / static_cast<double>(revealedPixels);
    const auto typeIndex = static_cast<std::size_t>(request.graph.type);
    const auto &palette = request.pmsMode ? kPmsGraphColors[typeIndex]
                                         : kGraphColors[typeIndex];
    const int authoredCellWidth = 4 + (request.graph.noHorizontalGap ? 1 : 0);
    const int authoredCellHeight = 4 + (request.graph.noGap ? 1 : 0);
    for (std::size_t second = 0; second < data.size(); ++second) {
      const int cellX = static_cast<int>(second * 5U);
      if (cellX >= revealedPixels) {
        break;
      }
      const int visibleCellWidth =
          std::min(authoredCellWidth, revealedPixels - cellX);
      int stack = 0;
      const auto emitBucket = [&](std::size_t bucket) {
        int count = std::max(data[second][bucket], 0);
        while (count-- > 0 && stack < maximum) {
          if (!builder.appendQuad(
                  request.geometry.rect.x + cellX * shapeScaleX,
                  request.geometry.rect.y + stack * 5.0 * scaleY,
                  visibleCellWidth * shapeScaleX,
                  authoredCellHeight * scaleY, palette[bucket])) {
            return false;
          }
          ++stack;
        }
        return true;
      };
      if (!request.graph.reverseOrder) {
        for (std::size_t bucket = 0; bucket < data[second].size(); ++bucket) {
          if (!emitBucket(bucket)) {
            return builder.take();
          }
        }
      } else {
        for (std::size_t bucket = data[second].size(); bucket-- > 0;) {
          if (!emitBucket(bucket)) {
            return builder.take();
          }
        }
      }
    }
  }

  const auto appendCursor = [&](std::optional<std::int64_t> millis,
                                std::uint32_t color) {
    if (!millis) {
      return true;
    }
    const std::int64_t cursor = cursorPixel(*millis, pixelWidth, data.size());
    const std::int64_t first = std::max<std::int64_t>(cursor, 0);
    const std::int64_t last =
        std::min<std::int64_t>(cursor > std::numeric_limits<std::int64_t>::max() - 3
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : cursor + 3,
                              pixelWidth);
    if (last <= first) {
      return true;
    }
    return builder.appendQuad(
        request.geometry.rect.x + static_cast<double>(first) * backgroundScaleX,
        request.geometry.rect.y,
        static_cast<double>(last - first) * backgroundScaleX,
        request.geometry.rect.height, color);
  };
  if (!appendCursor(request.startMillis, 0x80ff80ffU) ||
      !appendCursor(request.endMillis, 0xff8080ffU) ||
      !appendCursor(request.currentMillis, 0xffffffffU)) {
    return builder.take();
  }
  return builder.take();
}

} // namespace

SkinNoteDistributionGraphRenderResult renderSkinNoteDistributionGraph(
    const SkinNoteDistributionGraphRenderRequest &request) {
  switch (request.graph.type) {
  case SkinNoteDistributionGraphType::Normal:
    return renderDistribution(request, request.state.normalDistribution);
  case SkinNoteDistributionGraphType::Judge:
    return renderDistribution(request, request.state.judgementDistribution);
  case SkinNoteDistributionGraphType::EarlyLate:
    return renderDistribution(request, request.state.earlyLateDistribution);
  }
  return {.failure = diagnostic("skin.renderer.judgegraph.invalid",
                                "Note distribution graph type is invalid.")};
}

} // namespace skin

#endif
