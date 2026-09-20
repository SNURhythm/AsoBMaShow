#pragma once

#include "view/RasterRowDecoder.h"
#include "support/AllocationFailure.h"

#include <stb_image.h>

#include <array>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace {

void rasterFixturePut(std::vector<std::byte> &bytes, std::size_t offset,
                       std::uint32_t value, int count) {
  for (int i = 0; i < count; ++i) bytes[offset + i] = std::byte((value >> (i * 8)) & 255);
}

std::vector<std::byte> rasterFixtureBmp(int depth, bool topDown,
                                       bool bitfields = false, bool zeroAlpha = false) {
  constexpr unsigned width = 17, height = 13;
  const unsigned header = bitfields && depth == 32 ? 108 : 40;
  const unsigned maskBytes = bitfields && header == 40 ? 12 : 0;
  const unsigned paletteCount = depth <= 8 ? 1U << depth : 0;
  const unsigned offset = 14 + header + maskBytes + paletteCount * 4;
  const unsigned stride = ((width * depth + 31) / 32) * 4;
  std::vector<std::byte> bytes(offset + stride * height);
  bytes[0] = std::byte{'B'};
  bytes[1] = std::byte{'M'};
  rasterFixturePut(bytes, 2, bytes.size(), 4);
  rasterFixturePut(bytes, 10, offset, 4);
  rasterFixturePut(bytes, 14, header, 4);
  rasterFixturePut(bytes, 18, width, 4);
  rasterFixturePut(bytes, 22, topDown ? 0U - height : height, 4);
  rasterFixturePut(bytes, 26, 1, 2);
  rasterFixturePut(bytes, 28, depth, 2);
  rasterFixturePut(bytes, 30, bitfields ? 3 : 0, 4);
  rasterFixturePut(bytes, 34, stride * height, 4);
  rasterFixturePut(bytes, 46, paletteCount, 4);
  if (bitfields) {
    const std::array<std::uint32_t, 4> masks = depth == 16
        ? std::array<std::uint32_t, 4>{0xf800, 0x7e0, 0x1f, 0}
        : std::array<std::uint32_t, 4>{0xff, 0xff00, 0xff0000, 0xff000000};
    for (int i = 0; i < (depth == 16 ? 3 : 4); ++i) rasterFixturePut(bytes, 54 + i * 4, masks[i], 4);
  }
  for (unsigned i = 0; i < paletteCount; ++i) {
    const auto entry = 14 + header + maskBytes + i * 4;
    for (unsigned c = 0; c < 4; ++c) bytes[entry + c] = std::byte((i * 37 + c * 59) & 255);
  }
  for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
    const unsigned row = offset + y * stride;
    if (depth <= 8) {
      const auto index = (x * 7 + y * 11 + x * y) % paletteCount;
      bytes[row + x * depth / 8] |= std::byte(index << (8 - depth - (x * depth) % 8));
    } else if (depth == 16) {
      const std::uint32_t packed = (x * 2111 + y * 3157 + 457) & (bitfields ? 0xffff : 0x7fff);
      rasterFixturePut(bytes, row + x * 2, packed, 2);
    } else {
      for (int c = 0; c < depth / 8; ++c) {
        bytes[row + x * (depth / 8) + c] = std::byte(c == 3 && zeroAlpha ? 0 :
            (x * 23 + y * 47 + c * 71 + x * y * 3) & 255);
      }
    }
  }
  return bytes;
}

