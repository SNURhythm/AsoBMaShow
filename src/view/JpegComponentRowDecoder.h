#pragma once

#include "ImageRowReducer.h"

// Included after the bundled stb implementation. Retain its JPEG compatibility
// beyond FFmpeg's coded-dimension bounds, but convert/reduce one row at a time.
// Component planes (and progressive coefficients) remain decoder-owned; there
// is never an additional source-sized RGB(A) image on this path.
namespace image_decode::detail {
inline std::optional<DecodedImageData>
decodeJpegComponentRows(std::span<const std::byte> encoded,
                         const ImageDecodeOptions &options) {
  stbi__context input{};
  stbi__start_mem(&input, reinterpret_cast<const stbi_uc *>(encoded.data()),
                  static_cast<int>(encoded.size()));
  stbi__jpeg jpeg{};
  jpeg.s = &input;
  stbi__setup_jpeg(&jpeg);
  struct Cleanup {
    stbi__jpeg *jpeg;
    ~Cleanup() { stbi__cleanup_jpeg(jpeg); }
  } cleanup{&jpeg};
  if (options.stop.stop_requested() || !stbi__decode_jpeg_image(&jpeg) ||
      options.stop.stop_requested()) return std::nullopt;
  const int width = static_cast<int>(input.img_x), height = static_cast<int>(input.img_y);
  if (width <= 0 || height <= 0 || width > options.maximumDimension ||
      height > options.maximumDimension ||
      static_cast<std::uint64_t>(width) * height > options.maximumDecodedBytes / 4)
    return std::nullopt;
  std::array<stbi__resample, 4> resamplers{};
  std::array<stbi_uc *, 4> planes{};
  for (int k = 0; k < input.img_n; ++k) {
    auto &component = jpeg.img_comp[k];
    component.linebuf = static_cast<stbi_uc *>(stbi__malloc(width + 3));
    if (!component.linebuf) return std::nullopt;
    auto &r = resamplers[k];
    r.hs = jpeg.img_h_max / component.h;
    r.vs = jpeg.img_v_max / component.v;
    r.ystep = r.vs >> 1;
    r.w_lores = (width + r.hs - 1) / r.hs;
    r.line0 = r.line1 = component.data;
    if (r.hs == 1 && r.vs == 1) r.resample = resample_row_1;
    else if (r.hs == 1 && r.vs == 2) r.resample = stbi__resample_row_v_2;
    else if (r.hs == 2 && r.vs == 1) r.resample = stbi__resample_row_h_2;
    else if (r.hs == 2 && r.vs == 2) r.resample = jpeg.resample_row_hv_2_kernel;
    else r.resample = stbi__resample_row_generic;
  }
  ImageRowReducer reducer(width, height, options);
  std::vector<stbi_uc> row(static_cast<std::size_t>(width) * 4);
  const bool rgb = input.img_n == 3 &&
      (jpeg.rgb == 3 || (jpeg.app14_color_transform == 0 && !jpeg.jfif));
  for (int y = 0; y < height; ++y) {
    if (options.stop.stop_requested()) return std::nullopt;
    for (int k = 0; k < input.img_n; ++k) {
      auto &r = resamplers[k];
      const bool bottom = r.ystep >= (r.vs >> 1);
      planes[k] = r.resample(jpeg.img_comp[k].linebuf,
          bottom ? r.line1 : r.line0, bottom ? r.line0 : r.line1, r.w_lores, r.hs);
      if (++r.ystep >= r.vs) {
        r.ystep = 0; r.line0 = r.line1;
        if (++r.ypos < jpeg.img_comp[k].y) r.line1 += jpeg.img_comp[k].w2;
      }
    }
    if (input.img_n >= 3 && !rgb &&
        !(input.img_n == 4 && jpeg.app14_color_transform == 0)) {
      jpeg.YCbCr_to_RGB_kernel(row.data(), planes[0], planes[1], planes[2], width, 4);
    }
    for (int x = 0; x < width; ++x) {
      auto *pixel = row.data() + x * 4;
      if (input.img_n == 1) pixel[0] = pixel[1] = pixel[2] = planes[0][x];
      else if (rgb) for (int c = 0; c < 3; ++c) pixel[c] = planes[c][x];
      else if (input.img_n == 4 && jpeg.app14_color_transform == 0)
        for (int c = 0; c < 3; ++c) pixel[c] = stbi__blinn_8x8(planes[c][x], planes[3][x]);
      else if (input.img_n == 4 && jpeg.app14_color_transform == 2)
        for (int c = 0; c < 3; ++c) pixel[c] = stbi__blinn_8x8(255 - pixel[c], planes[3][x]);
      reducer.add(x, y, {pixel[0], pixel[1], pixel[2], 255});
    }
  }
  return reducer.finish();
}
}
