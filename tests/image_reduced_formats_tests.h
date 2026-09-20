#pragma once

namespace {
std::vector<std::byte> solidJpeg(int width, int height, bool progressive = false) {
  std::vector<std::byte> bytes;
  const auto put = [&](int value) { bytes.push_back(std::byte(value & 255)); };
  const auto word = [&](int value) { put(value >> 8); put(value); };
  const auto marker = [&](int value) { put(255); put(value); };
  marker(0xd8);
  marker(0xdb); word(67); put(0);
  for (int i = 0; i < 64; ++i) put(1);
  marker(progressive ? 0xc2 : 0xc0); word(11); put(8);
  word(height); word(width); put(1); put(1); put(0x11); put(0);
  for (int table : {0, 0x10}) {
    marker(0xc4); word(20); put(table); put(1);
    for (int i = 1; i < 16; ++i) put(0);
    put(0); // DC category zero, or AC end-of-block, encoded with one zero bit.
  }
  const auto scan = [&](int first, int last, int bitsPerBlock) {
    marker(0xda); word(8); put(1); put(1); put(0);
    put(first); put(last); put(0);
    const auto bits = static_cast<std::size_t>((width + 7) / 8) *
                      ((height + 7) / 8) * bitsPerBlock;
    bytes.insert(bytes.end(), bits / 8, std::byte{0});
    if (bits % 8 != 0) put((1 << (8 - bits % 8)) - 1);
  };
  if (progressive) { scan(0, 0, 1); scan(1, 63, 1); }
  else scan(0, 63, 2);
  marker(0xd9);
  return bytes;
}

void testJpegDecodeTimeScaling(bool stress = false) {
  for (const auto dimensions : {std::pair{stress ? 16384 : 4096, stress ? 16384 : 2048},
                                 std::pair{4099, 2051}, std::pair{65, 1}}) {
    const auto jpeg = solidJpeg(dimensions.first, dimensions.second);
    const image_decode::ImageDecodeOptions options{
        .targetWidth = stress ? 2048 : 128, .targetHeight = stress ? 2048 : 128};
    const auto [width, height] = image_decode::detail::reducedDimensions(
        dimensions.first, dimensions.second, options);
    const auto image = image_decode::decodeImageMemory(jpeg, options);
    expect(image && image->width == width && image->height == height,
           "large and odd-sized JPEGs decode directly to exact target dimensions");
    if (image) {
      for (std::size_t i = 0; i < image->rgba->size(); i += 4) {
        if (std::abs(int((*image->rgba)[i]) - 128) > 2 ||
            (*image->rgba)[i + 3] != 255) {
          expect(false, "scaled JPEG preserves solid gray and opaque alpha");
          break;
        }
      }
    }
  }
  const auto progressive = solidJpeg(259, 131, true);
  const auto image = image_decode::decodeImageMemory(
      progressive, {.targetWidth = 31, .targetHeight = 31});
  expect(image && image->width == 31 && image->height == 15,
         "progressive JPEG remains supported with exact downsample dimensions");
  const auto jpeg = solidJpeg(65, 33);
  expect(!image_decode::decodeImageMemory(jpeg, {.maximumDimension = 64,
                .targetWidth = 8, .targetHeight = 8}),
         "JPEG scaling preserves caller source dimension limits");
  expect(!image_decode::decodeImageMemory(jpeg, {.maximumDecodedBytes = 100,
                .targetWidth = 8, .targetHeight = 8}),
         "JPEG scaling preserves caller source byte limits");
  std::stop_source cancellation;
  cancellation.request_stop();
  expect(!image_decode::decodeImageMemory(jpeg, {.targetWidth = 8,
                .targetHeight = 8, .stop = cancellation.get_token()}),
         "JPEG scaling honors cancellation");
  expect(!image_decode::decodeImageMemory(std::span(jpeg).first(20),
                {.targetWidth = 8, .targetHeight = 8}),
         "truncated JPEG header is rejected");
}
std::vector<std::byte> smallGif(bool interlaced) {
  std::vector<std::byte> bytes;
  const auto put = [&](int n) { bytes.push_back(std::byte(n & 255)); };
  const auto word = [&](int n) { put(n); put(n >> 8); };
  for (char c : std::string("GIF89a")) put(c);
  word(17); word(13); put(0x81); put(1); put(0);
  for (int n : {255, 0, 0, 10, 20, 30, 0, 255, 0, 0, 0, 255}) put(n);
  for (int n : {0x21, 0xf9, 4, 1, 0, 0, 2, 0}) put(n);
  put(0x2c); word(3); word(2); word(9); word(7); put(interlaced ? 64 : 0);
  put(2);
  std::vector<unsigned char> raster;
  unsigned bits = 0; int count = 0;
  const auto code = [&](unsigned n) {
    bits |= n << count; count += 3;
    while (count >= 8) { raster.push_back(bits & 255); bits >>= 8; count -= 8; }
  };
  for (int i = 0; i < 63; ++i) { code(4); code(i % 4); }
  code(5);
  if (count) raster.push_back(bits & 255);
  put(static_cast<int>(raster.size()));
  for (auto n : raster) put(n);
  put(0); put(0x3b);
  return bytes;
}

std::vector<std::byte> smallPic(int compression) {
  std::vector<std::byte> bytes(104, std::byte{0});
  for (auto [offset, value] : {std::pair{0, 0x53}, {1, 0x80}, {2, 0xf6}, {3, 0x34},
                              {88, 'P'}, {89, 'I'}, {90, 'C'}, {91, 'T'}, {93, 9}, {95, 7}})
    bytes[offset] = std::byte(value);
  const auto put = [&](int n) { bytes.push_back(std::byte(n & 255)); };
  put(0); put(8); put(compression); put(0xf0);
  for (int y = 0; y < 7; ++y) {
    if (compression == 1) put(9);
    if (compression == 2) put(136); // repeat nine pixels
    for (int x = 0; x < (compression == 0 ? 9 : 1); ++x) {
      put(10 + y * 30); put(80); put(190 - y * 20); put(20 + y * 35);
    }
  }
  return bytes;
}

void checkRowDecodeAgainstStb(const std::vector<std::byte> &bytes,
                              const char *message) {
  int width = 0, height = 0, channels = 0;
  auto *pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(bytes.data()),
      static_cast<int>(bytes.size()), &width, &height, &channels, 4);
  expect(pixels != nullptr, "independent stb reference accepts generated image fixture");
  if (!pixels) return;
  for (const auto target : {std::pair{5, 3}, std::pair{11, 8}}) {
    const image_decode::ImageDecodeOptions options{.targetWidth = target.first, .targetHeight = target.second};
    image_decode::detail::ImageRowReducer reference(width, height, options);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const auto *p = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
      reference.add(x, y, {p[0], p[1], p[2], p[3]});
    }
    const auto expected = reference.finish();
    const auto actual = image_decode::decodeImageMemory(bytes, options);
    expect(actual && expected && actual->width == expected->width && actual->height == expected->height &&
               *actual->rgba == *expected->rgba, message);
  }
  stbi_image_free(pixels);
}

void testGifAndPicRowDecoders() {
  for (const bool interlaced : {false, true}) {
    const auto gif = smallGif(interlaced);
    checkRowDecodeAgainstStb(gif, "GIF interlace, partial frame, transparency and background match stb");
    expect(!image_decode::decodeImageMemory(std::span(gif).first(gif.size() - 10),
                {.targetWidth = 5, .targetHeight = 3}), "truncated GIF raster is rejected");
  }
  const auto lzw = readBytes(std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/resources/streamed-lzw.gif");
  checkRowDecodeAgainstStb(lzw, "GIF growing LZW dictionary matches independent stb reference");
  for (int compression : {0, 1, 2}) {
    const auto pic = smallPic(compression);
    checkRowDecodeAgainstStb(pic, "PIC raw and RLE rows match independent stb reference");
    expect(!image_decode::decodeImageMemory(std::span(pic).first(pic.size() - 1),
                {.targetWidth = 5, .targetHeight = 3}), "truncated PIC row is rejected");
  }
}

}
