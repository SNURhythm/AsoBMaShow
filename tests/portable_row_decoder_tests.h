#pragma once

#include "view/PortableRowDecoder.h"
#include "support/AllocationFailure.h"

#include <stb_image.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::vector<std::byte> portableTestBytes(const std::string &text) {
  const auto bytes = std::as_bytes(std::span(text));
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> portableTestPnm(int channels, int depth,
                                      int width = 17, int height = 13) {
  auto bytes = portableTestBytes(std::string(channels == 1 ? "P5" : "P6") +
      "\t# comment before dimensions\r\n" + std::to_string(width) +
      " \v# width comment\n\t" + std::to_string(height) +
      "\f\r\n" + (depth == 16 ? "65535\n" : "255\n"));
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    for (int channel = 0; channel < channels; ++channel) {
      // Deliberately unequal high/low bytes detect changes to stb's PNM16
      // native-endian conversion as well as incorrect grayscale replication.
      if (depth == 16) bytes.push_back(std::byte((x * 19 + y * 13 + channel * 83) & 255));
      bytes.push_back(std::byte((x * 47 + y * 29 + channel * 61 + 10) & 255));
    }
  }
  return bytes;
}

std::array<unsigned char, 4> portableTestRgbe(int x, int y) {
  if (x < 3) x = 0; // A genuine repeated run followed by varying literals.
  return {static_cast<unsigned char>((x * 37 + y * 17 + 64) & 255),
          static_cast<unsigned char>((x * 13 + y * 41 + 16) & 255),
          static_cast<unsigned char>((x * 71 + y * 7 + 4) & 255),
          static_cast<unsigned char>((x + y) % 11 == 0 ? 0 : 124 + (x + y) % 10)};
}

std::vector<std::byte> portableTestHdr(int width, int height, bool rle) {
  auto bytes = portableTestBytes("#?RADIANCE\n# metadata\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
      std::to_string(height) + " +X " + std::to_string(width) + "\n");
  for (int y = 0; y < height; ++y) {
    if (!rle) {
      for (int x = 0; x < width; ++x) {
        for (const auto channel : portableTestRgbe(x, y)) bytes.push_back(std::byte(channel));
      }
      continue;
    }
    bytes.insert(bytes.end(), {std::byte{2}, std::byte{2},
        std::byte((width >> 8) & 255), std::byte(width & 255)});
    for (int channel = 0; channel < 4; ++channel) {
      // Both run and literal packets, including literal count 128 when wide.
      bytes.push_back(std::byte{131});
      bytes.push_back(std::byte(portableTestRgbe(0, y)[channel]));
      for (int x = 3; x < width;) {
        const int count = std::min(128, width - x);
        bytes.push_back(std::byte(count));
        for (int i = 0; i < count; ++i, ++x) {
          bytes.push_back(std::byte(portableTestRgbe(x, y)[channel]));
        }
      }
    }
  }
  return bytes;
}

using PortableTestDecode = std::optional<image_decode::DecodedImageData> (*)(
    std::span<const std::byte>, const image_decode::ImageDecodeOptions &);

void expectPortableMatchesStb(const std::vector<std::byte> &encoded,
                              PortableTestDecode decode, const char *message) {
  int width = 0, height = 0, channels = 0;
  const std::unique_ptr<unsigned char, decltype(&stbi_image_free)> reference(
      stbi_load_from_memory(reinterpret_cast<const unsigned char *>(encoded.data()),
                            static_cast<int>(encoded.size()), &width, &height,
                            &channels, 4), stbi_image_free);
  expect(reference != nullptr, "portable test fixture is accepted by bundled stb");
  if (!reference) return;
  for (const auto target : {std::pair{0, 0}, std::pair{5, 3}}) {
    const image_decode::ImageDecodeOptions options{
        .targetWidth = target.first, .targetHeight = target.second};
    image_decode::detail::ImageRowReducer expected(width, height, options);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
      expected.add(x, y, {reference.get()[offset], reference.get()[offset + 1],
                          reference.get()[offset + 2], reference.get()[offset + 3]});
    }
    const auto result = decode(encoded, options);
    const auto reducedReference = expected.finish();
    expect(result && reducedReference && result->width == reducedReference->width &&
               result->height == reducedReference->height && result->rgba &&
               *result->rgba == *reducedReference->rgba, message);
  }
}

