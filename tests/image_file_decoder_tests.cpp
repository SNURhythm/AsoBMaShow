#include "view/ImageFileDecoder.h"

#include <array>
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
  return failures == 0 ? 0 : 1;
}
