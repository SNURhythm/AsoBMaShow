#include "view/ImageFileDecoder.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
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
}

int main() {
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
  verifyOptionalCimTree();
  return failures == 0 ? 0 : 1;
}
