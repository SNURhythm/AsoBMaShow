#include "view/ImageFileDecoder.h"
#include "view/ImageRowReducer.h"
#include "scene/play/GameplayBmsResourceAvailability.h"
#include "support/AllocationFailure.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"

#include <array>
#include <algorithm>
#include <stb_image.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

std::vector<std::byte> readBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() <= 0) return {};
  std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return input ? bytes : std::vector<std::byte>{};
}

void expectMemoryFileEquivalent(const std::filesystem::path &path,
                                const char *format) {
  const auto bytes = readBytes(path);
  image_decode::ImageDecodeOptions options{.maximumDimension = 40,
                                           .maximumEncodedBytes = 1024U * 1024U,
                                           .maximumDecodedBytes = 3200};
  const auto file = image_decode::decodeImageFile(path, options);
  const auto memory = image_decode::decodeImageMemory(bytes, options);
  expect(file && memory && file->width == memory->width &&
             file->height == memory->height && file->rgba && memory->rgba &&
             *file->rgba == *memory->rgba,
         format);
}

void expectLibGdxCimDecode(const char *format,
                           std::initializer_list<unsigned char> encoded,
                           std::array<unsigned char, 4> expectedRgba) {
  std::vector<std::byte> bytes;
  bytes.reserve(encoded.size());
  for (const unsigned char value : encoded) {
    bytes.push_back(static_cast<std::byte>(value));
  }
  const auto decoded = image_decode::decodeImageMemory(
      bytes, {.maximumDimension = 16,
              .maximumEncodedBytes = 1024,
              .maximumDecodedBytes = 1024});
  const bool exact = decoded && decoded->width == 1 && decoded->height == 1 &&
                     decoded->rgba && decoded->rgba->size() == expectedRgba.size() &&
                     std::equal(decoded->rgba->begin(), decoded->rgba->end(),
                                expectedRgba.begin(),
                                [](unsigned char actual, unsigned char expected) {
                                  return actual == expected;
                                });
  expect(exact, format);
}

void verifyOptionalCimTree() {
  const char *root = std::getenv("ASOBMASHOW_CIM_TEST_ROOT");
  if (!root || *root == '\0') return;
  const auto resource = std::filesystem::path(root) / "Play/parts/graph/main.cim";
  const auto decoded = image_decode::decodeImageFile(
      resource, {.maximumDimension = 8192,
                 .maximumEncodedBytes = 32U * 1024U * 1024U,
                 .maximumDecodedBytes = 128U * 1024U * 1024U});
  expect(decoded && decoded->valid() && decoded->width == 2048 &&
             decoded->height == 2048,
         "LITONE12 graph/main.cim decodes through the gameplay resource policy");
}

void testWebpFfmpegFallback() {
  constexpr std::array<unsigned char, 68> webp = {
      0x52, 0x49, 0x46, 0x46, 0x3c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42,
      0x50, 0x56, 0x50, 0x38, 0x20, 0x30, 0x00, 0x00, 0x00, 0xd0, 0x01,
      0x00, 0x9d, 0x01, 0x2a, 0x02, 0x00, 0x02, 0x00, 0x02, 0x00, 0x34,
      0x25, 0xa0, 0x02, 0x74, 0xba, 0x01, 0xf8, 0x00, 0x03, 0xb0, 0x00,
      0xfe, 0xf0, 0xc4, 0x0b, 0xff, 0x20, 0xb9, 0x61, 0x75, 0xc8, 0xd7,
      0xff, 0x20, 0x3f, 0xe4, 0x07, 0xfc, 0x80, 0xff, 0xf8, 0xf2, 0x00,
      0x00, 0x00};
  std::vector<std::byte> encoded;
  encoded.reserve(webp.size());
  for (const unsigned char byte : webp) {
    encoded.push_back(static_cast<std::byte>(byte));
  }
  const auto memoryDecoded = image_decode::decodeImageMemory(
      encoded, {.maximumDimension = 16, .maximumEncodedBytes = 1024,
                .maximumDecodedBytes = 1024});
  expect(memoryDecoded && memoryDecoded->width == 2 &&
             memoryDecoded->height == 2,
         "WebP FFmpeg fallback decodes the bounded byte source used by "
         "virtual resources");
  const auto memoryResized = image_decode::decodeImageMemory(
      encoded, {.maximumDimension = 16,
                .maximumEncodedBytes = 1024,
                .maximumDecodedBytes = 1024,
                .targetWidth = 1,
                .targetHeight = 1});
  expect(memoryResized && memoryResized->width == 1 &&
             memoryResized->height == 1,
         "WebP FFmpeg byte fallback applies the requested target-size fit");
  expect(!image_decode::decodeImageMemory(
             encoded, {.maximumDimension = 1, .maximumEncodedBytes = 1024,
                       .maximumDecodedBytes = 1024}),
         "WebP FFmpeg byte fallback rejects dimensions over the decode limit");
  const auto path = std::filesystem::temp_directory_path() /
                    "asobmashow-image-decoder-fallback.webp";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(webp.data()),
                 static_cast<std::streamsize>(webp.size()));
  }
  const auto decoded = image_decode::decodeImageFile(
      path, {.maximumDimension = 16, .maximumEncodedBytes = 1024,
             .maximumDecodedBytes = 1024});
  expect(decoded && decoded->width == 2 && decoded->height == 2,
         "WebP uses the PixmapResourcePool-compatible FFmpeg fallback");
  const auto resized = image_decode::decodeImageFile(
      path, {.maximumDimension = 16,
             .maximumEncodedBytes = 1024,
             .maximumDecodedBytes = 1024,
             .targetWidth = 1,
             .targetHeight = 1});
  expect(resized && resized->width == 1 && resized->height == 1,
         "WebP FFmpeg fallback applies the requested target-size fit");
  std::error_code error;
  std::filesystem::remove(path, error);
}

