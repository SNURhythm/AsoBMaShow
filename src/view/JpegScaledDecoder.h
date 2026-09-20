#pragma once

#include "ImageFileDecoder.h"
#include "ImageRowReducer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>

namespace image_decode::detail {

inline bool isJpeg(std::span<const std::byte> encoded) {
  return encoded.size() >= 3 && encoded[0] == std::byte{0xff} &&
         encoded[1] == std::byte{0xd8} && encoded[2] == std::byte{0xff};
}

// MJPEG performs the inverse DCT at reduced resolution before allocating its
// native pixel planes. Progressive JPEG still requires source-sized coefficient
// arrays; neither path materializes source-sized RGBA before reduction.
inline std::optional<DecodedImageData>
decodeJpegScaled(std::span<const std::byte> encoded,
                  const ImageDecodeOptions &options,
                  int sourceWidth, int sourceHeight) {
  const auto stopped = [&] { return options.stop.stop_requested(); };
  const auto validDimensions = [&](int width, int height) {
    if (width <= 0 || height <= 0 || options.maximumDimension <= 0 ||
        width > options.maximumDimension || height > options.maximumDimension) {
      return false;
    }
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    return pixels <= std::numeric_limits<std::size_t>::max() / 4 &&
           pixels <= options.maximumDecodedBytes / 4;
  };
  if (stopped() || !isJpeg(encoded) || encoded.size() > INT_MAX ||
      encoded.size() > options.maximumEncodedBytes ||
      !validDimensions(sourceWidth, sourceHeight)) return std::nullopt;

  const auto [outputWidth, outputHeight] =
      reducedDimensions(sourceWidth, sourceHeight, options);
  if (!validDimensions(outputWidth, outputHeight) || outputWidth > INT_MAX / 4) {
    return std::nullopt;
  }
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  if (!codec) return std::nullopt;
  const auto context = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)>(
      avcodec_alloc_context3(codec),
      [](AVCodecContext *value) { avcodec_free_context(&value); });
  if (!context) return std::nullopt;
  context->thread_count = 1;
  // The software MJPEG decoder supports up to 1/8 in each axis. Keep enough
  // samples for the exact target derived from the original aspect ratio.
  const int maximumLowres = std::min<int>(codec->max_lowres, 3);
  while (context->lowres < maximumLowres) {
    const int divisor = 1 << (context->lowres + 1);
    const auto width = (static_cast<std::int64_t>(sourceWidth) + divisor - 1) / divisor;
    const auto height = (static_cast<std::int64_t>(sourceHeight) + divisor - 1) / divisor;
    if (width < outputWidth || height < outputHeight) break;
    ++context->lowres;
  }
  if (stopped() || avcodec_open2(context.get(), codec, nullptr) < 0 || stopped()) {
    return std::nullopt;
  }
  const auto packet = std::unique_ptr<AVPacket, void (*)(AVPacket *)>(
      av_packet_alloc(), [](AVPacket *value) { av_packet_free(&value); });
  const auto frame = std::unique_ptr<AVFrame, void (*)(AVFrame *)>(
      av_frame_alloc(), [](AVFrame *value) { av_frame_free(&value); });
  if (!packet || !frame ||
      av_new_packet(packet.get(), static_cast<int>(encoded.size())) < 0) {
    return std::nullopt;
  }
  // av_new_packet supplies the zero-filled AV_INPUT_BUFFER_PADDING_SIZE tail
  // required by the decoder; the caller's span need not have any padding.
  for (std::size_t offset = 0; offset < encoded.size();) {
    if (stopped()) return std::nullopt;
    const auto count = std::min<std::size_t>(64 * 1024, encoded.size() - offset);
    std::memcpy(packet->data + offset, encoded.data() + offset, count);
    offset += count;
  }
  if (stopped() || avcodec_send_packet(context.get(), packet.get()) < 0 || stopped()) {
    return std::nullopt;
  }
  int received = avcodec_receive_frame(context.get(), frame.get());
  if (received == AVERROR(EAGAIN)) {
    if (stopped() || avcodec_send_packet(context.get(), nullptr) < 0 || stopped()) {
      return std::nullopt;
    }
    received = avcodec_receive_frame(context.get(), frame.get());
  }
  if (received < 0 || stopped() ||
      !validDimensions(frame->width, frame->height)) return std::nullopt;
  const int divisor = 1 << context->lowres;
  const int expectedWidth = static_cast<int>(
      (static_cast<std::int64_t>(sourceWidth) + divisor - 1) / divisor);
  const int expectedHeight = static_cast<int>(
      (static_cast<std::int64_t>(sourceHeight) + divisor - 1) / divisor);
  if (frame->width != expectedWidth || frame->height != expectedHeight) {
    return std::nullopt;
  }

  const auto scaler = std::unique_ptr<SwsContext, void (*)(SwsContext *)>(
      sws_getContext(frame->width, frame->height,
                     static_cast<AVPixelFormat>(frame->format),
                     outputWidth, outputHeight, AV_PIX_FMT_RGBA,
                     SWS_AREA, nullptr, nullptr, nullptr),
      sws_freeContext);
  if (!scaler || stopped()) return std::nullopt;
  auto rgba = std::make_shared<std::vector<unsigned char>>(
      static_cast<std::size_t>(outputWidth) * outputHeight * 4);
  std::array<std::uint8_t *, 4> output{rgba->data(), nullptr, nullptr, nullptr};
  const std::array<int, 4> strides{outputWidth * 4, 0, 0, 0};
  if (stopped() ||
      sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height,
                 output.data(), strides.data()) != outputHeight || stopped()) {
    return std::nullopt;
  }
  return DecodedImageData{.width = outputWidth, .height = outputHeight,
                          .rgba = std::move(rgba)};
}

} // namespace image_decode::detail
