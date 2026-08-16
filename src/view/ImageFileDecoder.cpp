#include "ImageFileDecoder.h"

#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../bgfx/bimg/3rdparty/stb/stb_image_resize.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include "../../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

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

class InflateEndGuard {
public:
  explicit InflateEndGuard(mz_stream &stream) : stream_(stream) {}
  ~InflateEndGuard() { mz_inflateEnd(&stream_); }

private:
  mz_stream &stream_;
};

constexpr std::size_t kCimHeaderBytes = 12;

std::uint32_t readBigEndian32(const std::array<unsigned char, kCimHeaderBytes> &value,
                               std::size_t offset) {
  return (static_cast<std::uint32_t>(value[offset]) << 24U) |
         (static_cast<std::uint32_t>(value[offset + 1]) << 16U) |
         (static_cast<std::uint32_t>(value[offset + 2]) << 8U) |
         static_cast<std::uint32_t>(value[offset + 3]);
}

std::optional<std::size_t> cimBytesPerPixel(std::uint32_t format) {
  // These are the six GDX2D_FORMAT_* values read by
  // PixmapIO.CIM.read in the LibGDX version Beatoraja uses.
  switch (format) {
  case 1: return 1; // Alpha
  case 2: return 2; // LuminanceAlpha
  case 3: return 3; // RGB888
  case 4: return 4; // RGBA8888
  case 5: return 2; // RGB565
  case 6: return 2; // RGBA4444
  default: return std::nullopt;
  }
}

bool inflateExact(mz_stream &stream, std::span<unsigned char> output,
                  const ImageDecodeOptions &options, bool &finished) {
  finished = false;
  std::size_t offset = 0;
  while (offset < output.size()) {
    if (stopped(options)) return false;
    const std::size_t remaining = output.size() - offset;
    if (remaining > std::numeric_limits<mz_uint>::max()) return false;
    stream.next_out = output.data() + offset;
    stream.avail_out = static_cast<mz_uint>(remaining);
    const int result = mz_inflate(&stream, MZ_NO_FLUSH);
    const std::size_t produced = remaining - stream.avail_out;
    offset += produced;
    if (result == MZ_STREAM_END) {
      finished = true;
      return offset == output.size();
    }
    if (result != MZ_OK || (produced == 0 && stream.avail_in == 0)) {
      return false;
    }
  }
  return !stopped(options);
}

bool inflateHasNoRemainingOutput(mz_stream &stream,
                                 const ImageDecodeOptions &options) {
  if (stopped(options)) return false;
  unsigned char sentinel = 0;
  stream.next_out = &sentinel;
  stream.avail_out = 1;
  return mz_inflate(&stream, MZ_FINISH) == MZ_STREAM_END &&
         stream.avail_out == 1;
}