std::vector<std::byte> rasterFixtureTga(int depth, bool rle, unsigned descriptor,
                                       int paletteDepth = 0, bool gray = false) {
  constexpr int width = 17, height = 13, paletteCount = 16;
  const bool indexed = paletteDepth != 0;
  const unsigned paletteStride = (paletteDepth + 7) / 8;
  std::vector<std::byte> bytes(18 + 3 + (indexed ? paletteCount * paletteStride : 0));
  bytes[0] = std::byte{3}; // An image ID must be skipped before the palette/pixels.
  bytes[1] = std::byte(indexed);
  bytes[2] = std::byte((indexed ? 1 : gray ? 3 : 2) + (rle ? 8 : 0));
  rasterFixturePut(bytes, 5, indexed ? paletteCount : 0, 2);
  bytes[7] = std::byte(paletteDepth);
  rasterFixturePut(bytes, 12, width, 2);
  rasterFixturePut(bytes, 14, height, 2);
  bytes[16] = std::byte(depth);
  bytes[17] = std::byte(descriptor);
  for (std::size_t i = 18; i < bytes.size(); ++i) bytes[i] = std::byte((i * 37 + 91) & 255);
  const auto appendSample = [&](int pixel) {
    const unsigned value = indexed ? (pixel * 7 + pixel / width * 3) % paletteCount
                                  : pixel * 3157 + pixel / width * 137 + 421;
    for (int c = 0; c < (depth + 7) / 8; ++c) {
      bytes.push_back(std::byte(indexed ? (value >> (c * 8)) & 255
                                       : (value >> (c * 5)) & 255));
    }
  };
  for (int pixel = 0, packet = 0; pixel < width * height; ++packet) {
    if (!rle) {
      appendSample(pixel++);
      continue;
    }
    // Both run and raw packets cross 17-pixel row boundaries.
    const bool repeated = packet % 2 == 0;
    const int count = std::min(repeated ? 19 : 23, width * height - pixel);
    bytes.push_back(std::byte((count - 1) | (repeated ? 128 : 0)));
    if (repeated) appendSample(pixel);
    else for (int i = 0; i < count; ++i) appendSample(pixel + i);
    pixel += count;
  }
  return bytes;
}

std::optional<image_decode::DecodedImageData> rasterFixtureDecode(
    bool bmp, std::span<const std::byte> bytes,
    const image_decode::ImageDecodeOptions &options = {}) {
  return bmp ? image_decode::detail::decodeBmpRows(bytes, options)
             : image_decode::detail::decodeTgaRows(bytes, options);
}

void expectRasterStbEquivalent(const std::vector<std::byte> &bytes, bool bmp,
                               const std::string &label, bool reflectReferenceX = false) {
  int width = 0, height = 0, channels = 0;
  auto *reference = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
      static_cast<int>(bytes.size()), &width, &height, &channels, 4);
  expect(reference != nullptr, (label + ": independent stb fixture decodes").c_str());
  if (!reference) return;
  for (const auto target : {std::array<int, 2>{0, 0}, {5, 5}, {16, 12}}) {
    image_decode::ImageDecodeOptions options{.targetWidth = target[0], .targetHeight = target[1]};
    image_decode::detail::ImageRowReducer expected(width, height, options);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      // stb ignores TGA's right-origin flag. Apply that specified reflection to
      // its independently decoded source before the reference area reduction.
      const auto offset = (y * width + (reflectReferenceX ? width - 1 - x : x)) * 4;
      expected.add(x, y, {reference[offset], reference[offset + 1], reference[offset + 2], reference[offset + 3]});
    }
    const auto reducedReference = expected.finish();
    const auto decoded = rasterFixtureDecode(bmp, bytes, options);
    expect(decoded && reducedReference && decoded->width == reducedReference->width &&
               decoded->height == reducedReference->height && *decoded->rgba == *reducedReference->rgba,
           (label + ": row decode and asymmetric fractional reduction match stb pixels").c_str());
    if (target[0] == 5) expect(decoded && decoded->width == 5 && decoded->height == 3,
                              (label + ": target fit preserves the 17:13 aspect ratio").c_str());
  }
  stbi_image_free(reference);
}

