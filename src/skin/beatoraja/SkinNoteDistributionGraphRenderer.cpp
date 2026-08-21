#include "SkinNoteDistributionGraphRenderer.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>

namespace skin {
namespace {

constexpr std::array<std::array<std::uint32_t, 10>, 3> kGraphColorsAbgr{{
    {0xff44ff44U, 0xff228822U, 0xff4444ffU, 0xffff4444U, 0xff882222U,
     0xffccccccU, 0xff000088U, 0U, 0U, 0U},
    {0xff555555U, 0xffff8800U, 0xff88ff00U, 0xff00ffffU, 0xff0088ffU,
     0xff0000ffU, 0U, 0U, 0U, 0U},
    {0xff555555U, 0xff44ff44U, 0xffff8800U, 0xffcc6600U, 0xff884400U,
     0xff442200U, 0xff0088ffU, 0xff0066ccU, 0xff004488U, 0xff002244U},
}};

constexpr std::array<std::array<std::uint32_t, 10>, 3> kPmsColorsAbgr{{
    kGraphColorsAbgr[0],
    {0xff555555U, 0xffb05effU, 0xff32beffU, 0xff3c46dcU, 0xffffc66cU,
     0xffffc66cU, 0U, 0U, 0U, 0U},
    {0xff555555U, 0xffb05effU, 0xffff8800U, 0xffcc6600U, 0xff884400U,
     0xff442200U, 0xff0088ffU, 0xff0066ccU, 0xff004488U, 0xff002244U},
}};

std::uint32_t abgrToRgba(std::uint32_t abgr) noexcept {
  return ((abgr & 0xffU) << 24U) | ((abgr & 0xff00U) << 8U) |
         ((abgr & 0xff0000U) >> 8U) | ((abgr >> 24U) & 0xffU);
}

SkinDiagnostic diagnostic(std::string message) {
  return {.code = "skin.renderer.geometry.invalid",
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

int maximumHeight(auto data) noexcept {
  int maximum = 20;
  for (const auto &second : data) {
    std::int64_t count = 0;
    for (const int bucket : second)
      count = std::min<std::int64_t>(1'000'000,
                                     count + std::max(bucket, 0));
    if (maximum < count)
      maximum = static_cast<int>(
          std::min<std::int64_t>((count / 10) * 10 + 10, 100));
  }
  return maximum;
}

std::int64_t cursorPixel(std::int64_t millis, int width,
                         std::size_t seconds) noexcept {
  const long double value = static_cast<long double>(millis) * width /
                            (static_cast<long double>(seconds) * 1000.0L);
  if (value <= std::numeric_limits<std::int64_t>::min())
    return std::numeric_limits<std::int64_t>::min();
  if (value >= std::numeric_limits<std::int64_t>::max())
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value);
}

bool appendResult(SkinNoteDistributionGraphRenderResult &target,
                  SkinNoteDistributionGraphRenderResult source) {
  if (source.failure) {
    target = {.failure = std::move(source.failure)};
    return false;
  }
  target.commands.insert(target.commands.end(),
                         std::make_move_iterator(source.commands.begin()),
                         std::make_move_iterator(source.commands.end()));
  return true;
}

template <typename Distribution>
SkinNoteDistributionGraphRenderResult renderDistribution(
    const SkinNoteDistributionGraphRenderRequest &request,
    std::span<const Distribution> data) {
  if (data.empty() || request.geometry.rect.width == 0.0 ||
      request.geometry.rect.height == 0.0) return {};
  if (data.size() > static_cast<std::size_t>(8192 / 5))
    return {.failure = diagnostic(
                "Note distribution Pixmap exceeds the session texture limit.")};
  const int width = static_cast<int>(data.size() * 5U);
  const int maximum = maximumHeight(data);
  const int height = maximum * 5;
  SkinNoteDistributionGraphRenderResult result;

  auto remaining = [&] {
    return request.maximumCommands -
           std::min(request.maximumCommands, result.commands.size());
  };
  SkinGeneratedTextureRaster background(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Background,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .maximumCommands = remaining(),
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = width,
       .sourceHeight = height,
       .verticalFlip = true,
       .diagnosticObject = "Note distribution background"});
  if (auto *pixmap = background.pixmap(); pixmap != nullptr) {
    pixmap->clear(0U);
    if (!request.graph.backgroundTextureOff) {
      pixmap->fillRectangle(0, 0, width, height, 0x000000ccU);
      for (int row = 10; row < maximum; row += 10) {
        const auto component =
            static_cast<std::uint8_t>(0.007F * row * 255.0F);
        const auto color = (static_cast<std::uint32_t>(component) << 24U) |
                           (static_cast<std::uint32_t>(component) << 16U) |
                           0x000000ffU;
        pixmap->fillRectangle(0, row * 5, width, 50, color);
      }
      for (std::size_t second = 0; second < data.size(); ++second) {
        if (second % 60U == 0U)
          pixmap->drawLine(static_cast<int>(second * 5U), 0,
                           static_cast<int>(second * 5U), height,
                           0x3f3f3fffU);
        else if (second % 10U == 0U)
          pixmap->drawLine(static_cast<int>(second * 5U), 0,
                           static_cast<int>(second * 5U), height,
                           0x1f1f1fffU);
      }
    }
  }
  if (!appendResult(result, background.take())) return result;

  SkinGeneratedTextureRaster shape(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Shape,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .elapsedMillis = request.elapsedMillis,
       .revealMillis = request.graph.delayMillis,
       .maximumCommands = remaining(),
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = width,
       .sourceHeight = height,
       .truncateDestinationRevealWidth = false,
       .verticalFlip = true,
       .diagnosticObject = "Note distribution shape"});
  if (auto *pixmap = shape.pixmap(); pixmap != nullptr) {
    const auto type = static_cast<std::size_t>(request.graph.type);
    const auto &palette = request.pmsMode ? kPmsColorsAbgr[type]
                                         : kGraphColorsAbgr[type];
    const int cellWidth = 4 + (request.graph.noHorizontalGap ? 1 : 0);
    const int cellHeight = 4 + (request.graph.noGap ? 1 : 0);
    for (std::size_t second = 0; second < data.size(); ++second) {
      int stack = 0;
      const auto drawBucket = [&](std::size_t bucket) {
        for (int count = std::max(data[second][bucket], 0);
             count > 0 && stack < maximum; --count, ++stack)
          pixmap->fillRectangle(static_cast<int>(second * 5U), stack * 5,
                                cellWidth, cellHeight,
                                abgrToRgba(palette[bucket]));
      };
      if (request.graph.reverseOrder) {
        for (std::size_t bucket = data[second].size(); bucket-- > 0;)
          drawBucket(bucket);
      } else {
        for (std::size_t bucket = 0; bucket < data[second].size(); ++bucket)
          drawBucket(bucket);
      }
    }
  }
  if (!appendResult(result, shape.take())) return result;

