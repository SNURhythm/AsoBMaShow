#include "SkinDestinationEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace skin {
namespace {

constexpr std::array<double, 10> kCenterX = {0.5, 0.0, 0.5, 1.0, 0.0,
                                             0.5, 1.0, 0.0, 0.5, 1.0};
constexpr std::array<double, 10> kCenterY = {0.5, 0.0, 0.0, 0.0, 0.5,
                                             0.5, 0.5, 1.0, 1.0, 1.0};

SkinDiagnostic diagnostic(std::string code, std::string message) {
  return {.code = std::move(code), .message = std::move(message)};
}

bool finite(const SkinDestinationFrame &frame) {
  return std::isfinite(frame.x) && std::isfinite(frame.y) &&
         std::isfinite(frame.width) && std::isfinite(frame.height) &&
         std::isfinite(frame.angleDegrees);
}

std::int64_t subtractMillis(std::int64_t now, std::int64_t start) {
  const auto value =
      static_cast<__int128>(now / 1000) - static_cast<__int128>(start / 1000);
  if (value > std::numeric_limits<std::int64_t>::max()) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value < std::numeric_limits<std::int64_t>::min()) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

float easedRate(float rate, int acceleration) {
  switch (acceleration) {
  case 1:
    return rate * rate;
  case 2:
    return 1.0 - (rate - 1.0) * (rate - 1.0);
  default:
    return rate;
  }
}

int objectAcceleration(const SkinDestinationBody &destination) {
  int acceleration = 0;
  for (const auto &frame : destination.frames) {
    if (acceleration == 0) {
      acceleration = frame.acceleration;
    }
  }
  return acceleration;
}

bool hasFixedColor(const SkinDestinationBody &destination) {
  const auto &first = destination.frames.front().rgba;
  return std::all_of(destination.frames.begin() + 1, destination.frames.end(),
                     [&first](const SkinDestinationFrame &frame) {
                       return frame.rgba == first;
                     });
}

double interpolate(double lower, double upper, double rate) {
  return lower + (upper - lower) * rate;
}

void applyRectOffset(AuthoredRect &rect, const ConfigOffset &offset) {
  rect.x += static_cast<double>(offset.x) - static_cast<double>(offset.w) / 2.0;
  rect.y += static_cast<double>(offset.y) - static_cast<double>(offset.h) / 2.0;
  rect.width += offset.w;
  rect.height += offset.h;
}

std::array<double, 2> apply(const Affine2D &affine, double x, double y) {
  return {affine.m00 * x + affine.m01 * y + affine.tx,
          affine.m10 * x + affine.m11 * y + affine.ty};
}

void fitWidth(AuthoredRect &rect, double width) {
  const double center = rect.x + rect.width / 2.0;
  rect.width = width;
  rect.x = center - width / 2.0;
}

void fitHeight(AuthoredRect &rect, double height) {
  const double center = rect.y + rect.height / 2.0;
  rect.height = height;
  rect.y = center - height / 2.0;
}

int truncateJava(double value) {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  if (value <= static_cast<double>(std::numeric_limits<int>::min())) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(std::trunc(value));
}

double regionWidth(const SkinSourceRect &region) {
  return std::abs(static_cast<double>(region.w));
}

double regionHeight(const SkinSourceRect &region) {
  return std::abs(static_cast<double>(region.h));
}

void setTrimmedRegionWidth(SkinSourceRect &region, double width) {
  const double farEdge =
      static_cast<double>(region.x) + static_cast<double>(region.w);
  const int newWidth = truncateJava(width);
  const int newX = truncateJava(static_cast<double>(region.x) +
                                regionWidth(region) * 0.5 - width * 0.5);
  // TextureRegion.setRegionX changes only U. setRegionWidth then preserves
  // whichever orientation remains relative to the unchanged U2 endpoint.
  if (static_cast<double>(newX) > farEdge) {
    region.x = truncateJava(farEdge + static_cast<double>(newWidth));
    region.w = truncateJava(-static_cast<double>(newWidth));
  } else {
    region.x = newX;
    region.w = newWidth;
  }
}

void setTrimmedRegionHeight(SkinSourceRect &region, double height) {
  const double farEdge =
      static_cast<double>(region.y) + static_cast<double>(region.h);
  const int newHeight = truncateJava(height);
  const int newY = truncateJava(static_cast<double>(region.y) +
                                regionHeight(region) * 0.5 - height * 0.5);
  if (static_cast<double>(newY) > farEdge) {
    region.y = truncateJava(farEdge + static_cast<double>(newHeight));
    region.h = truncateJava(-static_cast<double>(newHeight));
  } else {
    region.y = newY;
    region.h = newHeight;
  }
}

void fitWidthTrimmed(AuthoredRect &rect, double scale, SkinSourceRect &region) {
  const double target = scale * regionWidth(region);
  if (rect.width < target) {
    const double crop = rect.width / scale;
    setTrimmedRegionWidth(region, crop);
  } else {
    fitWidth(rect, target);
  }
}

void fitHeightTrimmed(AuthoredRect &rect, double scale,
                      SkinSourceRect &region) {
  const double target = scale * regionHeight(region);
  if (rect.height < target) {
    const double crop = rect.height / scale;
    setTrimmedRegionHeight(region, crop);
  } else {
    fitHeight(rect, target);
  }
}

void applyStretch(AuthoredRect &rect, SkinSourceRect &region,
                  SkinStretchMode stretch) {
  // LibGDX TextureRegion keeps signed UV endpoints for flips but reports
  // absolute intrinsic dimensions to StretchType.
  const double sourceWidth = regionWidth(region);
  const double sourceHeight = regionHeight(region);
  switch (stretch) {
  case SkinStretchMode::Stretch:
    break;
  case SkinStretchMode::KeepAspectRatioFitInner: {
    const double sx = rect.width / sourceWidth;
    const double sy = rect.height / sourceHeight;
    if (sx <= sy) {
      fitHeight(rect, sourceHeight * sx);
    } else {
      fitWidth(rect, sourceWidth * sy);
    }
    break;
  }
  case SkinStretchMode::KeepAspectRatioFitOuter: {
    const double sx = rect.width / sourceWidth;
    const double sy = rect.height / sourceHeight;
    if (sx >= sy) {
      fitHeight(rect, sourceHeight * sx);
    } else {
      fitWidth(rect, sourceWidth * sy);
    }
    break;
  }
  case SkinStretchMode::KeepAspectRatioFitOuterTrimmed: {
    const double sx = rect.width / sourceWidth;
    const double sy = rect.height / sourceHeight;
    if (sx >= sy) {
      fitHeightTrimmed(rect, sx, region);
    } else {
      fitWidthTrimmed(rect, sy, region);
    }
    break;
  }
  case SkinStretchMode::KeepAspectRatioFitWidth:
    fitHeight(rect, sourceHeight * rect.width / sourceWidth);
    break;
  case SkinStretchMode::KeepAspectRatioFitWidthTrimmed:
    fitHeightTrimmed(rect, rect.width / sourceWidth, region);
    break;
  case SkinStretchMode::KeepAspectRatioFitHeight:
    fitWidth(rect, sourceWidth * rect.height / sourceHeight);
    break;
  case SkinStretchMode::KeepAspectRatioFitHeightTrimmed:
    fitWidthTrimmed(rect, rect.height / sourceHeight, region);
    break;
  case SkinStretchMode::KeepAspectRatioNoExpanding: {
    const double scale = std::min(
        1.0, std::min(rect.width / sourceWidth, rect.height / sourceHeight));
    fitWidth(rect, sourceWidth * scale);
    fitHeight(rect, sourceHeight * scale);
    break;
  }
  case SkinStretchMode::NoResize:
    fitWidth(rect, sourceWidth);
    fitHeight(rect, sourceHeight);
    break;
  case SkinStretchMode::NoResizeTrimmed:
    fitWidthTrimmed(rect, 1.0, region);
    fitHeightTrimmed(rect, 1.0, region);
    break;
  }
}

} // namespace

