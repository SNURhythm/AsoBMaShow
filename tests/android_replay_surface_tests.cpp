// Device integration test: actual MediaCodec output catches channel swaps,
// upside-down frames, timestamp drift, lost frames, and encoder drain failures.
#include "replay/AndroidReplaySurface.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

int main(int argc, char **argv) {
  assert(argc >= 5);
  const int width = std::atoi(argv[2]);
  const int height = std::atoi(argv[3]);
  const int fps = std::atoi(argv[4]);
  assert(width >= 64 && height >= 64 && fps > 0);
  const int frames = fps * 2;
  const auto *codec = avcodec_find_encoder_by_name("h264_mediacodec");
  assert(codec);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  assert(context);
  context->width = width;
  context->height = height;
  context->pix_fmt = AV_PIX_FMT_MEDIACODEC;
  context->time_base = {1, fps};
  context->framerate = {fps, 1};
  context->bit_rate = 24000000;
  context->gop_size = fps;
  context->max_b_frames = 0;
  context->color_range = AVCOL_RANGE_MPEG;
  context->colorspace = AVCOL_SPC_SMPTE170M;
  context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  replay_video_export::AndroidReplaySurface surface;
  std::string error;
  assert(!surface.submit(nullptr, 0, error));
  assert(surface.prepare(context, error));
  AVDictionary *options = nullptr;
  av_dict_set(&options, "ndk_codec", "1", 0);
  if (argc > 5) av_dict_set(&options, "codec_name", argv[5], 0);
  av_dict_set(&options, "bitrate_mode", "vbr", 0);
  assert(avcodec_open2(context, codec, &options) == 0);
  av_dict_free(&options);
  if (!surface.initialize(context, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  assert(!surface.submit(nullptr, 0, error));
  AVFormatContext *muxer = nullptr;
  assert(avformat_alloc_output_context2(&muxer, nullptr, "mp4", argv[1]) == 0);
  auto *stream = avformat_new_stream(muxer, nullptr);
  assert(stream);
  stream->time_base = context->time_base;
  assert(avcodec_parameters_from_context(stream->codecpar, context) == 0);
  assert(avio_open(&muxer->pb, argv[1], AVIO_FLAG_WRITE) == 0);
  assert(avformat_write_header(muxer, nullptr) == 0);
  AVFrame *frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_MEDIACODEC;
  frame->width = width;
  frame->height = height;
  // FFmpeg requires a refcounted marker even though EGL supplies the pixels.
  frame->buf[0] = av_buffer_alloc(1);
  frame->data[0] = frame->buf[0]->data;
  AVPacket *packet = av_packet_alloc();
  int count = 0;
  auto drain = [&]() {
    int result;
    while ((result = avcodec_receive_packet(context, packet)) >= 0) {
      assert(packet->pts == count);
      packet->duration = 1;
      av_packet_rescale_ts(packet, context->time_base, stream->time_base);
      packet->stream_index = stream->index;
      assert(av_interleaved_write_frame(muxer, packet) == 0);
      av_packet_unref(packet);
      ++count;
    }
    assert(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
  };
  std::vector<uint8_t> pixels(width * height * 4);
  // BGRA quadrants: red, green, blue, white. Top-left marker alternates black
  // and white every frame so a decoded sequence can expose dropped frames.
  const std::array<std::array<uint8_t, 4>, 4> colors{{
      {0, 0, 255, 255}, {0, 255, 0, 255},
      {255, 0, 0, 255}, {255, 255, 255, 255}}};
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      for (int channel = 0; channel < 4; ++channel)
        pixels[(y * width + x) * 4 + channel] =
            colors[(y >= height / 2) * 2 + (x >= width / 2)][channel];
  // A one-pixel grayscale stripe detects texture-coordinate precision loss
  // above 2048px without relying on chroma subsampling accuracy.
  for (int y = height / 2 - 8; y < height / 2 + 8; ++y)
    for (int x = 0; x < width; ++x)
      for (int c = 0; c < 3; ++c)
        pixels[(y * width + x) * 4 + c] = x % 2 ? 255 : 0;
  const auto start = std::chrono::steady_clock::now();
  // Match the app: create on the export thread and submit on its worker.
  std::thread worker([&]() {
    for (int i = 0; i < frames; ++i) {
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
          for (int c = 0; c < 3; ++c)
            pixels[(y * width + x) * 4 + c] = i % 2 ? 255 : 0;
      assert(surface.submit(pixels.data(), int64_t(i) * 1000000000 / fps, error));
      frame->pts = i;
      assert(avcodec_send_frame(context, frame) == 0);
      drain();
    }
  });
  worker.join();
  assert(avcodec_send_frame(context, nullptr) == 0);
  drain();
  assert(count == frames);
  assert(av_write_trailer(muxer) == 0);
  std::printf("Encoded %d %dx%d frames in %.3fs\n", count, width, height,
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
  surface.reset();
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&context);
  avio_closep(&muxer->pb);
  avformat_free_context(muxer);
}
