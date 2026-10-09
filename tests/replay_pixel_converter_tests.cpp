#include "replay/ReplayPixelConverter.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
}

namespace {
struct Frame {
  AVFrame *value = av_frame_alloc();
  Frame(int width, int height, AVPixelFormat format) {
    assert(value);
    value->format = format;
    value->width = width;
    value->height = height;
    assert(av_frame_get_buffer(value, 32) == 0);
  }
  ~Frame() { av_frame_free(&value); }
};

void testVideoRangeColors(AVPixelFormat format) {
  // Solid BGRA colors and independently specified BT.601 limited-range YUV.
  const std::array<std::array<int, 7>, 5> colors{{
      {0, 0, 0, 255, 16, 128, 128},
      {255, 255, 255, 255, 235, 128, 128},
      {0, 0, 255, 255, 81, 90, 240},
      {0, 255, 0, 255, 145, 54, 34},
      {255, 0, 0, 255, 41, 240, 110},
  }};
  const bool interleaved = format == AV_PIX_FMT_NV12;
  Frame output(34, 18, format);
  replay_video_export::ReplayPixelConverter converter;
  assert(converter.convert(nullptr, output.value) < 0);
  assert(converter.initialize(output.value, 4) == 0);
  for (const auto &color : colors) {
    std::vector<uint8_t> pixels(34 * 18 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = color[i % 4];
    const auto original = pixels;
    auto *destination = output.value->data[0];
    assert(converter.convert(pixels.data(), output.value) >= 0);
    assert(pixels == original);
    assert(output.value->data[0] == destination);
    assert(av_buffer_get_ref_count(output.value->buf[0]) == 1);
    for (int plane = 0; plane < (interleaved ? 2 : 3); ++plane) {
      for (int y = 0; y < (plane ? 9 : 18); ++y) {
        for (int x = 0; x < (plane && !interleaved ? 17 : 34); ++x) {
          const int actual = output.value->data[plane][y * output.value->linesize[plane] + x];
          const int channel = plane && interleaved ? 1 + x % 2 : plane;
          assert(std::abs(actual - color[4 + channel]) <= 1);
        }
      }
    }
  }
  assert(converter.convert(nullptr, output.value) < 0);
}

void compareWithLegacy(AVPixelFormat format, bool benchmark) {
  const bool interleaved = format == AV_PIX_FMT_NV12;
  const int width = benchmark ? 2400 : 130;
  const int height = benchmark ? 1080 : 70;
  const int iterations = benchmark ? 120 : 2;
  std::vector<uint8_t> pixels(width * height * 4);
  uint32_t random = 13;
  for (auto &pixel : pixels) {
    random = random * 1664525 + 1013904223;
    pixel = random >> 24;
  }
  Frame reference(width, height, format);
  Frame output(width, height, format);
  SwsContext *legacy = sws_alloc_context();
  assert(legacy);
  legacy->flags = SWS_FAST_BILINEAR;
  legacy->threads = 7;
  legacy->src_w = legacy->dst_w = width;
  legacy->src_h = legacy->dst_h = height;
  legacy->src_format = AV_PIX_FMT_BGRA;
  legacy->dst_format = format;
  assert(sws_init_context(legacy, nullptr, nullptr) == 0);
  const uint8_t *source[] = {pixels.data(), nullptr, nullptr, nullptr};
  const int strides[] = {width * 4, 0, 0, 0};
  replay_video_export::ReplayPixelConverter converter;
  assert(converter.initialize(output.value, 7) == 0);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i)
    assert(sws_scale(legacy, source, strides, 0, height,
                     reference.value->data, reference.value->linesize) == height);
  const auto converted = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i)
    assert(converter.convert(pixels.data(), output.value) >= 0);
  const auto finished = std::chrono::steady_clock::now();
  for (int plane = 0; plane < (interleaved ? 2 : 3); ++plane) {
    for (int y = 0; y < (plane ? height / 2 : height); ++y) {
      for (int x = 0; x < (plane && !interleaved ? width / 2 : width); ++x) {
        const int actual = output.value->data[plane][y * output.value->linesize[plane] + x];
        const int expected = reference.value->data[plane][y * reference.value->linesize[plane] + x];
        assert(std::abs(actual - expected) <= 1);
      }
    }
  }
  if (benchmark) {
    const double oldMs = std::chrono::duration<double, std::milli>(converted - start).count() / iterations;
    const double newMs = std::chrono::duration<double, std::milli>(finished - converted).count() / iterations;
    std::printf("%dx%d BGRA->%s: legacy %.3f ms/frame, threaded %.3f ms/frame, %.2fx speedup\n",
                width, height, interleaved ? "NV12" : "YUV420P", oldMs, newMs, oldMs / newMs);
  }
  sws_freeContext(legacy);
}
}

int main(int argc, char **argv) {
  for (const auto format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12}) {
    testVideoRangeColors(format);
    compareWithLegacy(format, argc > 1 && std::strcmp(argv[1], "--benchmark") == 0);
  }
}
