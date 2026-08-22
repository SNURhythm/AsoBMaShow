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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {

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
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(left) +
                                     static_cast<std::uint32_t>(right));
}

// RGBA8888 subset of libGDX 1.9.9 gdx2d Pixmap used by the pinned widgets.
class SkinSoftwarePixmap {
public:
  static std::optional<SkinSoftwarePixmap> create(int width, int height) {
    if (width <= 0 || height <= 0 ||
        width > SkinResourcePolicy::maximumDimension ||
        height > SkinResourcePolicy::maximumDimension) {
      return std::nullopt;
    }
    const auto bytes = static_cast<std::uint64_t>(width) *
                       static_cast<std::uint64_t>(height) * 4U;
    if (bytes > std::numeric_limits<std::size_t>::max() ||
        bytes > SkinResourcePolicy::maximumGeneratedSessionBytes) {
      return std::nullopt;
    }
    try {
      SkinSoftwarePixmap result;
      result.width_ = width;
      result.height_ = height;
      result.pixels_ = std::make_shared<std::vector<std::uint8_t>>(
          static_cast<std::size_t>(bytes), 0U);
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] const std::shared_ptr<std::vector<std::uint8_t>> &pixels()
      const noexcept { return pixels_; }

  void clear(std::uint32_t rgba) noexcept {
    if (!pixels_) {
      return;
    }
    const std::array bytes{static_cast<std::uint8_t>(rgba >> 24U),
                           static_cast<std::uint8_t>(rgba >> 16U),
                           static_cast<std::uint8_t>(rgba >> 8U),
                           static_cast<std::uint8_t>(rgba)};
    for (std::size_t offset = 0; offset < pixels_->size(); offset += 4U) {
      std::copy(bytes.begin(), bytes.end(), pixels_->begin() + offset);
    }
  }

  void fillRectangle(int x, int y, int width, int height,
                     std::uint32_t rgba) noexcept {
    int x2 = wrappingAdd(x, static_cast<std::uint32_t>(width) - 1U);
    int y2 = wrappingAdd(y, static_cast<std::uint32_t>(height) - 1U);
    if (x >= width_ || y >= height_ || x2 < 0 || y2 < 0) {
      return;
    }
    x = std::max(x, 0);
    y = std::max(y, 0);
    x2 = std::min(x2, width_ - 1);
    y2 = std::min(y2, height_ - 1);
    for (int row = y; row <= y2; ++row) {
      horizontalLine(x, x2, row, rgba);
    }
  }

  void drawLine(int x0, int y0, int x1, int y1,
                std::uint32_t rgba) noexcept {
    int dy = y1 - y0;
    int dx = x1 - x0;
    const int stepY = dy < 0 ? -1 : 1;
    const int stepX = dx < 0 ? -1 : 1;
    dy = std::abs(dy) << 1;
    dx = std::abs(dx) << 1;
    blendPixel(x0, y0, rgba);
    if (dx > dy) {
      int fraction = dy - (dx >> 1);
      while (x0 != x1) {
        if (fraction >= 0) {
          y0 += stepY;
          fraction -= dx;
        }
        x0 += stepX;
        fraction += dy;
        blendPixel(x0, y0, rgba);
      }
    } else {
      int fraction = dx - (dy >> 1);
      while (y0 != y1) {
        if (fraction >= 0) {
          x0 += stepX;
          fraction -= dy;
        }
        y0 += stepY;
        fraction += dx;
        blendPixel(x0, y0, rgba);
      }
    }
  }

  void fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                    std::uint32_t rgba) noexcept {
    const double signedArea =
        static_cast<double>(static_cast<std::int64_t>(x2) - x1) *
            static_cast<double>(static_cast<std::int64_t>(y3) - y1) -
        static_cast<double>(static_cast<std::int64_t>(x3) - x1) *
            static_cast<double>(static_cast<std::int64_t>(y2) - y1);
    if (signedArea == 0.0) {
      return;
    }
    struct Edge {
      int x1;
      int y1;
      int x2;
      int y2;
    };
    const auto edge = [](int ax, int ay, int bx, int by) {
      return by > ay ? Edge{ax, ay, bx, by} : Edge{bx, by, ax, ay};
    };
    std::array edges{edge(x1, y1, x2, y2), edge(x1, y1, x3, y3),
                     edge(x2, y2, x3, y3)};
    const auto length = [](const Edge &value) { return value.y2 - value.y1; };
    if (length(edges[1]) >= length(edges[0]) &&
        length(edges[1]) >= length(edges[2])) {
      std::swap(edges[0], edges[1]);
    } else if (length(edges[2]) >= length(edges[0]) &&
               length(edges[2]) >= length(edges[1])) {
      std::swap(edges[0], edges[2]);
    }
    if (length(edges[2]) > length(edges[1])) {
      std::swap(edges[1], edges[2]);
    }
    const auto rasterPair = [&](const Edge &first, const Edge &second) {
      const int firstLength = length(first);
      const int secondLength = length(second);
      if (firstLength <= 0 || secondLength <= 0) {
        return;
      }
      const float firstSlope = static_cast<float>(
                                   static_cast<std::int64_t>(first.x1) -
                                   first.x2) /
                               static_cast<float>(firstLength);
      const float secondSlope = static_cast<float>(
                                    static_cast<std::int64_t>(second.x1) -
                                    second.x2) /
                                static_cast<float>(secondLength);
      for (int y = std::max(second.y1, 0);
           y <= std::min(second.y2, height_ - 1); ++y) {
        const int firstX = skinGeneratedTextureJavaInt(
            static_cast<double>(static_cast<float>(first.x2) +
                                firstSlope *
                                    static_cast<float>(first.y2 - y) +
                                0.5F));
        const int secondX = skinGeneratedTextureJavaInt(
            static_cast<double>(static_cast<float>(second.x2) +
                                secondSlope *
                                    static_cast<float>(second.y2 - y) +
                                0.5F));
        horizontalLine(firstX, secondX, y, rgba);
      }
    };
    rasterPair(edges[0], edges[1]);
    rasterPair(edges[0], edges[2]);
  }

