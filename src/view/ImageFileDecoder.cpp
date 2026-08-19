#include "ImageFileDecoder.h"
#include "../path.h"

#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../bgfx/bimg/3rdparty/stb/stb_image_resize.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include "../../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
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
resize(const DecodedImageData &, const ImageDecodeOptions &);

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

std::optional<std::uint32_t>
readWbmpMultiByteInteger(std::span<const std::byte> encoded,
                         std::size_t &offset) {
  std::uint32_t value = 0;
  for (int count = 0; count < 5; ++count) {
    if (offset >= encoded.size()) return std::nullopt;
    const unsigned char byte = std::to_integer<unsigned char>(encoded[offset++]);
    if (value > (std::numeric_limits<std::uint32_t>::max() >> 7U)) {
      return std::nullopt;
    }
    value = (value << 7U) | static_cast<std::uint32_t>(byte & 0x7fU);
    if ((byte & 0x80U) == 0) return value;
  }
  return std::nullopt;
}

std::optional<DecodedImageData>
decodeWbmp(std::span<const std::byte> encoded,
           const ImageDecodeOptions &options) {
  // ImageIO's standard WBMP reader accepts only Type-0 images: a zero type
  // field and zero fixed-header field, then variable-width dimensions and
  // MSB-first monochrome pixels.
  if (stopped(options) || encoded.size() < 4 ||
      std::to_integer<unsigned char>(encoded[0]) != 0 ||
      std::to_integer<unsigned char>(encoded[1]) != 0) {
    return std::nullopt;
  }
  std::size_t offset = 2;
  const auto rawWidth = readWbmpMultiByteInteger(encoded, offset);
  const auto rawHeight = readWbmpMultiByteInteger(encoded, offset);
  if (!rawWidth || !rawHeight ||
      *rawWidth > static_cast<std::uint32_t>(INT_MAX) ||
      *rawHeight > static_cast<std::uint32_t>(INT_MAX)) {
    return std::nullopt;
  }
  const int width = static_cast<int>(*rawWidth);
  const int height = static_cast<int>(*rawHeight);
  std::size_t rgbaBytes = 0;
  if (!validDimensions(width, height, options.maximumDimension,
                       options.maximumDecodedBytes, rgbaBytes)) {
    return std::nullopt;
  }
  const std::size_t rowBytes = (static_cast<std::size_t>(width) + 7U) / 8U;
  if (rowBytes > 0 && static_cast<std::size_t>(height) >
                          (encoded.size() - offset) / rowBytes) {
    return std::nullopt;
  }
  auto rgba = std::make_shared<std::vector<unsigned char>>(rgbaBytes);
  for (int y = 0; y < height; ++y) {
    if (stopped(options)) return std::nullopt;
    const std::size_t sourceRow = offset + static_cast<std::size_t>(y) * rowBytes;
    for (int x = 0; x < width; ++x) {
      const unsigned char packed = std::to_integer<unsigned char>(
          encoded[sourceRow + static_cast<std::size_t>(x) / 8U]);
      // WBMP's one bit denotes black; ImageIO materializes the other bit as
      // opaque white in BufferedImage before PixmapResourcePool converts it.
      const unsigned char intensity =
          (packed & (0x80U >> (x % 8))) == 0 ? 0xff : 0x00;
      const std::size_t destination =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) * 4U;
      (*rgba)[destination] = intensity;
      (*rgba)[destination + 1] = intensity;
      (*rgba)[destination + 2] = intensity;
      (*rgba)[destination + 3] = 0xff;
    }
  }
  DecodedImageData result{.width = width, .height = height, .rgba = std::move(rgba)};
  return result.valid() ? std::optional<DecodedImageData>(std::move(result))
                        : std::nullopt;
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

bool isWebpPath(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == ".webp";
}

