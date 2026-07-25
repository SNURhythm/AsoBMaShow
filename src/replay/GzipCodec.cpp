#include "GzipCodec.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include "../../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace replay {
namespace {

constexpr std::size_t kGzipHeaderSize = 10;
constexpr std::size_t kGzipTrailerSize = 8;
constexpr std::size_t kChunkSize = 64U * 1024U;

class DeflateEndGuard {
public:
  explicit DeflateEndGuard(mz_stream &stream) : stream_(stream) {}
  ~DeflateEndGuard() { mz_deflateEnd(&stream_); }

private:
  mz_stream &stream_;
};

class InflateEndGuard {
public:
  explicit InflateEndGuard(mz_stream &stream) : stream_(stream) {}
  ~InflateEndGuard() { mz_inflateEnd(&stream_); }

private:
  mz_stream &stream_;
};

void appendLittleEndian32(std::vector<std::byte> &output, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

std::uint16_t readLittleEndian16(std::span<const std::byte> value,
                                 std::size_t offset) {
  return static_cast<std::uint16_t>(
             std::to_integer<unsigned char>(value[offset])) |
         static_cast<std::uint16_t>(
             std::to_integer<unsigned char>(value[offset + 1]))
             << 8U;
}

std::uint32_t readLittleEndian32(std::span<const std::byte> value,
                                 std::size_t offset) {
  std::uint32_t result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    result |= static_cast<std::uint32_t>(
                  std::to_integer<unsigned char>(value[offset++]))
              << shift;
  }
  return result;
}

std::uint32_t crc32(std::span<const std::byte> value) {
  mz_ulong crc = MZ_CRC32_INIT;
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t count =
        std::min(value.size() - offset,
                 static_cast<std::size_t>(std::numeric_limits<mz_uint>::max()));
    crc = mz_crc32(
        crc, reinterpret_cast<const unsigned char *>(value.data() + offset),
        count);
    offset += count;
  }
  return static_cast<std::uint32_t>(crc);
}

bool skipZeroTerminated(std::span<const std::byte> input, std::size_t trailer,
                        std::size_t &offset) {
  while (offset < trailer) {
    if (input[offset++] == std::byte{0}) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t> gzipPayloadOffset(std::span<const std::byte> input,
                                             std::string &diagnostic) {
  if (input.size() < kGzipHeaderSize + kGzipTrailerSize) {
    diagnostic = "Gzip stream is shorter than its framing";
    return std::nullopt;
  }
  if (input[0] != std::byte{0x1f} || input[1] != std::byte{0x8b} ||
      input[2] != std::byte{8}) {
    diagnostic = "Gzip header is invalid";
    return std::nullopt;
  }
  const unsigned int flags = std::to_integer<unsigned char>(input[3]);
  if ((flags & 0xe0U) != 0U) {
    diagnostic = "Gzip header uses reserved flags";
    return std::nullopt;
  }

  const std::size_t trailer = input.size() - kGzipTrailerSize;
  std::size_t offset = kGzipHeaderSize;
  if ((flags & 0x04U) != 0U) {
    if (offset + 2 > trailer) {
      diagnostic = "Gzip extra-field length is truncated";
      return std::nullopt;
    }
    const std::size_t extraLength = readLittleEndian16(input, offset);
    offset += 2;
    if (extraLength > trailer - offset) {
      diagnostic = "Gzip extra field is truncated";
      return std::nullopt;
    }
    offset += extraLength;
  }
  if ((flags & 0x08U) != 0U && !skipZeroTerminated(input, trailer, offset)) {
    diagnostic = "Gzip filename field is unterminated";
    return std::nullopt;
  }
  if ((flags & 0x10U) != 0U && !skipZeroTerminated(input, trailer, offset)) {
    diagnostic = "Gzip comment field is unterminated";
    return std::nullopt;
  }
  if ((flags & 0x02U) != 0U) {
    if (offset + 2 > trailer) {
      diagnostic = "Gzip header CRC is truncated";
      return std::nullopt;
    }
    offset += 2;
  }
  if (offset >= trailer) {
    diagnostic = "Gzip stream has no deflate payload";
    return std::nullopt;
  }
  return offset;
}

} // namespace

std::optional<std::vector<std::byte>>
gzipCompress(std::span<const std::byte> input, std::string &diagnostic) {
  diagnostic.clear();
  if (input.size() > std::numeric_limits<mz_uint>::max()) {
    diagnostic = "Gzip input is too large for the compressor";
    return std::nullopt;
  }

  mz_stream stream{};
  if (mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED,
                      -MZ_DEFAULT_WINDOW_BITS, 9,
                      MZ_DEFAULT_STRATEGY) != MZ_OK) {
    diagnostic = "Could not initialize gzip compression";
    return std::nullopt;
  }
  DeflateEndGuard guard(stream);
  stream.next_in = reinterpret_cast<const unsigned char *>(input.data());
  stream.avail_in = static_cast<mz_uint>(input.size());

  std::vector<std::byte> output = {
      std::byte{0x1f}, std::byte{0x8b}, std::byte{0x08}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0xff},
  };
  std::array<std::byte, kChunkSize> chunk{};
  for (;;) {
    stream.next_out = reinterpret_cast<unsigned char *>(chunk.data());
    stream.avail_out = static_cast<mz_uint>(chunk.size());
    const int status = mz_deflate(&stream, MZ_FINISH);
    const std::size_t produced = chunk.size() - stream.avail_out;
    output.insert(output.end(), chunk.begin(), chunk.begin() + produced);
    if (status == MZ_STREAM_END) {
      break;
    }
    if (status != MZ_OK || produced == 0) {
      diagnostic = "Gzip compression failed";
      return std::nullopt;
    }
  }

  appendLittleEndian32(output, crc32(input));
  appendLittleEndian32(output, static_cast<std::uint32_t>(input.size()));
  return output;
}