void expandCimPixels(std::uint32_t format, std::span<const unsigned char> source,
                     std::span<unsigned char> rgba) {
  const std::size_t pixels = rgba.size() / 4;
  for (std::size_t index = 0; index < pixels; ++index) {
    const std::size_t target = index * 4;
    switch (format) {
    case 1: { // Alpha
      rgba[target] = 0xff;
      rgba[target + 1] = 0xff;
      rgba[target + 2] = 0xff;
      rgba[target + 3] = source[index];
      break;
    }
    case 2: { // LuminanceAlpha
      const std::size_t offset = index * 2;
      rgba[target] = source[offset];
      rgba[target + 1] = source[offset];
      rgba[target + 2] = source[offset];
      rgba[target + 3] = source[offset + 1];
      break;
    }
    case 3: { // RGB888
      const std::size_t offset = index * 3;
      rgba[target] = source[offset];
      rgba[target + 1] = source[offset + 1];
      rgba[target + 2] = source[offset + 2];
      rgba[target + 3] = 0xff;
      break;
    }
    case 4: { // RGBA8888
      const std::size_t offset = index * 4;
      rgba[target] = source[offset];
      rgba[target + 1] = source[offset + 1];
      rgba[target + 2] = source[offset + 2];
      rgba[target + 3] = source[offset + 3];
      break;
    }
    case 5: { // RGB565, stored in the host-endian pixmap backing buffer.
      const std::size_t offset = index * 2;
      const std::uint16_t value = static_cast<std::uint16_t>(source[offset]) |
                                  static_cast<std::uint16_t>(source[offset + 1]) << 8U;
      rgba[target] = static_cast<unsigned char>(((value >> 11U) & 0x1fU) * 255U / 31U);
      rgba[target + 1] = static_cast<unsigned char>(((value >> 5U) & 0x3fU) * 255U / 63U);
      rgba[target + 2] = static_cast<unsigned char>((value & 0x1fU) * 255U / 31U);
      rgba[target + 3] = 0xff;
      break;
    }
    case 6: { // RGBA4444, stored in the host-endian pixmap backing buffer.
      const std::size_t offset = index * 2;
      const std::uint16_t value = static_cast<std::uint16_t>(source[offset]) |
                                  static_cast<std::uint16_t>(source[offset + 1]) << 8U;
      rgba[target] = static_cast<unsigned char>(((value >> 12U) & 0x0fU) * 255U / 15U);
      rgba[target + 1] = static_cast<unsigned char>(((value >> 8U) & 0x0fU) * 255U / 15U);
      rgba[target + 2] = static_cast<unsigned char>(((value >> 4U) & 0x0fU) * 255U / 15U);
      rgba[target + 3] = static_cast<unsigned char>((value & 0x0fU) * 255U / 15U);
      break;
    }
    }
  }
}

std::optional<DecodedImageData>
decodeLibGdxCim(std::span<const std::byte> encoded,
                const ImageDecodeOptions &options) {
  if (stopped(options) || encoded.size() > std::numeric_limits<mz_uint>::max()) {
    return std::nullopt;
  }
  mz_stream stream{};
  if (mz_inflateInit(&stream) != MZ_OK) return std::nullopt;
  InflateEndGuard guard(stream);
  stream.next_in = reinterpret_cast<const unsigned char *>(encoded.data());
  stream.avail_in = static_cast<mz_uint>(encoded.size());

  std::array<unsigned char, kCimHeaderBytes> header{};
  bool finished = false;
  if (!inflateExact(stream, header, options, finished) || finished) {
    return std::nullopt;
  }
  const std::uint32_t rawWidth = readBigEndian32(header, 0);
  const std::uint32_t rawHeight = readBigEndian32(header, 4);
  const std::uint32_t format = readBigEndian32(header, 8);
  if (rawWidth > static_cast<std::uint32_t>(INT_MAX) ||
      rawHeight > static_cast<std::uint32_t>(INT_MAX)) {
    return std::nullopt;
  }
  std::size_t rgbaBytes = 0;
  const int width = static_cast<int>(rawWidth);
  const int height = static_cast<int>(rawHeight);
  if (!validDimensions(width, height, options.maximumDimension,
                       options.maximumDecodedBytes, rgbaBytes)) {
    return std::nullopt;
  }
  const auto bytesPerPixel = cimBytesPerPixel(format);
  const std::size_t pixels = static_cast<std::size_t>(width) *
                             static_cast<std::size_t>(height);
  if (!bytesPerPixel || pixels > std::numeric_limits<std::size_t>::max() /
                                       *bytesPerPixel) {
    return std::nullopt;
  }
  std::vector<unsigned char> nativePixels(pixels * *bytesPerPixel);
  if (!inflateExact(stream, nativePixels, options, finished) ||
      (!finished && !inflateHasNoRemainingOutput(stream, options)) ||
      stopped(options)) {
    return std::nullopt;
  }
  auto rgba = std::make_shared<std::vector<unsigned char>>(rgbaBytes);
  expandCimPixels(format, nativePixels, *rgba);
  DecodedImageData result{.width = width, .height = height, .rgba = std::move(rgba)};
  return result.valid() ? std::optional<DecodedImageData>(std::move(result))
                        : std::nullopt;
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
    const auto decoded = decodeLibGdxCim(encoded, options);
    return decoded ? resize(*decoded, options) : std::nullopt;
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
