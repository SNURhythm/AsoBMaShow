#pragma once

#include "view/PsdRowDecoder.h"

namespace {

void psdTestAppend16(std::vector<std::byte> &bytes, unsigned value) {
  bytes.push_back(static_cast<std::byte>((value >> 8) & 255));
  bytes.push_back(static_cast<std::byte>(value & 255));
}

void psdTestAppend32(std::vector<std::byte> &bytes, std::uint32_t value) {
  psdTestAppend16(bytes, value >> 16);
  psdTestAppend16(bytes, value & 65535);
}

std::vector<std::byte> psdTestPackBits(const std::vector<unsigned char> &row) {
  std::vector<std::byte> result{std::byte{128}}; // Legal no-op before samples.
  std::size_t position = 0;
  while (position < row.size()) {
    std::size_t run = 1;
    while (run < 128 && position + run < row.size() &&
           row[position + run] == row[position]) ++run;
    if (run >= 3) {
      result.push_back(static_cast<std::byte>(257 - run));
      result.push_back(static_cast<std::byte>(row[position]));
      position += run;
    } else {
      const auto start = position++;
      while (position - start < 128 && position < row.size()) {
        if (position + 2 < row.size() && row[position] == row[position + 1] &&
            row[position] == row[position + 2]) break;
        ++position;
      }
      result.push_back(static_cast<std::byte>(position - start - 1));
      for (auto i = start; i < position; ++i) {
        result.push_back(static_cast<std::byte>(row[i]));
      }
    }
  }
  // Only leading no-ops: stb ignores row byte counts, so a trailing no-op
  // after the last row would be consumed as the next channel's first token.
  return result;
}

std::vector<std::byte> psdTestImage(
    int width, int height, int depth, int channels, bool compressed,
    const std::vector<std::array<std::uint16_t, 4>> &samples) {
  std::vector<std::byte> encoded{std::byte{'8'}, std::byte{'B'},
                                 std::byte{'P'}, std::byte{'S'}};
  psdTestAppend16(encoded, 1);
  encoded.insert(encoded.end(), 6, std::byte{0});
  psdTestAppend16(encoded, channels);
  psdTestAppend32(encoded, height);
  psdTestAppend32(encoded, width);
  psdTestAppend16(encoded, depth);
  psdTestAppend16(encoded, 3);
  for (int section = 0; section < 3; ++section) psdTestAppend32(encoded, 0);
  psdTestAppend16(encoded, compressed ? 1 : 0);

  std::vector<std::vector<std::byte>> rows;
  for (int channel = 0; channel < channels; ++channel) {
    for (int y = 0; y < height; ++y) {
      std::vector<unsigned char> row;
      for (int x = 0; x < width; ++x) {
        const auto value = channel < 4 ? samples[y * width + x][channel] : 0x9b61;
        row.push_back(static_cast<unsigned char>(depth == 16 ? value >> 8 : value));
        if (depth == 16) row.push_back(static_cast<unsigned char>(value));
      }
      if (compressed) {
        rows.push_back(psdTestPackBits(row));
        psdTestAppend16(encoded, static_cast<unsigned>(rows.back().size()));
      } else {
        for (const auto value : row) encoded.push_back(static_cast<std::byte>(value));
      }
    }
  }
  if (compressed) {
    for (const auto &row : rows) encoded.insert(encoded.end(), row.begin(), row.end());
  }
  return encoded;
}

void psdTestAgainstStb(const std::vector<std::byte> &encoded,
                       const image_decode::ImageDecodeOptions &options,
                       const char *message) {
  int width = 0, height = 0, channels = 0;
  const auto pixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>(
      stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()),
                            static_cast<int>(encoded.size()), &width, &height,
                            &channels, 4), stbi_image_free);
  expect(pixels != nullptr, "PSD fixture has an independent stb decode");
  if (!pixels) return;
  image_decode::detail::ImageRowReducer reference(width, height, options);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto offset = static_cast<std::size_t>(y * width + x) * 4;
      reference.add(x, y, {pixels.get()[offset], pixels.get()[offset + 1],
                            pixels.get()[offset + 2], pixels.get()[offset + 3]});
    }
  }
  const auto expected = reference.finish();
  const auto actual = image_decode::detail::decodePsdRows(encoded, options);
  expect(actual && expected && actual->width == expected->width &&
             actual->height == expected->height && *actual->rgba == *expected->rgba,
         message);
}

