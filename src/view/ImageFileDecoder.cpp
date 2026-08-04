#include "ImageFileDecoder.h"

#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../bgfx/bimg/3rdparty/stb/stb_image_resize.h"

#include <fstream>
#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <limits>
#include <memory>
#include <vector>

namespace image_decode {
namespace {

bool validDimensions(int width, int height, int maximumDimension,
                     std::size_t maximumDecodedBytes,
                     std::size_t &bytes) {
  if (width <= 0 || height <= 0 || maximumDimension <= 0 ||
      width > maximumDimension || height > maximumDimension) {
    return false;
  }
  const auto pixels = static_cast<std::uint64_t>(width) *
                      static_cast<std::uint64_t>(height);
  if (pixels > std::numeric_limits<std::size_t>::max() / 4U) {
    return false;
  }
  bytes = static_cast<std::size_t>(pixels) * 4U;
  return bytes <= maximumDecodedBytes;
}

bool stopped(const ImageDecodeOptions &options) {
  return options.stop.stop_requested();
}

std::optional<DecodedImageData>
resize(const DecodedImageData &decoded, const ImageDecodeOptions &options) {
  if (stopped(options) || !decoded.valid() || options.targetWidth <= 0 ||
      options.targetHeight <= 0 ||
      (decoded.width <= options.targetWidth &&
       decoded.height <= options.targetHeight)) {
    return stopped(options) ? std::nullopt
                            : std::optional<DecodedImageData>(decoded);
  }
  const double scale =
      std::min(static_cast<double>(options.targetWidth) / decoded.width,
               static_cast<double>(options.targetHeight) / decoded.height);
  const int width = std::max(1, static_cast<int>(std::floor(decoded.width * scale)));
  const int height =
      std::max(1, static_cast<int>(std::floor(decoded.height * scale)));
  std::size_t bytes = 0;
  if (!validDimensions(width, height, options.maximumDimension,
                       options.maximumDecodedBytes, bytes) || stopped(options)) {
    return std::nullopt;
  }
  auto rgba = std::make_shared<std::vector<unsigned char>>(bytes);
  if (stbir_resize_uint8(decoded.rgba->data(), decoded.width, decoded.height, 0,
                         rgba->data(), width, height, 0, 4) == 0 ||
      stopped(options)) {
    return std::nullopt;
  }
  DecodedImageData result{.width = width, .height = height, .rgba = std::move(rgba)};
  return result.valid() ? std::optional<DecodedImageData>(std::move(result))
                        : std::nullopt;
}

} // namespace

std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded,
                  const ImageDecodeOptions &options) {
  if (stopped(options) || encoded.empty() ||
      encoded.size() > static_cast<std::size_t>(INT_MAX) ||
      encoded.size() > options.maximumEncodedBytes) {
    return std::nullopt;
  }
  int width = 0;
  int height = 0;
  int channels = 0;
  if (stbi_info_from_memory(
          reinterpret_cast<const stbi_uc *>(encoded.data()),
          static_cast<int>(encoded.size()), &width, &height, &channels) == 0) {
    return std::nullopt;
  }
  std::size_t bytes = 0;
  if (stopped(options) ||
      !validDimensions(width, height, options.maximumDimension,
                       options.maximumDecodedBytes, bytes)) {
    return std::nullopt;
  }
  std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
      stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()),
                            static_cast<int>(encoded.size()), &width, &height,
                            &channels, 4),
      stbi_image_free);
  if (stopped(options) || !decoded ||
      !validDimensions(width, height, options.maximumDimension,
                       options.maximumDecodedBytes, bytes)) {
    return std::nullopt;
  }
  if (stopped(options)) return std::nullopt;
  auto rgba = std::make_shared<std::vector<unsigned char>>(decoded.get(),
                                                             decoded.get() + bytes);
  if (stopped(options)) return std::nullopt;
  DecodedImageData result{.width = width, .height = height, .rgba = std::move(rgba)};
  if (!result.valid()) return std::nullopt;
  return resize(result, options);
}

std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path,
                const ImageDecodeOptions &options) {
  if (stopped(options)) return std::nullopt;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::vector<std::byte> encoded;
  std::array<std::byte, 64U * 1024U> chunk{};
  while (!stopped(options)) {
    input.read(reinterpret_cast<char *>(chunk.data()),
               static_cast<std::streamsize>(chunk.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      const auto readCount = static_cast<std::size_t>(count);
      if (readCount > options.maximumEncodedBytes - encoded.size() ||
          readCount > static_cast<std::size_t>(INT_MAX) - encoded.size()) {
        return std::nullopt;
      }
      encoded.insert(encoded.end(), chunk.begin(), chunk.begin() + count);
    }
    if (input.eof()) break;
    if (!input) return std::nullopt;
  }
  return !stopped(options) ? decodeImageMemory(encoded, options) : std::nullopt;
}

std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded, int maximumDimension,
                  std::size_t maximumDecodedBytes) {
  return decodeImageMemory(encoded, {.maximumDimension = maximumDimension,
                                     .maximumDecodedBytes = maximumDecodedBytes});
}

std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path, int maximumDimension,
                std::size_t maximumDecodedBytes,
                std::size_t maximumEncodedBytes) {
  return decodeImageFile(path, {.maximumDimension = maximumDimension,
                                .maximumEncodedBytes = maximumEncodedBytes,
                                .maximumDecodedBytes = maximumDecodedBytes});
}

std::optional<DecodedImageData>
resizeDecodedImage(const DecodedImageData &decoded,
                   const ImageDecodeOptions &options) {
  return resize(decoded, options);
}

} // namespace image_decode