  [[nodiscard]] std::uint8_t maximumAlpha() const noexcept {
    std::uint8_t result = 0;
    if (pixels_) {
      for (std::size_t i = 3; i < pixels_->size(); i += 4U) {
        result = std::max(result, (*pixels_)[i]);
      }
    }
    return result;
  }

private:
  SkinSoftwarePixmap() = default;
  static int wrappingAdd(int value, std::uint32_t amount) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value) +
                                       amount);
  }
  void horizontalLine(int x1, int x2, int y, std::uint32_t rgba) noexcept {
    if (y < 0 || y >= height_) {
      return;
    }
    if (x1 > x2) {
      std::swap(x1, x2);
    }
    if (x1 >= width_ || x2 < 0) {
      return;
    }
    x1 = std::max(x1, 0);
    x2 = std::min(x2, width_ - 1);
    for (int x = x1; x <= x2; ++x) {
      blendPixel(x, y, rgba);
    }
  }
  void blendPixel(int x, int y, std::uint32_t source) noexcept {
    if (!pixels_ || x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
         static_cast<std::size_t>(x)) * 4U;
    const int sr = static_cast<int>(source >> 24U);
    const int sg = static_cast<int>((source >> 16U) & 0xffU);
    const int sb = static_cast<int>((source >> 8U) & 0xffU);
    const int sa = static_cast<int>(source & 0xffU);
    const int dr = (*pixels_)[offset];
    const int dg = (*pixels_)[offset + 1U];
    const int db = (*pixels_)[offset + 2U];
    const int da = (*pixels_)[offset + 3U];
    (*pixels_)[offset] = static_cast<std::uint8_t>(dr + sa * (sr - dr) / 255);
    (*pixels_)[offset + 1U] =
        static_cast<std::uint8_t>(dg + sa * (sg - dg) / 255);
    (*pixels_)[offset + 2U] =
        static_cast<std::uint8_t>(db + sa * (sb - db) / 255);
    (*pixels_)[offset + 3U] = static_cast<std::uint8_t>(
        (1.0F - (1.0F - sa / 255.0F) * (1.0F - da / 255.0F)) * 255.0F);
  }
  int width_ = 0;
  int height_ = 0;
  std::shared_ptr<std::vector<std::uint8_t>> pixels_;
};