SkinDestinationEvaluationResult
evaluateSkinDestinationAuthored(const SkinDestinationBody &destination,
                                const SkinDestinationEvaluationInputs &inputs) {
  SkinDestinationEvaluationResult result;
  const std::size_t expectedConditions =
      destination.conditions.size() + (destination.drawCondition ? 1U : 0U);
  if (inputs.optionConditions.size() != expectedConditions) {
    result.diagnostics.push_back(diagnostic(
        "skin.destination.conditions.length_mismatch",
        "Resolved condition count does not match destination conditions."));
    return result;
  }
  if (std::any_of(inputs.optionConditions.begin(),
                  inputs.optionConditions.end(),
                  [](bool value) { return !value; })) {
    return result;
  }
  if (destination.frames.empty()) {
    result.diagnostics.push_back(diagnostic("skin.destination.frames.empty",
                                            "Destination has no frames."));
    return result;
  }
  for (std::size_t index = 0; index < destination.frames.size(); ++index) {
    if (!finite(destination.frames[index]) ||
        (index > 0 && destination.frames[index - 1].timeMillis >
                          destination.frames[index].timeMillis)) {
      result.diagnostics.push_back(
          diagnostic("skin.destination.frames.invalid",
                     "Destination frames must be finite and sorted by time."));
      return result;
    }
  }
  if (destination.timer && inputs.timerOff) {
    return result;
  }

  std::int64_t timeMillis =
      destination.timer
          ? subtractMillis(inputs.nowMicros, inputs.timerStartMicros)
          : inputs.nowMicros / 1000;
  const std::int64_t start = destination.frames.front().timeMillis;
  const std::int64_t end = destination.frames.back().timeMillis;
  if (destination.loop == -1) {
    if (timeMillis > end) {
      timeMillis = -1;
    }
  } else if (end > 0 && timeMillis > destination.loop) {
    if (end == destination.loop) {
      timeMillis = destination.loop;
    } else {
      timeMillis = (timeMillis - destination.loop) % (end - destination.loop) +
                   destination.loop;
    }
  }
  if (start > timeMillis) {
    return result;
  }

  std::size_t lowerIndex = 0;
  float rate = 0.0F;
  if (timeMillis == end) {
    lowerIndex = destination.frames.size() - 1;
  } else {
    std::int64_t upperTime = end;
    for (std::size_t reverse = destination.frames.size() - 1; reverse > 0;
         --reverse) {
      const std::size_t index = reverse - 1;
      const auto lowerTime =
          static_cast<std::int64_t>(destination.frames[index].timeMillis);
      if (lowerTime <= timeMillis && upperTime > timeMillis) {
        lowerIndex = index;
        rate = static_cast<float>(timeMillis - lowerTime) /
               static_cast<float>(upperTime - lowerTime);
        break;
      }
      upperTime = lowerTime;
    }
  }
  const int acceleration = objectAcceleration(destination);
  const bool fixedColor = hasFixedColor(destination);
  rate = easedRate(rate, acceleration);
  const bool interpolated =
      lowerIndex + 1 < destination.frames.size() && rate != 0.0F;
  const auto &lower = destination.frames[lowerIndex];
  const auto &upper =
      destination
          .frames[std::min(lowerIndex + 1, destination.frames.size() - 1)];

  AuthoredDestinationGeometry geometry;
  if (!interpolated || acceleration == 3) {
    geometry.rect = {.x = lower.x,
                     .y = lower.y,
                     .width = lower.width,
                     .height = lower.height};
    geometry.angleDegrees = lower.angleDegrees;
    for (std::size_t index = 0; index < geometry.rgba.size(); ++index) {
      geometry.rgba[index] = static_cast<float>(lower.rgba[index]) / 255.0F;
    }
    if (lower.clip) {
      geometry.clip =
          AuthoredRect{.x = static_cast<double>(lower.clip->x),
                       .y = static_cast<double>(lower.clip->y),
                       .width = static_cast<double>(lower.clip->w),
                       .height = static_cast<double>(lower.clip->h)};
    }
  } else {
    geometry.rect = {.x = interpolate(lower.x, upper.x, rate),
                     .y = interpolate(lower.y, upper.y, rate),
                     .width = interpolate(lower.width, upper.width, rate),
                     .height = interpolate(lower.height, upper.height, rate)};
    geometry.angleDegrees =
        truncateJava(interpolate(lower.angleDegrees, upper.angleDegrees, rate));
    for (std::size_t index = 0; index < geometry.rgba.size(); ++index) {
      const float low = static_cast<float>(lower.rgba[index]) / 255.0F;
      const float high = static_cast<float>(upper.rgba[index]) / 255.0F;
      geometry.rgba[index] = low + (high - low) * static_cast<float>(rate);
    }
    if (lower.clip) {
      if (upper.clip) {
        geometry.clip = AuthoredRect{
            .x = interpolate(lower.clip->x, upper.clip->x, rate),
            .y = interpolate(lower.clip->y, upper.clip->y, rate),
            .width = interpolate(lower.clip->w, upper.clip->w, rate),
            .height = interpolate(lower.clip->h, upper.clip->h, rate)};
      } else {
        geometry.clip =
            AuthoredRect{.x = static_cast<double>(lower.clip->x),
                         .y = static_cast<double>(lower.clip->y),
                         .width = static_cast<double>(lower.clip->w),
                         .height = static_cast<double>(lower.clip->h)};
      }
    }
  }

  for (const auto &offset : inputs.orderedOffsets) {
    applyRectOffset(geometry.rect, offset);
    if (geometry.clip) {
      applyRectOffset(*geometry.clip, offset);
    }
    geometry.angleDegrees += offset.r;
    // SkinObject.prepareColor returns before offset alpha on an interpolated
    // non-step frame.  Preserve that behavior exactly.
    if (!interpolated || acceleration == 3 || fixedColor) {
      geometry.rgba[3] = std::clamp(
          geometry.rgba[3] + static_cast<float>(offset.a) / 255.0F, 0.0F, 1.0F);
    }
  }
  if (geometry.clip &&
      (geometry.clip->width <= 0.0 || geometry.clip->height <= 0.0)) {
    geometry.clip.reset();
  }

  const int center = destination.center >= 0 && destination.center < 10
                         ? destination.center
                         : 0;
  geometry.centerX = kCenterX[center];
  geometry.centerY = kCenterY[center];
  geometry.blend = destination.blend;
  geometry.filter = destination.filter;
  geometry.stretch = destination.stretch;
  result.geometry = std::move(geometry);
  return result;
}