void appendBigEndian(std::vector<std::byte> &bytes, std::uint32_t value) {
  for (int shift : {24, 16, 8, 0}) bytes.push_back(std::byte((value >> shift) & 255));
}

void appendPngChunk(std::vector<std::byte> &png, const char *type,
                    std::span<const std::byte> payload) {
  appendBigEndian(png, static_cast<std::uint32_t>(payload.size()));
  const auto start = png.size();
  for (int i = 0; i < 4; ++i) png.push_back(std::byte(type[i]));
  png.insert(png.end(), payload.begin(), payload.end());
  appendBigEndian(png, static_cast<std::uint32_t>(mz_crc32(
      0, reinterpret_cast<const unsigned char *>(png.data() + start), payload.size() + 4)));
}

std::vector<std::byte> makeLargePng(int width, int height) {
  std::vector<std::byte> png{std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
                            std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
  std::vector<std::byte> header;
  appendBigEndian(header, width);
  appendBigEndian(header, height);
  for (int value : {8, 6, 0, 0, 0}) header.push_back(std::byte(value));
  appendPngChunk(png, "IHDR", header);
  mz_stream stream{};
  mz_deflateInit(&stream, 1);
  std::vector<unsigned char> row(static_cast<std::size_t>(width) * 4 + 1);
  std::array<unsigned char, 16384> compressed{};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      row[x * 4 + 1] = x < width / 2 ? 240 : 16;
      row[x * 4 + 2] = y < height / 2 ? 32 : 224;
      row[x * 4 + 3] = 64;
      row[x * 4 + 4] = 128;
    }
    stream.next_in = row.data();
    stream.avail_in = static_cast<mz_uint>(row.size());
    int status;
    do {
      stream.next_out = compressed.data();
      stream.avail_out = compressed.size();
      status = mz_deflate(&stream, y + 1 == height ? MZ_FINISH : MZ_NO_FLUSH);
      appendPngChunk(png, "IDAT", std::as_bytes(std::span(compressed)).first(
                                    compressed.size() - stream.avail_out));
    } while (stream.avail_in || (y + 1 == height && status != MZ_STREAM_END));
  }
  mz_deflateEnd(&stream);
  appendPngChunk(png, "IEND", {});
  return png;
}

void testLargePngDownsamplesWithoutSourceAllocation() {
  const bool stress = std::getenv("ASOBMS_IMAGE_STRESS") != nullptr;
  const int sourceSize = stress ? 16384 : 4096;
  const int targetSize = stress ? 2048 : 128;
  const auto png = makeLargePng(sourceSize, sourceSize);
  test_support::AllocationSizeObserver allocations;
  const auto decoded = image_decode::decodeImageMemory(
      png, {.targetWidth = targetSize, .targetHeight = targetSize});
  expect(decoded && decoded->width == targetSize && decoded->height == targetSize,
         "large PNG remains accepted and fits the requested dimensions");
  expect(allocations.largest() <= std::max<std::size_t>(1024U * 1024U, targetSize * targetSize * 4),
         "PNG reduction never materializes the 64 MiB source pixel buffer");
  if (decoded) {
    for (int y : {0, targetSize - 1}) for (int x : {0, targetSize - 1}) {
      const auto offset = (y * targetSize + x) * 4;
      expect((*decoded->rgba)[offset] == (x == 0 ? 240 : 16) &&
                 (*decoded->rgba)[offset + 1] == (y == 0 ? 32 : 224) &&
                 (*decoded->rgba)[offset + 2] == 64 && (*decoded->rgba)[offset + 3] == 128,
             "streamed PNG preserves colors, alpha, and orientation");
    }
  }
}