void testRasterBmpVariants() {
  for (int depth : {1, 4, 8, 16, 24, 32}) for (bool topDown : {false, true}) {
    const auto label = "BMP " + std::to_string(depth) + (topDown ? " top-down" : " bottom-up");
    expectRasterStbEquivalent(rasterFixtureBmp(depth, topDown), true, label);
    if (depth == 16 || depth == 32)
      expectRasterStbEquivalent(rasterFixtureBmp(depth, topDown, true), true, label + " bitfields");
    if (depth == 32)
      expectRasterStbEquivalent(rasterFixtureBmp(depth, topDown, false, true), true, label + " zero default alpha");
  }
  auto opaqueMasks = rasterFixtureBmp(32, false, true, true);
  const auto transparent = image_decode::detail::decodeBmpRows(opaqueMasks, {.targetWidth = 5, .targetHeight = 5});
  expect(transparent && (*transparent->rgba)[3] == 0,
         "explicit BMP alpha masks retain all-zero transparency instead of BI_RGB's opacity repair");

  auto bad = rasterFixtureBmp(16, false, true);
  rasterFixturePut(bad, 58, 0xf800, 4);
  expect(!image_decode::detail::decodeBmpRows(bad, {}), "BMP overlapping color masks are rejected");
  bad = rasterFixtureBmp(16, false, true);
  rasterFixturePut(bad, 54, 0x10000, 4);
  expect(!image_decode::detail::decodeBmpRows(bad, {}), "BMP masks outside the sample depth are rejected");
  bad = rasterFixtureBmp(8, false);
  rasterFixturePut(bad, 46, 1, 4);
  expect(!image_decode::detail::decodeBmpRows(bad, {}), "BMP indexes outside the declared palette are rejected");
  bad = rasterFixtureBmp(24, false);
  rasterFixturePut(bad, 10, 53, 4);
  expect(!image_decode::detail::decodeBmpRows(bad, {}), "BMP pixel data cannot overlap its header");
  rasterFixturePut(bad, 10, static_cast<std::uint32_t>(bad.size() + 1), 4);
  expect(!image_decode::detail::decodeBmpRows(bad, {}), "BMP pixel offsets outside input are rejected");
}

void testRasterTgaVariants() {
  for (bool rle : {false, true}) for (unsigned descriptor : {0U, 0x20U, 0x10U, 0x30U}) {
    const auto label = std::string("TGA ") + (rle ? "RLE " : "raw ") + std::to_string(descriptor);
    for (int depth : {8, 15, 16, 24, 32})
      expectRasterStbEquivalent(rasterFixtureTga(depth, rle, descriptor), false,
                               label + " depth " + std::to_string(depth), (descriptor & 0x10) != 0);
    expectRasterStbEquivalent(rasterFixtureTga(16, rle, descriptor, 0, true), false,
                             label + " gray-alpha", (descriptor & 0x10) != 0);
    for (int indexDepth : {8, 16}) for (int paletteDepth : {8, 15, 16, 24, 32})
      expectRasterStbEquivalent(rasterFixtureTga(indexDepth, rle, descriptor, paletteDepth), false,
          label + " palette " + std::to_string(paletteDepth) + " index " + std::to_string(indexDepth),
          (descriptor & 0x10) != 0);
  }

  const auto original = rasterFixtureTga(8, false, 0x20, 24);
  auto shifted = original;
  rasterFixturePut(shifted, 3, 17, 2);
  for (std::size_t i = 18 + 3 + 16 * 3; i < shifted.size(); ++i)
    shifted[i] = std::byte(std::to_integer<unsigned>(shifted[i]) + 17);
  const auto base = image_decode::detail::decodeTgaRows(original, {.targetWidth = 5, .targetHeight = 5});
  const auto moved = image_decode::detail::decodeTgaRows(shifted, {.targetWidth = 5, .targetHeight = 5});
  expect(base && moved && *base->rgba == *moved->rgba,
         "TGA nonzero first palette index changes indexing without skipping palette bytes");
  shifted[18 + 3 + 16 * 3] = std::byte{16};
  expect(!image_decode::detail::decodeTgaRows(shifted, {}), "TGA indexes below the first palette entry are rejected");
  shifted[18 + 3 + 16 * 3] = std::byte{33};
  expect(!image_decode::detail::decodeTgaRows(shifted, {}), "TGA indexes past the palette are rejected");

  auto oversizedPacket = rasterFixtureTga(32, true, 0x20);
  rasterFixturePut(oversizedPacket, 12, 1, 2);
  rasterFixturePut(oversizedPacket, 14, 1, 2);
  expect(!image_decode::detail::decodeTgaRows(oversizedPacket, {}), "TGA RLE packets cannot exceed the declared pixel count");
  auto missingId = rasterFixtureTga(24, false, 0x20);
  missingId.resize(20);
  expect(!image_decode::detail::decodeTgaRows(missingId, {}), "TGA truncated image IDs are rejected before pixel reads");
}

