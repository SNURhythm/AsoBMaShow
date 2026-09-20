#pragma once

#include "ImageFileDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace image_decode::detail {

inline std::pair<int, int> reducedDimensions(int width, int height,
                                            const ImageDecodeOptions &options) {
  if (options.targetWidth <= 0 || options.targetHeight <= 0 ||
      (width <= options.targetWidth && height <= options.targetHeight)) {
    return {width, height};
  }
  const double scale = std::min(static_cast<double>(options.targetWidth) / width,
                                static_cast<double>(options.targetHeight) / height);
  return {std::max(1, static_cast<int>(std::floor(width * scale))),
          std::max(1, static_cast<int>(std::floor(height * scale)))};
}

// Area-average pixels directly into the requested image. Sequential decoders
// retain two reduced rows of sums. Adam7 uses reduced-image sums, or a source
// buffer when that is smaller (near-unity reductions), never both.
class ImageRowReducer {
public:
  ImageRowReducer(int width, int height, const ImageDecodeOptions &options,
                  bool interlaced = false)
      : sourceWidth_(width), sourceHeight_(height), interlaced_(interlaced), stop_(options.stop) {
    const auto [outputWidth, outputHeight] = reducedDimensions(width, height, options);
    image_ = {.width = outputWidth, .height = outputHeight,
              .rgba = std::make_shared<std::vector<unsigned char>>(
                  static_cast<std::size_t>(outputWidth) * outputHeight * 4)};
    reducing_ = width != outputWidth || height != outputHeight;
    if (reducing_) {
      if (static_cast<std::uint64_t>(width) * height > UINT64_MAX / 256) {
        throw std::length_error("Image reduction exceeds accumulator representation");
      }
      const auto sourceBytes = static_cast<std::size_t>(width) * height * 4;
      const auto sumBytes = static_cast<std::size_t>(outputWidth) * outputHeight * 4 * sizeof(std::uint64_t);
      if (interlaced_ && sourceBytes < sumBytes) {
        source_.resize(sourceBytes);
        bufferingSource_ = true;
        interlaced_ = false;
      }
      sums_.resize(static_cast<std::size_t>(outputWidth) * 4 *
                   (interlaced_ ? outputHeight : 2));
    }
  }

  void add(int x, int y, const std::array<unsigned char, 4> &rgba) {
    if (bufferingSource_) {
      const auto offset = (static_cast<std::size_t>(y) * sourceWidth_ + x) * 4;
      std::copy(rgba.begin(), rgba.end(), source_.begin() + offset);
      return;
    }
    if (!reducing_) {
      const auto offset = (static_cast<std::size_t>(y) * image_.width + x) * 4;
      std::copy(rgba.begin(), rgba.end(), image_.rgba->begin() + offset);
      return;
    }
    // At a new source row, flush output rows whose entire vertical coverage
    // has arrived. One source pixel overlaps at most two reduced rows/columns.
    if (!interlaced_ && y != currentSourceRow_) {
      while (static_cast<std::int64_t>(nextOutputRow_ + 1) * sourceHeight_ <=
             static_cast<std::int64_t>(y) * image_.height) flushRow(nextOutputRow_++);
      currentSourceRow_ = y;
    }
    const std::int64_t left = static_cast<std::int64_t>(x) * image_.width;
    const std::int64_t right = left + image_.width;
    const std::int64_t top = static_cast<std::int64_t>(y) * image_.height;
    const std::int64_t bottom = top + image_.height;
    for (int outputY = top / sourceHeight_; outputY <= (bottom - 1) / sourceHeight_; ++outputY) {
      const auto dy = std::min(bottom, static_cast<std::int64_t>(outputY + 1) * sourceHeight_) -
                      std::max(top, static_cast<std::int64_t>(outputY) * sourceHeight_);
      for (int outputX = left / sourceWidth_; outputX <= (right - 1) / sourceWidth_; ++outputX) {
        const auto dx = std::min(right, static_cast<std::int64_t>(outputX + 1) * sourceWidth_) -
                        std::max(left, static_cast<std::int64_t>(outputX) * sourceWidth_);
        const auto offset = sumOffset(outputX, outputY);
        const auto weight = static_cast<std::uint64_t>(dx) * dy;
        for (int channel = 0; channel < 4; ++channel) sums_[offset + channel] += rgba[channel] * weight;
      }
    }
  }

  std::optional<DecodedImageData> finish() {
    if (stop_.stop_requested()) return std::nullopt;
    if (bufferingSource_) {
      bufferingSource_ = false;
      for (int y = 0; y < sourceHeight_; ++y) {
        if (stop_.stop_requested()) return std::nullopt;
        for (int x = 0; x < sourceWidth_; ++x) {
          const auto offset = (static_cast<std::size_t>(y) * sourceWidth_ + x) * 4;
          add(x, y, {source_[offset], source_[offset + 1], source_[offset + 2], source_[offset + 3]});
        }
      }
    }
    if (reducing_) {
      for (int y = interlaced_ ? 0 : nextOutputRow_; y < image_.height; ++y) {
        if (stop_.stop_requested()) return std::nullopt;
        flushRow(y);
      }
    }
    return stop_.stop_requested() ? std::nullopt : std::optional(std::move(image_));
  }

private:
  std::size_t sumOffset(int x, int y) const {
    return (static_cast<std::size_t>(interlaced_ ? y : y % 2) * image_.width + x) * 4;
  }
  void flushRow(int y) {
    const auto area = static_cast<std::uint64_t>(sourceWidth_) * sourceHeight_;
    for (int x = 0; x < image_.width; ++x) {
      const auto output = (static_cast<std::size_t>(y) * image_.width + x) * 4;
      const auto source = sumOffset(x, y);
      for (int channel = 0; channel < 4; ++channel) {
        (*image_.rgba)[output + channel] =
            static_cast<unsigned char>((sums_[source + channel] + area / 2) / area);
        sums_[source + channel] = 0;
      }
    }
  }

  int sourceWidth_, sourceHeight_;
  bool interlaced_, reducing_ = false, bufferingSource_ = false;
  std::stop_token stop_;
  int currentSourceRow_ = -1, nextOutputRow_ = 0;
  DecodedImageData image_;
  std::vector<std::uint64_t> sums_;
  std::vector<unsigned char> source_;
};

} // namespace image_decode::detail