std::vector<std::byte> makeVariantPng(int depth, int color, bool interlaced) {
  constexpr int width = 17, height = 13;
  const int channels = color == 3 ? 1 : (color & 2 ? 3 : 1) + (color & 4 ? 1 : 0);
  std::vector<std::byte> png{std::byte{137}, std::byte{80}, std::byte{78}, std::byte{71},
                            std::byte{13}, std::byte{10}, std::byte{26}, std::byte{10}};
  std::vector<std::byte> header;
  appendBigEndian(header, width);
  appendBigEndian(header, height);
  for (int value : {depth, color, 0, 0, interlaced ? 1 : 0}) header.push_back(std::byte(value));
  appendPngChunk(png, "IHDR", header);
  if (color == 3) {
    std::vector<std::byte> palette, alpha;
    for (int i = 0; i < 1 << depth; ++i) {
      for (int channel = 0; channel < 3; ++channel) palette.push_back(std::byte((i * 31 + channel * 17) & 255));
      alpha.push_back(std::byte((i * 61) & 255));
    }
    appendPngChunk(png, "PLTE", palette);
    appendPngChunk(png, "tRNS", alpha);
  } else if (!(color & 4)) {
    std::vector<std::byte> transparency;
    for (int channel = 0; channel < channels; ++channel) {
      const unsigned value = (channel * 37) & ((1U << depth) - 1);
      transparency.push_back(std::byte(value >> 8));
      transparency.push_back(std::byte(value & 255));
    }
    appendPngChunk(png, "tRNS", transparency);
  }
  std::vector<unsigned char> raw;
  const std::array<int, 7> xs{0, 4, 0, 2, 0, 1, 0}, ys{0, 0, 4, 0, 2, 0, 1};
  const std::array<int, 7> dx{8, 8, 4, 4, 2, 2, 1}, dy{8, 8, 8, 4, 4, 2, 2};
  for (int pass = 0; pass < (interlaced ? 7 : 1); ++pass) {
    const int startX = interlaced ? xs[pass] : 0, stepX = interlaced ? dx[pass] : 1;
    const int startY = interlaced ? ys[pass] : 0, stepY = interlaced ? dy[pass] : 1;
    const int count = (width - startX + stepX - 1) / stepX;
    const int stride = std::max(1, (channels * depth + 7) / 8);
    std::vector<unsigned char> previous((count * channels * depth + 7) / 8);
    for (int y = startY; y < height; y += stepY) {
      std::vector<unsigned char> row(previous.size());
      int sample = 0;
      for (int x = startX; x < width; x += stepX) for (int channel = 0; channel < channels; ++channel) {
        const unsigned value = (x * 271 + y * 173 + channel * 37) & ((1U << depth) - 1);
        const int bit = sample++ * depth;
        if (depth == 16) {
          row[bit / 8] = value >> 8;
          row[bit / 8 + 1] = value & 255;
        } else {
          row[bit / 8] |= value << (8 - depth - bit % 8);
        }
      }
      const int filter = y % 5;
      raw.push_back(filter);
      for (std::size_t i = 0; i < row.size(); ++i) {
        const int left = i >= static_cast<std::size_t>(stride) ? row[i - stride] : 0;
        const int above = previous[i], corner = i >= static_cast<std::size_t>(stride) ? previous[i - stride] : 0;
        const int estimate = left + above - corner;
        const int a = std::abs(estimate - left), b = std::abs(estimate - above), c = std::abs(estimate - corner);
        const int paeth = a <= b && a <= c ? left : b <= c ? above : corner;
        const int prediction = filter == 1 ? left : filter == 2 ? above : filter == 3 ?
                               (left + above) / 2 : filter == 4 ? paeth : 0;
        raw.push_back(static_cast<unsigned char>(row[i] - prediction));
      }
      previous = std::move(row);
    }
  }
  mz_ulong compressedSize = mz_compressBound(raw.size());
  std::vector<std::byte> compressed(compressedSize);
  mz_compress(reinterpret_cast<unsigned char *>(compressed.data()), &compressedSize, raw.data(), raw.size());
  compressed.resize(compressedSize);
  // Fragment even the zlib header/footer across IDATs, with empty chunks too.
  for (std::size_t i = 0; i < compressed.size(); ++i) {
    appendPngChunk(png, "IDAT", {});
    appendPngChunk(png, "IDAT", std::span(compressed).subspan(i, 1));
  }
  appendPngChunk(png, "IEND", {});
  return png;
}

