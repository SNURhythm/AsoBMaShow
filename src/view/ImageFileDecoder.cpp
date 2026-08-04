#include "ImageFileDecoder.h"

#include "../StbImageRAII.h"

#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <fstream>
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

} // namespace

std::optional<DecodedImageData>
decodeImageMemory(std::span<const std::byte> encoded, int maximumDimension,
                  std::size_t maximumDecodedBytes) {
  if (encoded.empty() || encoded.size() > static_cast<std::size_t>(INT_MAX)) {
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
  if (!validDimensions(width, height, maximumDimension, maximumDecodedBytes,
                       bytes)) {
    return std::nullopt;
  }
  StbiImageHandle decoded(stbi_load_from_memory(
      reinterpret_cast<const stbi_uc *>(encoded.data()),
      static_cast<int>(encoded.size()), &width, &height, &channels, 4));
  if (!decoded || !validDimensions(width, height, maximumDimension,
                                  maximumDecodedBytes, bytes)) {
    return std::nullopt;
  }
  auto rgba = std::make_shared<std::vector<unsigned char>>(decoded.get(),
                                                             decoded.get() + bytes);
  DecodedImageData result{.width = width, .height = height, .rgba = std::move(rgba)};
  return result.valid() ? std::optional<DecodedImageData>(std::move(result))
                        : std::nullopt;
}

std::optional<DecodedImageData>
decodeImageFile(const std::filesystem::path &path, int maximumDimension,
                std::size_t maximumDecodedBytes,
                std::size_t maximumEncodedBytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return std::nullopt;
  }
  const auto length = input.tellg();
  if (length <= 0 || static_cast<std::uintmax_t>(length) >
                         static_cast<std::uintmax_t>(INT_MAX) ||
      static_cast<std::uintmax_t>(length) > maximumEncodedBytes) {
    return std::nullopt;
  }
  std::vector<std::byte> encoded(static_cast<std::size_t>(length));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(encoded.data()),
             static_cast<std::streamsize>(encoded.size()));
  return input ? decodeImageMemory(encoded, maximumDimension,
                                   maximumDecodedBytes)
               : std::nullopt;
}

} // namespace image_decode