  SkinGeneratedTextureRaster cursor(
      {.sourceObject = request.sourceObject,
       .authoredOrdinal = request.authoredOrdinal,
       .layer = SkinGeneratedTextureLayer::Cursor,
       .geometry = request.geometry,
       .viewport = request.viewport,
       .maximumCommands = remaining(),
       .maximumPrimitiveVertices = request.maximumPrimitiveVertices,
       .sourceWidth = width,
       .sourceHeight = height,
       .verticalFlip = true,
       .diagnosticObject = "Note distribution cursor"});
  if (auto *pixmap = cursor.pixmap(); pixmap != nullptr) {
    const auto drawCursor = [&](std::optional<std::int64_t> millis,
                                std::uint32_t color) {
      if (!millis) return;
      const auto x = cursorPixel(*millis, width, data.size());
      if (x >= std::numeric_limits<int>::min() &&
          x <= std::numeric_limits<int>::max())
        pixmap->fillRectangle(static_cast<int>(x), 0, 3, height, color);
    };
    drawCursor(request.startMillis, 0x80ff80ffU);
    drawCursor(request.endMillis, 0xff8080ffU);
    drawCursor(request.currentMillis, 0xffffffffU);
  }
  (void)appendResult(result, cursor.take());
  return result;
}

} // namespace

SkinNoteDistributionGraphRenderResult renderSkinNoteDistributionGraph(
    const SkinNoteDistributionGraphRenderRequest &request) {
  if (request.geometry.rgba[3] <= 0.0F) return {};
  switch (request.graph.type) {
  case SkinNoteDistributionGraphType::Normal:
    return renderDistribution(request, request.state.normalDistribution);
  case SkinNoteDistributionGraphType::Judge:
    return renderDistribution(request, request.state.judgementDistribution);
  case SkinNoteDistributionGraphType::EarlyLate:
    return renderDistribution(request, request.state.earlyLateDistribution);
  }
  return {.failure = diagnostic("Note distribution graph type is invalid.")};
}

} // namespace skin

#endif