void testPsdRowDecoders() {
  using image_decode::detail::decodePsdRows;
  std::vector<std::array<std::uint16_t, 4>> samples;
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 5; ++x) {
      const unsigned alpha = x == 0 ? 0 : x == 4 ? 255 : 64 + 32 * y + 17 * x;
      samples.push_back({static_cast<std::uint16_t>(250),
                         static_cast<std::uint16_t>(255 - alpha + (13 * x + 7 * y) % (alpha + 1)),
                         static_cast<std::uint16_t>(255 - alpha + (23 * x + 11 * y) % (alpha + 1)),
                         static_cast<std::uint16_t>(alpha)});
    }
  }
  const image_decode::ImageDecodeOptions full{};
  const image_decode::ImageDecodeOptions reduced{.targetWidth = 3, .targetHeight = 2};
  const auto raw8 = psdTestImage(5, 3, 8, 4, false, samples);
  const auto rle8 = psdTestImage(5, 3, 8, 4, true, samples);
  auto samples16 = samples;
  for (auto &pixel : samples16) {
    for (int channel = 0; channel < 4; ++channel) {
      pixel[channel] = static_cast<std::uint16_t>((pixel[channel] << 8) | (17 + 31 * channel));
    }
  }
  const auto raw16 = psdTestImage(5, 3, 16, 4, false, samples16);
  psdTestAgainstStb(raw8, full, "raw PSD preserves asymmetric channels and white-matte alpha");
  psdTestAgainstStb(raw8, reduced, "raw PSD reduces fractional areas after white-matte removal");
  psdTestAgainstStb(raw16, full, "raw 16-bit PSD selects high bytes before alpha correction");
  psdTestAgainstStb(raw16, reduced, "raw 16-bit PSD row reduction matches full stb decoding");
  psdTestAgainstStb(rle8, full, "PackBits PSD handles literal, repeat, and no-op packets");
  psdTestAgainstStb(rle8, reduced, "PackBits PSD preserves channel row order during reduction");
  for (const int channels : {1, 3, 5}) {
    for (const bool compressed : {false, true}) {
      const auto encoded = psdTestImage(5, 3, 8, channels, compressed, samples);
      psdTestAgainstStb(encoded, reduced, "PSD channel defaults and extra channels match stb");
    }
  }

  // stb treats compressed 16-bit samples as 8-bit. Check the standard PSD
  // interpretation independently instead of enshrining that existing bug.
  const std::vector<std::array<std::uint16_t, 4>> precise16{
      {0x1234, 0x56ab, 0x9cef, 0xffff}, {0x2102, 0x6533, 0xab44, 0xffff},
      {0x3405, 0x8766, 0xcd77, 0xffff}, {0x4508, 0xa988, 0xef99, 0xffff}};
  const auto rle16 = psdTestImage(2, 2, 16, 3, true, precise16);
  const auto decoded16 = decodePsdRows(rle16, full);
  const std::vector<unsigned char> expected16{
      0x12, 0x56, 0x9c, 255, 0x21, 0x65, 0xab, 255,
      0x34, 0x87, 0xcd, 255, 0x45, 0xa9, 0xef, 255};
  expect(decoded16 && decoded16->width == 2 && decoded16->height == 2 &&
             *decoded16->rgba == expected16,
         "16-bit PackBits PSD uses high bytes from every channel row");
  const auto tiny16 = decodePsdRows(rle16, {.targetWidth = 1, .targetHeight = 1});
  expect(tiny16 && *tiny16->rgba == std::vector<unsigned char>{43, 123, 193, 255},
         "16-bit PackBits PSD averages decoded high-byte samples");

  bool truncationsRejected = true;
  for (const auto *encoded : {&raw8, &raw16, &rle8, &rle16}) {
    for (std::size_t size = 0; size < encoded->size(); ++size) {
      if (decodePsdRows(std::span<const std::byte>(*encoded).first(size), full)) {
        truncationsRejected = false;
      }
    }
  }
  expect(truncationsRejected, "PSD rejects every truncated header, table, channel, and row prefix");
  auto wrongRowLength = rle8;
  wrongRowLength[40] = std::byte{0};
  wrongRowLength[41] = std::byte{1};
  expect(!decodePsdRows(wrongRowLength, full), "PSD PackBits cannot borrow samples from another row");
  auto overflowingRun = rle8;
  overflowingRun[40 + 4 * 3 * 2 + 1] = std::byte{127};
  expect(!decodePsdRows(overflowingRun, full), "PSD rejects a PackBits run larger than its decoded row");
  for (const bool compressed : {false, true}) {
    auto missingExtraChannel = psdTestImage(5, 3, 8, 5, compressed, samples);
    missingExtraChannel.pop_back();
    expect(!decodePsdRows(missingExtraChannel, full),
           "PSD rejects truncated declared extra-channel payload");
  }
  auto overflowingSection = raw8;
  std::fill(overflowingSection.begin() + 26, overflowingSection.begin() + 30, std::byte{255});
  expect(!decodePsdRows(overflowingSection, full), "PSD checks metadata section lengths before skipping");
  expect(!decodePsdRows(raw8, {.maximumDimension = 4, .targetWidth = 1, .targetHeight = 1}),
         "PSD source dimension admission remains effective while reducing");
  expect(!decodePsdRows(raw8, {.maximumDecodedBytes = 59, .targetWidth = 1, .targetHeight = 1}),
         "PSD source byte admission remains effective while reducing");
  expect(!decodePsdRows(raw8, {.maximumEncodedBytes = raw8.size() - 1}),
         "PSD honors the encoded byte admission bound");
  std::stop_source cancelled;
  cancelled.request_stop();
  expect(!decodePsdRows(raw8, {.stop = cancelled.get_token()}) &&
             !decodePsdRows(rle8, {.stop = cancelled.get_token()}),
         "PSD raw and PackBits decoding honor cancellation");
}

} // namespace