std::optional<DecodedImageData>
decodeWebpWithFfmpeg(const std::filesystem::path &path,
                     const ImageDecodeOptions &options) {
  if (stopped(options)) return std::nullopt;

  const std::string encodedPath = fspath_to_utf8(path);
  AVFormatContext *rawFormat = nullptr;
  if (avformat_open_input(&rawFormat, encodedPath.c_str(), nullptr, nullptr) <
      0) {
    return std::nullopt;
  }
  const auto format = std::unique_ptr<AVFormatContext,
                                      void (*)(AVFormatContext *)>(
      rawFormat, [](AVFormatContext *value) { avformat_close_input(&value); });
  if (avformat_find_stream_info(format.get(), nullptr) < 0 ||
      stopped(options)) {
    return std::nullopt;
  }
  const int streamIndex = av_find_best_stream(
      format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (streamIndex < 0 ||
      streamIndex >= static_cast<int>(format->nb_streams)) {
    return std::nullopt;
  }
  const AVCodecParameters *parameters =
      format->streams[streamIndex]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(parameters->codec_id);
  if (codec == nullptr) return std::nullopt;

  AVCodecContext *rawCodec = avcodec_alloc_context3(codec);
  if (rawCodec == nullptr) return std::nullopt;
  const auto codecContext = std::unique_ptr<AVCodecContext,
                                             void (*)(AVCodecContext *)>(
      rawCodec, [](AVCodecContext *value) { avcodec_free_context(&value); });
  if (avcodec_parameters_to_context(codecContext.get(), parameters) < 0 ||
      avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
    return std::nullopt;
  }

  const auto frame = std::unique_ptr<AVFrame, void (*)(AVFrame *)>(
      av_frame_alloc(), [](AVFrame *value) { av_frame_free(&value); });
  const auto packet = std::unique_ptr<AVPacket, void (*)(AVPacket *)>(
      av_packet_alloc(), [](AVPacket *value) { av_packet_free(&value); });
  if (!frame || !packet) return std::nullopt;

  const auto receive = [&]() -> std::optional<DecodedImageData> {
    for (;;) {
      if (stopped(options)) return std::nullopt;
      const int result = avcodec_receive_frame(codecContext.get(), frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return std::nullopt;
      }
      if (result < 0) return std::nullopt;

      std::size_t bytes = 0;
      if (!validDimensions(frame->width, frame->height,
                           options.maximumDimension,
                           options.maximumDecodedBytes, bytes)) {
        return std::nullopt;
      }
      const auto sws = std::unique_ptr<SwsContext, void (*)(SwsContext *)>(
          sws_getContext(frame->width, frame->height,
                         static_cast<AVPixelFormat>(frame->format),
                         frame->width, frame->height, AV_PIX_FMT_RGBA,
                         SWS_BILINEAR, nullptr, nullptr, nullptr),
          sws_freeContext);
      if (!sws) return std::nullopt;
      auto rgba = std::make_shared<std::vector<unsigned char>>(bytes);
      std::array<std::uint8_t *, 4> output{rgba->data(), nullptr, nullptr,
                                            nullptr};
      std::array<int, 4> lineSizes{frame->width * 4, 0, 0, 0};
      if (sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height,
                    output.data(), lineSizes.data()) != frame->height ||
          stopped(options)) {
        return std::nullopt;
      }
      DecodedImageData decoded{.width = frame->width,
                               .height = frame->height,
                               .rgba = std::move(rgba)};
      return decoded.valid() ? resize(decoded, options) : std::nullopt;
    }
  };

  while (!stopped(options) && av_read_frame(format.get(), packet.get()) >= 0) {
    if (packet->stream_index == streamIndex &&
        avcodec_send_packet(codecContext.get(), packet.get()) >= 0) {
      if (const auto decoded = receive()) return decoded;
    }
    av_packet_unref(packet.get());
  }
  if (!stopped(options) && avcodec_send_packet(codecContext.get(), nullptr) >=
                               0) {
    return receive();
  }
  return std::nullopt;
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
    if (decoded) return resize(*decoded, options);
    const auto wbmp = decodeWbmp(encoded, options);
    return wbmp ? resize(*wbmp, options) : std::nullopt;
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
  if (stopped(options)) return std::nullopt;
  if (const auto decoded = decodeImageMemory(encoded, options)) {
    return decoded;
  }
  // PixmapResourcePool takes this same FFmpeg fallback for .webp after the
  // native pixmap and ImageIO paths reject it.
  return isWebpPath(path) ? decodeWebpWithFfmpeg(path, options)
                          : std::nullopt;
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