void testPngVariantsAndInvalidInput() {
  for (int color : {0, 2, 3, 4, 6}) for (int depth : {1, 2, 4, 8, 16}) {
    if ((depth < 8 && color != 0 && color != 3) || (depth == 16 && color == 3)) continue;
    for (bool interlaced : {false, true}) {
      const auto png = makeVariantPng(depth, color, interlaced);
      int width = 0, height = 0, channels = 0;
      auto *reference = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(png.data()),
                                               png.size(), &width, &height, &channels, 4);
      const auto original = image_decode::decodeImageMemory(png, image_decode::ImageDecodeOptions{});
      expect(reference && original && original->width == width && original->height == height &&
                 std::equal(original->rgba->begin(), original->rgba->end(), reference),
             "PNG row decode matches stb for all depths/colors/filters, transparency and Adam7");
      const auto reduced = image_decode::decodeImageMemory(png, {.targetWidth = 5, .targetHeight = 5});
      expect(reduced && reduced->width == 5 && reduced->height == 3,
             "PNG row reduction preserves nonintegral aspect-ratio fit");
      if (reference && reduced) {
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) {
          std::array<unsigned, 4> sum{};
          unsigned count = 0;
          for (int sy = 0; sy < height; ++sy) for (int sx = 0; sx < width; ++sx) {
            const int dx = std::max(0, std::min((sx + 1) * 5, (x + 1) * width) - std::max(sx * 5, x * width));
            const int dy = std::max(0, std::min((sy + 1) * 3, (y + 1) * height) - std::max(sy * 3, y * height));
            for (int channel = 0; channel < 4; ++channel) sum[channel] += reference[(sy * width + sx) * 4 + channel] * dx * dy;
            count += dx * dy;
          }
          for (int channel = 0; channel < 4; ++channel) expect(
              (*reduced->rgba)[(y * 5 + x) * 4 + channel] == (sum[channel] + count / 2) / count,
              "PNG reduction averages every source pixel exactly once, including interlaced passes");
        }
      }
      if (interlaced) {
        test_support::AllocationSizeObserver allocations;
        const auto slight = image_decode::decodeImageMemory(png, {.targetWidth = 16, .targetHeight = 12});
        expect(slight && slight->width == 15 && slight->height == 12 && allocations.largest() <= 1024,
               "near-unity Adam7 reduction chooses source storage over larger accumulators");
      }
      stbi_image_free(reference);
    }
  }
  auto png = makeVariantPng(8, 6, false);
  png[29] ^= std::byte{1};
  expect(!image_decode::decodeImageMemory(png, {.targetWidth = 5, .targetHeight = 5}),
         "PNG checksum corruption does not publish partial pixels");
  png = makeVariantPng(8, 6, true);
  png.resize(png.size() - 16);
  expect(!image_decode::decodeImageMemory(png, {.targetWidth = 5, .targetHeight = 5}),
         "truncated PNG data is rejected after downsampling");
  png = makeVariantPng(8, 6, false);
  expect(!image_decode::decodeImageMemory(png, {.maximumDecodedBytes = 17 * 13 * 4 - 1,
                                              .targetWidth = 5, .targetHeight = 5}),
         "explicit library source-byte limits remain enforced before streaming decode");
}

void testPngRejectsInvalidInflatedRows() {
  for (const auto &raw : {std::vector<unsigned char>{5, 255, 0, 0, 255},
                          std::vector<unsigned char>{0, 255, 0, 0},
                          std::vector<unsigned char>{0, 255, 0, 0, 255, 99}}) {
    auto templatePng = makeVariantPng(8, 6, false);
    std::vector<std::byte> png(templatePng.begin(), templatePng.begin() + 8), header;
    appendBigEndian(header, 1);
    appendBigEndian(header, 1);
    for (int value : {8, 6, 0, 0, 0}) header.push_back(std::byte(value));
    appendPngChunk(png, "IHDR", header);
    mz_ulong size = mz_compressBound(raw.size());
    std::vector<std::byte> compressed(size);
    mz_compress(reinterpret_cast<unsigned char *>(compressed.data()), &size, raw.data(), raw.size());
    compressed.resize(size);
    appendPngChunk(png, "IDAT", compressed);
    appendPngChunk(png, "IEND", {});
    const auto decoded = image_decode::decodeImageMemory(png, image_decode::ImageDecodeOptions{});
    if (raw.size() > 5) {
      expect(decoded && (*decoded->rgba)[0] == 255 && (*decoded->rgba)[3] == 255,
             "PNG trailing inflated padding remains accepted without retaining it");
    } else {
      expect(!decoded, "CRC-valid PNG with invalid filter or short inflated pixels is rejected");
    }
  }
}