void expectPortableLimits(const std::vector<std::byte> &encoded,
                          PortableTestDecode decode, int width, int height) {
  const auto sourceBytes = static_cast<std::size_t>(width) * height * 4;
  image_decode::ImageDecodeOptions options{
      .maximumDimension = std::max(width, height),
      .maximumEncodedBytes = encoded.size(),
      .maximumDecodedBytes = sourceBytes,
      .targetWidth = 3, .targetHeight = 2};
  expect(decode(encoded, options).has_value(),
         "portable source exactly at explicit admission limits remains accepted");
  options.maximumDimension--;
  expect(!decode(encoded, options), "portable source dimension limit is checked before reduction");
  options.maximumDimension++;
  options.maximumDecodedBytes--;
  expect(!decode(encoded, options), "portable decoded-byte limit applies to the source, not target");
  options.maximumDecodedBytes++;
  options.maximumEncodedBytes--;
  expect(!decode(encoded, options), "portable encoded-byte limit is enforced");
  options.maximumEncodedBytes++;
  std::stop_source stop;
  stop.request_stop();
  options.stop = stop.get_token();
  expect(!decode(encoded, options), "portable pre-cancelled decode never publishes pixels");
}

void testPortablePnmRows() {
  using image_decode::detail::decodePnmRows;
  for (int channels : {1, 3}) for (int depth : {8, 16}) {
    const auto encoded = portableTestPnm(channels, depth);
    expect(image_decode::detail::isPnm(encoded), "P5/P6 signatures route to the row decoder");
    expectPortableMatchesStb(encoded, decodePnmRows,
        "PNM rows match stb for comments, whitespace, grayscale, color and 16-bit samples");
    expectPortableLimits(encoded, decodePnmRows, 17, 13);
    for (std::size_t count : {std::size_t{0}, std::size_t{2}, encoded.size() - 1}) {
      expect(!decodePnmRows(std::span(encoded).first(count), {}),
             "PNM rejects truncated headers and pixel data");
    }
  }
  // MAXVAL does not rescale samples in bundled stb; a whitespace-valued first
  // pixel must not be accidentally consumed with the header separator.
  auto smallRange = portableTestBytes("P5\n3 1\n31\n");
  smallRange.insert(smallRange.end(), {std::byte{10}, std::byte{13}, std::byte{31}});
  const auto pixels = decodePnmRows(smallRange, {});
  expect(pixels && *pixels->rgba == std::vector<unsigned char>{10, 10, 10, 255,
             13, 13, 13, 255, 31, 31, 31, 255},
         "PNM retains raw samples and whitespace-valued raster bytes");
  expectPortableMatchesStb(smallRange, decodePnmRows, "PNM non-255 MAXVAL matches stb");
  for (const auto header : {"P5\n0 1\n255\n", "P6\n2147483648 1\n255\n",
                            "P5\n1 1\n65536\n", "P5\n1 1\n0\n"}) {
    expect(!decodePnmRows(portableTestBytes(header), {}),
           "PNM rejects zero, overflowed, or unsupported header values");
  }
}