void testRasterAdmissionAndTruncation() {
  for (bool bmp : {false, true}) {
    const auto bytes = bmp ? rasterFixtureBmp(4, false) : rasterFixtureTga(16, true, 0, 32);
    bool allTruncatedRejected = true;
    for (std::size_t size = 0; size < bytes.size(); ++size) {
      if (rasterFixtureDecode(bmp, std::span(bytes).first(size), {.targetWidth = 5, .targetHeight = 5})) {
        allTruncatedRejected = false;
        break;
      }
    }
    expect(allTruncatedRejected, bmp ? "every truncated BMP prefix is rejected" : "every truncated RLE/palette TGA prefix is rejected");
    expect(rasterFixtureDecode(bmp, bytes, {.maximumDimension = 17, .maximumEncodedBytes = bytes.size(),
               .maximumDecodedBytes = 17 * 13 * 4, .targetWidth = 5, .targetHeight = 5}).has_value(),
           "raster source is admitted at exact explicit dimension, encoded and decoded limits");
    expect(!rasterFixtureDecode(bmp, bytes, {.maximumDimension = 16, .targetWidth = 5, .targetHeight = 5}),
           "raster reduction does not bypass explicit source dimensions");
    expect(!rasterFixtureDecode(bmp, bytes, {.maximumEncodedBytes = bytes.size() - 1, .targetWidth = 5, .targetHeight = 5}),
           "raster row decoder honors explicit encoded byte limits");
    expect(!rasterFixtureDecode(bmp, bytes, {.maximumDecodedBytes = 17 * 13 * 4 - 1, .targetWidth = 5, .targetHeight = 5}),
           "raster reduction does not bypass explicit source decoded byte limits");
    std::stop_source stop;
    stop.request_stop();
    expect(!rasterFixtureDecode(bmp, bytes, {.targetWidth = 5, .targetHeight = 5, .stop = stop.get_token()}),
           "cancelled raster decode does not publish output");
  }
  const auto raw = rasterFixtureTga(24, false, 0);
  expect(!image_decode::detail::decodeTgaRows(std::span(raw).first(raw.size() - 1), {}),
         "uncompressed TGA rejects its final truncated pixel");
  const auto bmp = rasterFixtureBmp(16, false, true);
  expect(!image_decode::detail::decodeBmpRows(std::span(bmp).first(65), {}),
         "BMP rejects a truncated external bitfield table");
}

void testRasterLargeRleAllocation() {
  constexpr unsigned width = 4096, height = 4096, target = 32;
  std::vector<std::byte> bytes(18);
  bytes[2] = std::byte{10};
  rasterFixturePut(bytes, 12, width, 2);
  rasterFixturePut(bytes, 14, height, 2);
  bytes[16] = std::byte{32};
  bytes[17] = std::byte{0x18}; // Bottom-right origin, eight alpha bits.
  bytes.reserve(18 + (width * height / 128) * 5);
  for (unsigned pixel = 0; pixel < width * height; pixel += 128)
    for (unsigned byte : {255U, 211U, 93U, 17U, 129U}) bytes.push_back(std::byte(byte));
  std::optional<image_decode::DecodedImageData> decoded;
  std::size_t largest = 0;
  {
    test_support::AllocationSizeObserver allocations;
    decoded = image_decode::detail::decodeTgaRows(bytes, {.targetWidth = target, .targetHeight = target});
    largest = allocations.largest();
  }
  expect(decoded && decoded->width == target && decoded->height == target,
         "large compressed TGA remains accepted at a small requested target");
  expect(largest <= 64U * 1024U,
         "TGA row reduction avoids the 64 MiB source RGBA allocation, including reversed origins");
  bool exact = decoded.has_value();
  if (decoded) for (std::size_t i = 0; i < decoded->rgba->size(); i += 4) {
    exact = exact && (*decoded->rgba)[i] == 17 && (*decoded->rgba)[i + 1] == 93 &&
            (*decoded->rgba)[i + 2] == 211 && (*decoded->rgba)[i + 3] == 129;
  }
  expect(exact, "large RLE TGA reduction preserves every target pixel's color and alpha");
}

void testRasterRowDecoders() {
  testRasterBmpVariants();
  testRasterTgaVariants();
  testRasterAdmissionAndTruncation();
  testRasterLargeRleAllocation();
}

} // namespace