void testPngTrailingOutputIsBounded() {
  for (std::size_t padding : {0U, 1U, 65536U, 65537U, 4U * 1024U * 1024U}) {
    const auto templatePng = makeVariantPng(8, 6, false);
    std::vector<std::byte> png(templatePng.begin(), templatePng.begin() + 8), header;
    appendBigEndian(header, 1);
    appendBigEndian(header, 1);
    for (int value : {8, 6, 0, 0, 0}) header.push_back(std::byte(value));
    appendPngChunk(png, "IHDR", header);
    std::vector<unsigned char> raw(5 + padding, 0);
    raw[1] = raw[4] = 255;
    mz_ulong size = mz_compressBound(raw.size());
    std::vector<std::byte> compressed(size);
    expect(mz_compress(reinterpret_cast<unsigned char *>(compressed.data()), &size,
                       raw.data(), raw.size()) == MZ_OK, "padding fixture compresses");
    compressed.resize(size);
    // Force completion/padding accounting across IDAT boundaries.
    const auto midpoint = compressed.size() / 2;
    appendPngChunk(png, "IDAT", std::span(compressed).first(midpoint));
    appendPngChunk(png, "IDAT", std::span(compressed).subspan(midpoint));
    appendPngChunk(png, "IEND", {});
    const auto decoded = image_decode::decodeImageMemory(png, image_decode::ImageDecodeOptions{});
    expect(decoded.has_value() == (padding <= 65536),
           ("PNG padding admission matches allowance: " + std::to_string(padding)).c_str());
    if (decoded) expect((*decoded->rgba)[0] == 255 && (*decoded->rgba)[3] == 255,
                        "padding does not change image pixels");
    if (padding == 65536) {
      // A bounded drain must still check the zlib checksum at the exact boundary.
      compressed.back() ^= std::byte{1};
      png.resize(33);
      appendPngChunk(png, "IDAT", compressed);
      appendPngChunk(png, "IEND", {});
      expect(!image_decode::decodeImageMemory(png, image_decode::ImageDecodeOptions{}),
             "PNG padding boundary still validates zlib completion and checksum");
    }
  }
}