struct SkinGeneratedTextureCacheLimits {
  std::size_t maximumTextures = SkinResourcePolicy::maximumGeneratedTextures;
  std::size_t maximumBytes =
      SkinResourcePolicy::maximumGeneratedSessionBytes;
};

struct SkinGeneratedTextureCacheStats {
  std::size_t pixmapAllocations = 0;
  std::size_t rasterizations = 0;
  std::size_t alphaScans = 0;
  std::size_t reuses = 0;
  std::size_t allocatedBytes = 0;
};

// Session-owned CPU Pixmap state. Two buffers per key keep a previously
// published command immutable while the next dirty generation is rasterized.
// Count and byte admission happens before either backing vector is allocated.
class SkinGeneratedTextureCache {
public:
  struct Acquisition {
    SkinSoftwarePixmap *pixmap = nullptr;
    std::uint8_t *maximumAlpha = nullptr;
    bool rasterize = false;
    std::uint64_t generation = 0;
  };

  explicit SkinGeneratedTextureCache(
      SkinGeneratedTextureCacheLimits limits = {}) noexcept
      : limits_(limits) {}
  ~SkinGeneratedTextureCache() { clear(); }
  SkinGeneratedTextureCache(const SkinGeneratedTextureCache &) = delete;
  SkinGeneratedTextureCache &
  operator=(const SkinGeneratedTextureCache &) = delete;

  void setLiveResourceCounters(
      std::shared_ptr<SkinLiveResourceCounters> counters) noexcept {
    if (liveCounters_ == counters) return;
    clear();
    liveCounters_ = std::move(counters);
  }

  [[nodiscard]] Acquisition acquire(const SkinGeneratedTextureKey &key,
                                    int width, int height,
                                    std::uint64_t contentRevision,
                                    std::int64_t nowMillis,
                                    int minimumUpdateIntervalMillis) {
    if (key.sourceObject == 0 || width <= 0 || height <= 0) {
      return {};
    }
    const auto byteCount64 = static_cast<std::uint64_t>(width) *
                             static_cast<std::uint64_t>(height) * 4U;
    if (byteCount64 > std::numeric_limits<std::size_t>::max() ||
        byteCount64 > std::numeric_limits<std::size_t>::max() / 2U) {
      return {};
    }
    const std::size_t doubleBufferedBytes =
        static_cast<std::size_t>(byteCount64) * 2U;
    auto found = entries_.find(key);
    const std::size_t oldBytes =
        found == entries_.end() ? 0U : found->second.allocatedBytes;
    if (limits_.maximumTextures == 0 ||
        (found == entries_.end() && entries_.size() >= limits_.maximumTextures) ||
        stats_.allocatedBytes < oldBytes ||
        doubleBufferedBytes >
            limits_.maximumBytes -
                std::min(limits_.maximumBytes,
                         stats_.allocatedBytes - oldBytes)) {
      return {};
    }

    if (found == entries_.end() || found->second.width != width ||
        found->second.height != height) {
      auto first = SkinSoftwarePixmap::create(width, height);
      auto second = SkinSoftwarePixmap::create(width, height);
      if (!first || !second) {
        return {};
      }
      Entry replacement;
      replacement.width = width;
      replacement.height = height;
      replacement.buffers[0] = std::move(*first);
      replacement.buffers[1] = std::move(*second);
      replacement.allocatedBytes = doubleBufferedBytes;
      replacement.contentRevision = contentRevision;
      replacement.lastRasterMillis = nowMillis;
      replacement.generation = 1;
      if (found == entries_.end()) {
        try {
          found = entries_.emplace(key, std::move(replacement)).first;
        } catch (...) {
          return {};
        }
      } else {
        found->second = std::move(replacement);
      }
      stats_.allocatedBytes = stats_.allocatedBytes - oldBytes +
                              doubleBufferedBytes;
      if (liveCounters_) {
        if (oldBytes != 0) {
          liveCounters_->generatedPixmapDestroyed(oldBytes / 2U);
          liveCounters_->generatedPixmapDestroyed(oldBytes / 2U);
        }
        liveCounters_->generatedPixmapCreated(doubleBufferedBytes / 2U);
        liveCounters_->generatedPixmapCreated(doubleBufferedBytes / 2U);
      }
      stats_.pixmapAllocations += 2;
      ++stats_.rasterizations;
      found->second.buffers[0]->clear(0U);
      return {.pixmap = &*found->second.buffers[0],
              .maximumAlpha = &found->second.maximumAlpha,
              .rasterize = true,
              .generation = found->second.generation};
    }

    Entry &entry = found->second;
    bool cadenceReached = minimumUpdateIntervalMillis <= 0;
    if (!cadenceReached && nowMillis >= entry.lastRasterMillis) {
      cadenceReached =
          nowMillis - entry.lastRasterMillis >= minimumUpdateIntervalMillis;
    }
    if (contentRevision != entry.contentRevision &&
        (cadenceReached || nowMillis < entry.lastRasterMillis)) {
      entry.active ^= 1U;
      entry.contentRevision = contentRevision;
      entry.lastRasterMillis = nowMillis;
      ++entry.generation;
      ++stats_.rasterizations;
      entry.buffers[entry.active]->clear(0U);
      return {.pixmap = &*entry.buffers[entry.active],
              .maximumAlpha = &entry.maximumAlpha,
              .rasterize = true,
              .generation = entry.generation};
    }
    ++stats_.reuses;
    return {.pixmap = &*entry.buffers[entry.active],
            .maximumAlpha = &entry.maximumAlpha,
            .rasterize = false,
            .generation = entry.generation};
  }