void testPortableHdrRows() {
  using image_decode::detail::decodeHdrRows;
  for (int width : {1, 7, 8, 17, 257, 32768}) {
    const auto flat = portableTestHdr(width, 5, false);
    expect(image_decode::detail::isHdr(flat), "Radiance signature routes to the HDR row decoder");
    expectPortableMatchesStb(flat, decodeHdrRows,
        "flat HDR matches stb tone mapping, alpha, orientation and odd target reduction");
    if (width >= 8 && width < 32768) {
      const auto rle = portableTestHdr(width, 5, true);
      expectPortableMatchesStb(rle, decodeHdrRows,
          "HDR RLE runs and literal packets match stb before and after reduction");
    }
  }
  auto gamma = portableTestBytes("#?RGBE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n");
  gamma.insert(gamma.end(), {std::byte{64}, std::byte{16}, std::byte{4}, std::byte{128},
                            std::byte{255}, std::byte{255}, std::byte{255}, std::byte{0}});
  const auto result = decodeHdrRows(gamma, {});
  expect(result && *result->rgba == std::vector<unsigned char>{136, 72, 39, 255, 0, 0, 0, 255},
         "HDR applies gamma 2.2, rounds to nearest byte, and treats zero exponent as black");
  const auto averaged = decodeHdrRows(gamma, {.targetWidth = 1, .targetHeight = 1});
  expect(averaged && *averaged->rgba == std::vector<unsigned char>{68, 36, 20, 255},
         "HDR reduction averages tone-mapped bytes instead of linear radiance");
  for (bool rle : {false, true}) {
    const auto encoded = portableTestHdr(17, 13, rle);
    expectPortableLimits(encoded, decodeHdrRows, 17, 13);
    expect(!decodeHdrRows(std::span(encoded).first(encoded.size() - 1), {}),
           "HDR rejects truncated flat or RLE raster without partial pixels");
  }
  const auto rleHeader = portableTestBytes("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 8\n");
  for (const auto badPacket : {std::vector<std::byte>{std::byte{2}, std::byte{2}, std::byte{0}, std::byte{9}},
                              std::vector<std::byte>{std::byte{2}, std::byte{2}, std::byte{0}, std::byte{8}, std::byte{0}},
                              std::vector<std::byte>{std::byte{2}, std::byte{2}, std::byte{0}, std::byte{8}, std::byte{137}, std::byte{255}},
                              std::vector<std::byte>{std::byte{2}, std::byte{2}, std::byte{0}, std::byte{8}, std::byte{9}}}) {
    auto malformed = rleHeader;
    malformed.insert(malformed.end(), badPacket.begin(), badPacket.end());
    expect(!decodeHdrRows(malformed, {}), "HDR rejects wrong scanline width, zero counts and RLE overruns");
  }
  for (const auto header : {"#?RADIANCE\n\n-Y 1 +X 1\n", "#?RGBE\nFORMAT=32-bit_rle_rgbe\n",
                            "#?RGBE\nFORMAT=32-bit_rle_rgbe\n\n+Y 1 +X 1\n",
                            "#?RGBE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2147483648\n"}) {
    expect(!decodeHdrRows(portableTestBytes(header), {}), "HDR rejects malformed or unsupported headers");
  }
}

void testPortableAllocationAndCancellation() {
  const auto pnm = portableTestPnm(3, 16, 2048, 1024);
  const auto hdr = portableTestHdr(1024, 1024, true);
  for (const bool useHdr : {false, true}) {
    const auto &encoded = useHdr ? hdr : pnm;
    const PortableTestDecode decode = useHdr ? image_decode::detail::decodeHdrRows
                                             : image_decode::detail::decodePnmRows;
    {
      test_support::AllocationSizeObserver allocations;
      const auto result = decode(encoded, {.targetWidth = 63, .targetHeight = 31});
      expect(result && result->width <= 63 && result->height == 31,
             "large portable artwork is accepted and reduced to its requested target");
      expect(allocations.largest() <= 64U * 1024U,
             "portable reduction allocates no source-sized RGB, RGBA or float image");
    }
    std::stop_source stop;
    std::jthread cancel([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      stop.request_stop();
    });
    const auto cancelled = decode(encoded, {.targetWidth = 63, .targetHeight = 31,
                                            .stop = stop.get_token()});
    expect(!cancelled, "cancellation during a large portable decode publishes no partial image");
  }
}

void testPortableRowDecoders() {
  testPortablePnmRows();
  testPortableHdrRows();
  testPortableAllocationAndCancellation();
}

} // namespace