void testDeclaredPngOverflowAndFractionalReduction() {
  auto png = makeVariantPng(8, 6, true);
  std::vector<std::byte> crafted(png.begin(), png.begin() + 8), header;
  appendBigEndian(header, 1);
  appendBigEndian(header, INT_MAX);
  for (int value : {1, 0, 0, 0, 1}) header.push_back(std::byte(value));
  appendPngChunk(crafted, "IHDR", header);
  const std::array<std::byte, 8> emptyZlib{std::byte{0x78}, std::byte{0x9c}, std::byte{0x03}, std::byte{0},
                                        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  appendPngChunk(crafted, "IDAT", emptyZlib);
  appendPngChunk(crafted, "IEND", {});
  expect(!image_decode::decodeImageMemory(crafted,
             {.maximumDimension = INT_MAX, .maximumDecodedBytes = SIZE_MAX,
              .targetWidth = 1, .targetHeight = 1}),
         "extreme Adam7 declared extents reject missing pixels without signed overflow");

  // A 3x1 WBMP white/black/white stripe reduced to two pixels has equal
  // coverage on each side (170). Whole-pixel bucketing would yield 128/255.
  const std::array<std::byte, 5> stripe{std::byte{0}, std::byte{0}, std::byte{3}, std::byte{1}, std::byte{0x40}};
  const auto reduced = image_decode::decodeImageMemory(stripe, {.targetWidth = 2, .targetHeight = 1});
  expect(reduced && reduced->width == 2 && reduced->height == 1 &&
             (*reduced->rgba)[0] == 170 && (*reduced->rgba)[4] == 170,
         "fractional area reduction preserves symmetric thin features");
}

std::vector<std::byte> makeLargeCim(int size) {
  std::vector<std::byte> header;
  appendBigEndian(header, size);
  appendBigEndian(header, size);
  appendBigEndian(header, 4);
  std::vector<unsigned char> row(static_cast<std::size_t>(size) * 4, 128);
  std::array<unsigned char, 16384> output;
  std::vector<std::byte> encoded;
  mz_stream stream{};
  mz_deflateInit(&stream, 1);
  for (int y = -1; y < size; ++y) {
    stream.next_in = y < 0 ? reinterpret_cast<const unsigned char *>(header.data()) : row.data();
    stream.avail_in = y < 0 ? header.size() : row.size();
    int status;
    do {
      stream.next_out = output.data();
      stream.avail_out = output.size();
      status = mz_deflate(&stream, y + 1 == size ? MZ_FINISH : MZ_NO_FLUSH);
      const auto bytes = std::as_bytes(std::span(output)).first(output.size() - stream.avail_out);
      encoded.insert(encoded.end(), bytes.begin(), bytes.end());
    } while (stream.avail_in || (y + 1 == size && status != MZ_STREAM_END));
  }
  mz_deflateEnd(&stream);
  return encoded;
}

void testInterlacedReductionCancellation() {
  std::stop_source stop;
  image_decode::detail::ImageRowReducer reducer(1024, 1024,
      {.targetWidth = 1000, .targetHeight = 1000, .stop = stop.get_token()}, true);
  for (int y = 0; y < 1024; ++y) for (int x = 0; x < 1024; ++x) reducer.add(x, y, {1, 2, 3, 4});
  stop.request_stop();
  expect(!reducer.finish(), "cancelled interlaced reduction never publishes buffered pixels");
}

void testLargeLegacyImagesUseRows() {
  const auto cim = makeLargeCim(4096);
  {
    test_support::AllocationSizeObserver allocations;
    const auto reduced = image_decode::decodeImageMemory(cim, {.targetWidth = 128, .targetHeight = 128});
    expect(reduced && reduced->width == 128 && reduced->height == 128 &&
               std::ranges::all_of(*reduced->rgba, [](auto value) { return value == 128; }) &&
               allocations.largest() <= 1024U * 1024U,
           "large CIM inflates/converts rows without a full native or RGBA image");
  }
  std::vector<std::byte> wbmp{std::byte{0}, std::byte{0}, std::byte{0xa0}, std::byte{0},
                             std::byte{0xa0}, std::byte{0}}; // 4096 x 4096
  wbmp.resize(6 + 4096 * 512, std::byte{0xaa});
  {
    test_support::AllocationSizeObserver allocations;
    const auto reduced = image_decode::decodeImageMemory(wbmp, {.targetWidth = 128, .targetHeight = 128});
    expect(reduced && reduced->width == 128 && reduced->height == 128 &&
               (*reduced->rgba)[0] == 128 && (*reduced->rgba)[3] == 255 &&
               allocations.largest() <= 1024U * 1024U,
           "large WBMP reduces packed pixels without a full RGBA image");
  }
}

bool waitForProbe(const gameplay::BmsResourceImageAvailabilityProbe &probe) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!probe.complete() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return probe.complete();
}

void testGameplayBmsResourceProbeHandlesPathAllocationFailure() {
  bms_parser::ChartMeta meta;
  meta.BmsPath = std::filesystem::path("charts") /
                 std::string(128, 'a') / "chart.bms";
  const std::filesystem::path declaredPath = "stage.png";
  std::atomic_int decodeCalls{0};
  const auto decode = [&](const std::filesystem::path &, std::stop_token) {
    ++decodeCalls;
    return true;
  };
  gameplay::BmsResourceImageAvailabilityProbe::Decode failedDecode = decode;
  gameplay::BmsResourceImageAvailabilityProbe probe;
  {
    test_support::FailNextAllocation failure;
    probe.start(meta, declaredPath, std::move(failedDecode));
  }
  expect(probe.complete() && !probe.available() && decodeCalls.load() == 0,
         "path allocation failure completes the probe without decoding");
  probe.start(meta, declaredPath, decode);
  expect(waitForProbe(probe) && probe.available() && decodeCalls.load() == 1,
         "a probe can retry after path allocation failure");
}