  void clear() noexcept {
    if (liveCounters_) {
      for (const auto &[key, entry] : entries_) {
        (void)key;
        liveCounters_->generatedPixmapDestroyed(entry.allocatedBytes / 2U);
        liveCounters_->generatedPixmapDestroyed(entry.allocatedBytes / 2U);
      }
    }
    entries_.clear();
    stats_.allocatedBytes = 0;
  }

  [[nodiscard]] SkinGeneratedTextureCacheStats stats() const noexcept {
    return stats_;
  }

  void recordAlphaScan() noexcept { ++stats_.alphaScans; }

private:
  struct Entry {
    int width = 0;
    int height = 0;
    std::array<std::optional<SkinSoftwarePixmap>, 2> buffers;
    std::size_t active = 0;
    std::size_t allocatedBytes = 0;
    std::uint64_t contentRevision = 0;
    std::int64_t lastRasterMillis = 0;
    std::uint64_t generation = 0;
    std::uint8_t maximumAlpha = 0;
  };

  SkinGeneratedTextureCacheLimits limits_;
  std::map<SkinGeneratedTextureKey, Entry> entries_;
  SkinGeneratedTextureCacheStats stats_;
  std::shared_ptr<SkinLiveResourceCounters> liveCounters_;
};

struct SkinGeneratedTextureRasterRequest {
  SkinObjectId sourceObject = 0;
  std::uint32_t authoredOrdinal = 0;
  SkinGeneratedTextureLayer layer = SkinGeneratedTextureLayer::Primary;
  const AuthoredDestinationGeometry &geometry;
  const PlaySkinViewport &viewport;
  std::int64_t elapsedMillis = 0;
  int revealMillis = 0;
  std::size_t maximumCommands = 0;
  std::size_t maximumPrimitiveVertices = 0;
  std::optional<float> sourceRevealWidth;
  std::optional<int> sourceWidth;
  std::optional<int> sourceHeight;
  bool truncateDestinationRevealWidth = true;
  bool verticalFlip = true;
  std::string_view diagnosticObject;
  SkinGeneratedTextureCache *cache = nullptr;
  std::uint64_t contentRevision = 0;
  int minimumUpdateIntervalMillis = 0;
};

struct SkinGeneratedTextureRasterResult {
  std::vector<SkinDrawCommand> commands;
  std::size_t primitiveVertices = 0;
  std::optional<SkinDiagnostic> failure;
};