UiDestinationGeometry
projectSkinDestinationToUi(const AuthoredDestinationGeometry &destination,
                           const SkinSourceRegionGeometry &source,
                           const PlaySkinViewport &viewport) {
  UiDestinationGeometry result;
  result.rgba = destination.rgba;
  result.blend = destination.blend;
  result.filter = destination.filter;
  if (!viewport.valid || source.textureWidth <= 0 ||
      source.textureHeight <= 0 || source.region.w == 0 ||
      source.region.h == 0) {
    return result;
  }

  auto rect = destination.rect;
  auto region = source.region;
  applyStretch(rect, region, destination.stretch);
  const double radians = destination.angleDegrees * std::numbers::pi / 180.0;
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  const double pivotX = rect.x + destination.centerX * rect.width;
  const double pivotY = rect.y + destination.centerY * rect.height;
  const std::array<std::array<double, 2>, 4> corners = {
      {{rect.x, rect.y},
       {rect.x + rect.width, rect.y},
       {rect.x + rect.width, rect.y + rect.height},
       {rect.x, rect.y + rect.height}}};
  for (std::size_t index = 0; index < corners.size(); ++index) {
    const double x = corners[index][0] - pivotX;
    const double y = corners[index][1] - pivotY;
    const auto point =
        apply(viewport.authoredToUi, pivotX + x * cosine - y * sine,
              pivotY + x * sine + y * cosine);
    result.vertices[index] = point;
  }
  const double u0 = static_cast<double>(region.x) / source.textureWidth;
  const double v0 = static_cast<double>(region.y) / source.textureHeight;
  const double u1 =
      (static_cast<double>(region.x) + static_cast<double>(region.w)) /
      source.textureWidth;
  const double v1 =
      (static_cast<double>(region.y) + static_cast<double>(region.h)) /
      source.textureHeight;
  result.normalizedUvs = {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
  if (destination.clip && destination.clip->width > 0.0 &&
      destination.clip->height > 0.0) {
    const auto topLeft = apply(viewport.authoredToUi, destination.clip->x,
                               destination.clip->y + destination.clip->height);
    const auto bottomRight = apply(
        viewport.authoredToUi, destination.clip->x + destination.clip->width,
        destination.clip->y);
    result.clip =
        UiLogicalRect{.x = std::min(topLeft[0], bottomRight[0]),
                      .y = std::min(topLeft[1], bottomRight[1]),
                      .width = std::abs(bottomRight[0] - topLeft[0]),
                      .height = std::abs(bottomRight[1] - topLeft[1])};
  }
  return result;
}

} // namespace skin