void testGameplayBmsResourceProbePublishesDecodedAvailabilityOffThread() {
  const auto resources = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
                         "tests/fixtures/beatoraja_skin/resources";
  const auto temporary =
      std::filesystem::temp_directory_path() /
      ("asobmashow-gameplay-image-probe-" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()));
  std::filesystem::create_directories(temporary);
  std::filesystem::copy_file(resources / "fixture.png",
                             temporary / "fixture.png");
  {
    std::ofstream(temporary / "chart.bms") << "#TITLE probe\n";
    std::ofstream(temporary / "corrupt-stage.png", std::ios::binary)
        << "not an encoded image";
  }
  bms_parser::ChartMeta meta;
  meta.BmsPath = temporary / "chart.bms";
  const auto decode = [](const std::filesystem::path &path,
                         std::stop_token stop) {
    return image_decode::decodeImageFile(
               path, {.maximumDimension = 40,
                      .maximumEncodedBytes = 1024U * 1024U,
                      .maximumDecodedBytes = 3200,
                      .stop = stop})
        .has_value();
  };

  const auto callerThread = std::this_thread::get_id();
  std::atomic_bool decodedOffCallerThread{false};
  gameplay::BmsResourceImageAvailabilityProbe valid;
  valid.start(meta, "fixture.png", [&](const std::filesystem::path &path,
                                        std::stop_token stop) {
    decodedOffCallerThread.store(std::this_thread::get_id() != callerThread,
                                 std::memory_order_release);
    return decode(path, stop);
  });
  expect(waitForProbe(valid) && valid.available() &&
             decodedOffCallerThread.load(std::memory_order_acquire),
         "gameplay BMS image availability publishes a successful decode "
         "without blocking the gameplay caller");

  gameplay::BmsResourceImageAvailabilityProbe invalid;
  invalid.start(meta, "corrupt-stage.png", decode);
  expect(waitForProbe(invalid) && !invalid.available(),
         "gameplay BMS image availability stays false when an existing "
         "resource cannot be decoded");

  std::atomic_bool cancellationStarted{false};
  std::atomic_bool cancellationObserved{false};
  {
    gameplay::BmsResourceImageAvailabilityProbe cancelled;
    cancelled.start(meta, "fixture.png",
                    [&](const std::filesystem::path &, std::stop_token stop) {
                      cancellationStarted.store(true,
                                                std::memory_order_release);
                      while (!stop.stop_requested()) {
                        std::this_thread::yield();
                      }
                      cancellationObserved.store(true,
                                                 std::memory_order_release);
                      return false;
                    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!cancellationStarted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
  }
  expect(cancellationObserved.load(std::memory_order_acquire),
         "destroying a gameplay BMS image probe cancels its decoder worker");

  std::error_code error;
  std::filesystem::remove_all(temporary, error);
}

}

#include "image_reduced_formats_tests.h"
#include "psd_row_decoder_tests.h"
#include "portable_row_decoder_tests.h"
#include "raster_row_decoder_tests.h"