class SkinGeneratedTextureRaster {
public:
  explicit SkinGeneratedTextureRaster(
      const SkinGeneratedTextureRasterRequest &request)
      : request_(request),
        textureWidth_(request.sourceWidth.value_or(skinGeneratedTextureJavaInt(
            std::abs(request.geometry.rect.width)))),
        textureHeight_(request.sourceHeight.value_or(skinGeneratedTextureJavaInt(
            std::abs(request.geometry.rect.height)))) {
    if (request.geometry.rgba[3] <= 0.0F || textureWidth_ <= 0 ||
        textureHeight_ <= 0) {
      return;
    }
    if (request.cache != nullptr) {
      const auto acquired = request.cache->acquire(
          {.sourceObject = request.sourceObject,
           .authoredOrdinal = request.authoredOrdinal,
           .layer = request.layer},
          textureWidth_, textureHeight_, request.contentRevision,
          request.elapsedMillis, request.minimumUpdateIntervalMillis);
      pixmap_ = acquired.pixmap;
      cachedMaximumAlpha_ = acquired.maximumAlpha;
      rasterize_ = acquired.rasterize;
      contentRevision_ = acquired.generation;
      cache_ = request.cache;
    } else {
      ownedPixmap_ = SkinSoftwarePixmap::create(textureWidth_, textureHeight_);
      pixmap_ = ownedPixmap_ ? &*ownedPixmap_ : nullptr;
      rasterize_ = true;
      contentRevision_ = 1;
    }
    if (pixmap_ == nullptr) {
      failure_ = diagnostic("skin.renderer.generated_texture.invalid",
                            "has an invalid or unallocatable Pixmap canvas.");
      return;
    }
    drawable_ = true;
  }
  [[nodiscard]] bool drawable() const noexcept { return drawable_; }
  [[nodiscard]] int textureWidth() const noexcept { return textureWidth_; }
  [[nodiscard]] int textureHeight() const noexcept { return textureHeight_; }
  [[nodiscard]] SkinSoftwarePixmap *pixmap() noexcept {
    return rasterize_ ? pixmap_ : nullptr;
  }
  bool appendRectangle(int x, int y, int width, int height,
                       std::uint32_t rgba) noexcept {
    if (pixmap_ && rasterize_) pixmap_->fillRectangle(x, y, width, height, rgba);
    return true;
  }
  bool appendLine(int x0, int y0, int x1, int y1,
                  std::uint32_t rgba) noexcept {
    if (pixmap_ && rasterize_) pixmap_->drawLine(x0, y0, x1, y1, rgba);
    return true;
  }
  bool appendTriangle(int x1, int y1, int x2, int y2, int x3, int y3,
                      std::uint32_t rgba) noexcept {
    if (pixmap_ && rasterize_)
      pixmap_->fillTriangle(x1, y1, x2, y2, x3, y3, rgba);
    return true;
  }
  [[nodiscard]] SkinGeneratedTextureRasterResult take() {
    SkinGeneratedTextureRasterResult result;
    if (failure_) {
      result.failure = std::move(failure_);
      return result;
    }
    if (!drawable_ || !pixmap_) {
      return result;
    }
    std::uint8_t maximumAlpha = 0;
    if (rasterize_ || cachedMaximumAlpha_ == nullptr) {
      maximumAlpha = pixmap_->maximumAlpha();
      if (cachedMaximumAlpha_ != nullptr) {
        *cachedMaximumAlpha_ = maximumAlpha;
        cache_->recordAlphaScan();
      }
    } else {
      maximumAlpha = *cachedMaximumAlpha_;
    }
    const float reveal = request_.revealMillis <= 0 ||
                                 request_.elapsedMillis >= request_.revealMillis
                             ? 1.0F
                             : static_cast<float>(request_.elapsedMillis) /
                                   static_cast<float>(request_.revealMillis);
    const int visibleWidth = skinGeneratedTextureJavaInt(
        static_cast<double>(request_.sourceRevealWidth.value_or(
                                static_cast<float>(textureWidth_)) * reveal));
    const float revealedDestinationWidth =
        static_cast<float>(request_.geometry.rect.width) * reveal;
    const double drawWidth = request_.truncateDestinationRevealWidth
                                 ? static_cast<double>(
                                       skinGeneratedTextureJavaInt(
                                           revealedDestinationWidth))
                                 : static_cast<double>(
                                       revealedDestinationWidth);
    if (visibleWidth == 0 || drawWidth == 0) {
      return result;
    }
    const auto packedAlpha = static_cast<std::uint8_t>(
        static_cast<float>(maximumAlpha) *
        std::clamp(request_.geometry.rgba[3], 0.0F, 1.0F));
    if (packedAlpha == 0U) {
      return result;
    }
    if (request_.maximumCommands == 0) {
      result.failure = diagnostic("skin.renderer.command.limit",
                                  "exceeds the fixed frame command limit.");
      return result;
    }
    auto geometry = request_.geometry;
    if (request_.verticalFlip) {
      geometry.rect = {.x = request_.geometry.rect.x,
                       .y = request_.geometry.rect.y +
                            request_.geometry.rect.height,
                       .width = drawWidth,
                       .height = -request_.geometry.rect.height};
    } else {
      geometry.rect.width = drawWidth;
    }
    const auto projected = projectSkinDestinationToUi(
        geometry,
        {.textureWidth = textureWidth_,
         .textureHeight = textureHeight_,
         .region = {.x = 0, .y = 0, .w = visibleWidth, .h = textureHeight_}},
        request_.viewport);
    bool empty = false;
    const auto clip = intersect(projected.clip,
                                projectedSkinScissorBounds(request_.viewport),
                                empty);
    if (empty) {
      return result;
    }
    SkinGeneratedTexturedQuadCommand command;
    command.key = {.sourceObject = request_.sourceObject,
                   .authoredOrdinal = request_.authoredOrdinal,
                   .layer = request_.layer};
    command.texture = {.width = textureWidth_,
                       .height = textureHeight_,
                       .rgba = pixmap_->pixels(),
                       .contentRevision = contentRevision_};
    command.state = {.blend = projected.blend,
                     .filter = projected.filter,
                     .scissor = clip};
    const std::uint32_t color = packAbgr(projected.rgba);
    const auto validFloat = [](double value) {
      return std::isfinite(value) &&
             value >= -std::numeric_limits<float>::max() &&
             value <= std::numeric_limits<float>::max();
    };
    for (std::size_t i = 0; i < command.vertices.size(); ++i) {
      if (!validFloat(projected.vertices[i][0]) ||
          !validFloat(projected.vertices[i][1]) ||
          !validFloat(projected.normalizedUvs[i][0]) ||
          !validFloat(projected.normalizedUvs[i][1])) {
        result.failure = diagnostic("skin.renderer.geometry.invalid",
                                    "projects outside the finite range.");
        return result;
      }
      command.vertices[i] = {.x = static_cast<float>(projected.vertices[i][0]),
                             .y = static_cast<float>(projected.vertices[i][1]),
                             .u = static_cast<float>(
                                 projected.normalizedUvs[i][0]),
                             .v = static_cast<float>(
                                 projected.normalizedUvs[i][1]),
                             .rgba = color};
    }
    result.commands.push_back({.authoredOrdinal = request_.authoredOrdinal,
                               .sourceObject = request_.sourceObject,
                               .payload = std::move(command)});
    return result;
  }

private:
  [[nodiscard]] SkinDiagnostic diagnostic(std::string code,
                                          std::string suffix) const {
    return {.code = std::move(code),
            .message = std::string(request_.diagnosticObject) + " " + suffix,
            .severity = DiagnosticSeverity::Error};
  }
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
    return UiLogicalRect{.x = left,
                         .y = top,
                         .width = right - left,
                         .height = bottom - top};
  }
  static std::uint8_t colorByte(float value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
  }
  static std::uint32_t packAbgr(const std::array<float, 4> &rgba) noexcept {
    const std::uint32_t r = colorByte(rgba[0]);
    const std::uint32_t g = colorByte(rgba[1]);
    const std::uint32_t b = colorByte(rgba[2]);
    const std::uint32_t a = colorByte(rgba[3]);
    return (a << 24U) | (b << 16U) | (g << 8U) | r;
  }
  SkinGeneratedTextureRasterRequest request_;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  std::optional<SkinSoftwarePixmap> ownedPixmap_;
  SkinSoftwarePixmap *pixmap_ = nullptr;
  SkinGeneratedTextureCache *cache_ = nullptr;
  std::uint8_t *cachedMaximumAlpha_ = nullptr;
  std::optional<SkinDiagnostic> failure_;
  bool drawable_ = false;
  bool rasterize_ = false;
  std::uint64_t contentRevision_ = 0;
};

} // namespace skin