std::optional<std::vector<std::byte>>
gzipDecompressBounded(std::span<const std::byte> input,
                      std::size_t maximumOutputBytes, std::string &diagnostic) {
  diagnostic.clear();
  const auto payloadOffset = gzipPayloadOffset(input, diagnostic);
  if (!payloadOffset) {
    return std::nullopt;
  }
  const std::size_t trailerOffset = input.size() - kGzipTrailerSize;
  const std::size_t payloadSize = trailerOffset - *payloadOffset;
  if (payloadSize > std::numeric_limits<mz_uint>::max()) {
    diagnostic = "Gzip deflate payload is too large";
    return std::nullopt;
  }

  mz_stream stream{};
  if (mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
    diagnostic = "Could not initialize gzip decompression";
    return std::nullopt;
  }
  InflateEndGuard guard(stream);
  stream.next_in =
      reinterpret_cast<const unsigned char *>(input.data() + *payloadOffset);
  stream.avail_in = static_cast<mz_uint>(payloadSize);

  std::vector<std::byte> output;
  output.reserve(std::min(maximumOutputBytes, kChunkSize));
  std::array<std::byte, kChunkSize> chunk{};
  for (;;) {
    const std::size_t remaining = maximumOutputBytes - output.size();
    if (remaining == 0) {
      // Give inflate one byte so an exactly-full stream can still report END.
      std::byte sentinel{};
      stream.next_out = reinterpret_cast<unsigned char *>(&sentinel);
      stream.avail_out = 1;
      const int status = mz_inflate(&stream, MZ_FINISH);
      if (status == MZ_STREAM_END && stream.avail_out == 1) {
        break;
      }
      diagnostic = "Gzip output exceeds the configured limit";
      return std::nullopt;
    }

    const std::size_t capacity = std::min(remaining, chunk.size());
    stream.next_out = reinterpret_cast<unsigned char *>(chunk.data());
    stream.avail_out = static_cast<mz_uint>(capacity);
    const int status = mz_inflate(&stream, MZ_NO_FLUSH);
    const std::size_t produced = capacity - stream.avail_out;
    output.insert(output.end(), chunk.begin(), chunk.begin() + produced);
    if (status == MZ_STREAM_END) {
      break;
    }
    if (status != MZ_OK || (produced == 0 && stream.avail_in == 0)) {
      diagnostic = "Gzip deflate payload is malformed or truncated";
      return std::nullopt;
    }
  }

  if (stream.total_in != payloadSize) {
    diagnostic = "Gzip stream contains trailing deflate data";
    return std::nullopt;
  }
  const std::uint32_t expectedCrc = readLittleEndian32(input, trailerOffset);
  const std::uint32_t expectedSize =
      readLittleEndian32(input, trailerOffset + 4);
  if (crc32(output) != expectedCrc) {
    diagnostic = "Gzip CRC does not match decompressed data";
    return std::nullopt;
  }
  if (static_cast<std::uint32_t>(output.size()) != expectedSize) {
    diagnostic = "Gzip uncompressed size does not match its trailer";
    return std::nullopt;
  }
  return output;
}

} // namespace replay