int main() {
  if (std::getenv("ASOBMS_JPEG_STRESS")) {
    testJpegDecodeTimeScaling(true);
    return failures == 0 ? 0 : 1;
  }
  testJpegDecodeTimeScaling();
  testGifAndPicRowDecoders();
  testPsdRowDecoders();
  testPortableRowDecoders();
  testRasterRowDecoders();
  testLargePngDownsamplesWithoutSourceAllocation();
  testPngVariantsAndInvalidInput();
  testPngRejectsInvalidInflatedRows();
  testPngTrailingOutputIsBounded();
  testDeclaredPngOverflowAndFractionalReduction();
  testLargeLegacyImagesUseRows();
  testInterlacedReductionCancellation();
  testGameplayBmsResourceProbeHandlesPathAllocationFailure();
  const auto resources = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
                         "tests/fixtures/beatoraja_skin/resources";
  const auto png = resources / "fixture.png";
  const auto jpg = resources / "fixture.jpg";
  const auto pngBytes = readBytes(png);
  expectMemoryFileEquivalent(png, "PNG memory and file decodes are equivalent");
  expectMemoryFileEquivalent(jpg, "JPEG memory and file decodes are equivalent");

  image_decode::ImageDecodeOptions resized{.maximumDimension = 40,
                                           .maximumEncodedBytes = 1024U * 1024U,
                                           .maximumDecodedBytes = 3200,
                                           .targetWidth = 10,
                                           .targetHeight = 10};
  const auto decoded = image_decode::decodeImageFile(png, resized);
  expect(decoded && decoded->width == 10 && decoded->height == 5,
         "shared decoder applies target-size fit after bounded decode");
  const auto full = image_decode::decodeImageMemory(
      pngBytes, {.maximumDimension = 40, .maximumEncodedBytes = 1024U * 1024U,
                 .maximumDecodedBytes = 3200});
  const auto resizedByHelper = image_decode::resizeDecodedImage(*full, resized);
  expect(resizedByHelper && resizedByHelper->width == 10 &&
             resizedByHelper->height == 5,
         "shared resize helper applies the same target-size fit");

  const std::vector<std::byte> twoByTwoCim = {
      std::byte{0x78}, std::byte{0x9c}, std::byte{0x63}, std::byte{0x60},
      std::byte{0x60}, std::byte{0x60}, std::byte{0x62}, std::byte{0x80},
      std::byte{0x60}, std::byte{0xe6}, std::byte{0xff}, std::byte{0x40},
      std::byte{0x02}, std::byte{0x8c}, std::byte{0x81}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x1e}, std::byte{0x6d}, std::byte{0x06},
      std::byte{0x02}};
  const auto resizedCim = image_decode::decodeImageMemory(
      twoByTwoCim, {.maximumDimension = 40,
                    .maximumEncodedBytes = 1024,
                    .maximumDecodedBytes = 3200,
                    .targetWidth = 1,
                    .targetHeight = 1});
  expect(resizedCim && resizedCim->width == 1 && resizedCim->height == 1,
         "LibGDX CIM decoding applies the requested target-size fit");

  expect(!image_decode::decodeImageMemory(
             pngBytes, {.maximumDimension = 19, .maximumEncodedBytes = 1024U * 1024U,
                        .maximumDecodedBytes = 3200}),
         "shared decoder rejects decoded dimensions before allocation");
  expect(!image_decode::decodeImageMemory(
             pngBytes, {.maximumDimension = 40, .maximumEncodedBytes = 1024U * 1024U,
                        .maximumDecodedBytes = 799}),
         "shared decoder rejects decoded byte bounds before allocation");
  expect(!image_decode::decodeImageMemory(
             pngBytes, {.maximumDimension = 40, .maximumEncodedBytes = 16,
                        .maximumDecodedBytes = 3200}),
         "shared decoder rejects encoded memory bytes before probing");

  std::stop_source stopped;
  stopped.request_stop();
  resized.stop = stopped.get_token();
  expect(!image_decode::decodeImageMemory(pngBytes, resized),
         "shared decoder observes a pre-probe memory stop request");
  expect(!image_decode::decodeImageFile(png, resized),
         "shared decoder observes a pre-acquisition stop request");
  resized.stop = {};
  resized.maximumEncodedBytes = 16;
  expect(!image_decode::decodeImageFile(png, resized),
         "shared decoder rejects encoded bytes before file-buffer allocation");

  // These are LibGDX PixmapIO.writeCIM streams. Beatoraja loads a .cim
  // resource through PixmapIO.readCIM, including every documented GDX2D
  // pixmap format, before making a texture from the resulting pixmap.
  expectLibGdxCimDecode(
      "LibGDX CIM Alpha converts to opaque-white RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x62, 0x05, 0x00,
       0x00, 0x3f, 0x00, 0x24},
      {0xff, 0xff, 0xff, 0x20});
  expectLibGdxCimDecode(
      "LibGDX CIM LuminanceAlpha converts to RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x60, 0x26, 0x41,
       0x25, 0x00, 0x00, 0x6a, 0x00, 0x38},
      {0x11, 0x11, 0x11, 0x22});
  expectLibGdxCimDecode(
      "LibGDX CIM RGB888 converts to opaque RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x60, 0xe6, 0x55,
       0xbb, 0xcf, 0x00, 0x00, 0x04, 0x6f, 0x02, 0x37},
      {0xaa, 0xbb, 0xcc, 0xff});
  expectLibGdxCimDecode(
      "LibGDX CIM RGBA8888 preserves RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x60, 0x16, 0x41,
       0x25, 0x63, 0x17, 0x00, 0x01, 0x8e, 0x00, 0xb1},
      {0x11, 0x22, 0x33, 0x44});
  expectLibGdxCimDecode(
      "LibGDX CIM RGB565 converts to opaque RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x60, 0x56, 0x86,
       0x1f, 0x00, 0x01, 0x27, 0x01, 0x00},
      {0xff, 0x00, 0x00, 0xff});
  expectLibGdxCimDecode(
      "LibGDX CIM RGBA4444 converts to RGBA",
      {0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x64, 0x80, 0x60, 0x36, 0x13,
       0x21, 0x00, 0x00, 0xac, 0x00, 0x4f},
      {0x11, 0x22, 0x33, 0x44});

  // WBMP is the standard JDK ImageIO reader not covered by LibGDX's native
  // Pixmap loaders or stb. PixmapResourcePool reaches it on the stagefile and
  // backbmp fallback path. This is a 2x1 Type-0 WBMP with one packed pixel
  // byte.
  const std::vector<std::byte> wbmp = {
      std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
      std::byte{0x01}, std::byte{0x80}};
  const auto decodedWbmp = image_decode::decodeImageMemory(
      wbmp, {.maximumDimension = 16, .maximumEncodedBytes = 1024,
             .maximumDecodedBytes = 1024});
  expect(decodedWbmp && decodedWbmp->valid() && decodedWbmp->width == 2 &&
             decodedWbmp->height == 1,
         "standard JDK ImageIO WBMP fallback decodes BMS image resources");
  verifyOptionalCimTree();
  testWebpFfmpegFallback();
  testGameplayBmsResourceProbePublishesDecodedAvailabilityOffThread();
  return failures == 0 ? 0 : 1;
}
