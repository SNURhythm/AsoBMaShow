#include "StorageVideoCapabilities.h"
#include "StorageVideoEncoder.h"
#include "../video/HardwareVideoDecoder.h"
#include "../video/HardwareVideoFrame.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace storage_management {
namespace {
void require(int status, const char *operation) {
  if (status >= 0) return;
  char error[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(status, error, sizeof(error));
  throw std::runtime_error(std::string(operation) + ": " + error);
}
struct Codec {
  AVCodecContext *value = nullptr;
  ~Codec() { avcodec_free_context(&value); }
};
struct Frame {
  AVFrame *value = av_frame_alloc();
  Frame() { if (!value) throw std::bad_alloc(); }
  ~Frame() { av_frame_free(&value); }
};
struct Packet {
  AVPacket *value = av_packet_alloc();
  Packet() { if (!value) throw std::bad_alloc(); }
  ~Packet() { av_packet_free(&value); }
};
struct Parameters {
  AVCodecParameters *value = avcodec_parameters_alloc();
  Parameters() { if (!value) throw std::bad_alloc(); }
  ~Parameters() { avcodec_parameters_free(&value); }
};
void probe(const StorageVideoEncoderCandidate &candidate, StorageVideoCodecSupport &support,
           bool preferHardware = true) {
  StoragePolicy policy; policy.videoCodec = support.codec;
  Codec encoder, decoder;
  const auto codec = avcodec_find_encoder_by_name(candidate.name.c_str());
  if (!codec) throw std::runtime_error("Encoder is not included in this build");
  encoder.value = avcodec_alloc_context3(codec);
  if (!encoder.value) throw std::bad_alloc();
  auto &context = *encoder.value;
  context.width = 320; context.height = 240;
  context.time_base = {1, 25}; context.framerate = {25, 1};
  context.sample_aspect_ratio = {1, 1};
  context.color_range = AVCOL_RANGE_MPEG;
  context.colorspace = AVCOL_SPC_BT709;
  context.color_primaries = AVCOL_PRI_BT709;
  context.color_trc = AVCOL_TRC_BT709;
  context.chroma_sample_location = AVCHROMA_LOC_LEFT;
  context.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  std::string reason;
  if (!ConfigureStorageVideoEncoder(&context, candidate, policy, reason))
    throw std::runtime_error(reason);

  // Match VideoPlayer's decoder negotiation and native import. A device
  // decoder advertised by the OS alone is not sufficient for app playback.
  const auto playback = avcodec_find_decoder(StorageVideoCodecId(support.codec));
  if (!playback) throw std::runtime_error("App playback decoder is unavailable");
  decoder.value = avcodec_alloc_context3(playback);
  if (!decoder.value) throw std::bad_alloc();
  Parameters parameters;
  require(avcodec_parameters_from_context(parameters.value, &context), "Read probe codec parameters");
  require(avcodec_parameters_to_context(decoder.value, parameters.value), "Configure playback probe");
  decoder.value->pkt_timebase = context.time_base;
  decoder.value->thread_count = 1;
  if (preferHardware) video::configureHardwareVideoDecoder(decoder.value);
  require(avcodec_open2(decoder.value, playback, nullptr), "Open app playback decoder");

  Frame input, decoded, pixels;
  input.value->format = context.pix_fmt;
  input.value->width = context.width; input.value->height = context.height;
  input.value->color_range = context.color_range; input.value->colorspace = context.colorspace;
  input.value->color_primaries = context.color_primaries; input.value->color_trc = context.color_trc;
  input.value->chroma_location = context.chroma_sample_location;
  input.value->sample_aspect_ratio = context.sample_aspect_ratio;
  require(av_frame_get_buffer(input.value, 32), "Allocate probe frame");
  pixels.value->format = AV_PIX_FMT_YUV420P;
  pixels.value->width = context.width; pixels.value->height = context.height;
  require(av_frame_get_buffer(pixels.value, 32), "Allocate playback pixels");
  int frames = 0;
  bool nativeFrames = true;
  auto receiveDecoded = [&] {
    int status;
    while ((status = avcodec_receive_frame(decoder.value, decoded.value)) >= 0) {
      if ((decoded.value->flags & AV_FRAME_FLAG_CORRUPT) || decoded.value->decode_error_flags ||
          decoded.value->width != context.width || decoded.value->height != context.height ||
          decoded.value->pts != frames)
        throw std::runtime_error("Playback probe changed frame geometry or timestamps");
      if (decoded.value->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        if (!video::probeHardwareVideoFrame(*decoded.value))
          throw std::runtime_error("App playback cannot import the native video surface");
      } else {
        nativeFrames = false;
        std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler(
            sws_getContext(context.width, context.height, static_cast<AVPixelFormat>(decoded.value->format),
                           context.width, context.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
        if (!scaler || sws_scale(scaler.get(), decoded.value->data, decoded.value->linesize, 0, context.height,
                                pixels.value->data, pixels.value->linesize) != context.height)
          throw std::runtime_error("App playback cannot convert decoded frames");
      }
      ++frames;
      av_frame_unref(decoded.value);
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) require(status, "Decode playback probe");
  };
  Packet packet;
  auto send = [&](AVFrame *frame) {
    require(avcodec_send_frame(&context, frame), "Encode capability probe");
    int status;
    while ((status = avcodec_receive_packet(&context, packet.value)) >= 0) {
      require(avcodec_send_packet(decoder.value, packet.value), "Send playback probe packet");
      receiveDecoded();
      av_packet_unref(packet.value);
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) require(status, "Receive capability probe packet");
  };
  for (int i = 0; i < 3; ++i) {
    require(av_frame_make_writable(input.value), "Prepare probe frame");
    for (int row = 0; row < context.height; ++row)
      for (int col = 0; col < context.width; ++col)
        input.value->data[0][row * input.value->linesize[0] + col] = 32 + (col + row + i * 13) % 180;
    const bool nv12 = context.pix_fmt == AV_PIX_FMT_NV12;
    for (int plane = 1; plane < (nv12 ? 2 : 3); ++plane)
      for (int row = 0; row < context.height / 2; ++row)
        std::memset(input.value->data[plane] + row * input.value->linesize[plane], 128,
                    nv12 ? context.width : context.width / 2);
    input.value->pts = i; input.value->duration = 1;
    send(input.value);
  }
  send(nullptr);
  require(avcodec_send_packet(decoder.value, nullptr), "Flush playback probe");
  receiveDecoded();
  if (frames != 3) throw std::runtime_error("Playback probe did not decode all frames");
  support.available = true; support.hardware = candidate.hardware;
  support.encoder = candidate.name; support.playbackAvailable = true;
  support.playbackHardware = nativeFrames && video::probeHardwareVideoDecoder(*decoder.value);
  support.decoder = nativeFrames ? "VideoToolbox" : playback->name;
  support.reason.clear();
}
StorageVideoCapabilities unavailable(const char *reason) {
  StorageVideoCapabilities result;
  for (std::size_t i = 0; i < result.size(); ++i) {
    result[i].codec = static_cast<VideoCodec>(i); result[i].reason = reason;
  }
  return result;
}
struct ProbeCache {
  std::mutex mutex;
  std::shared_future<StorageVideoCapabilities> future;
  std::vector<std::function<void()>> callbacks;
  std::jthread worker;
};
}
StorageVideoCapabilities ProbeStorageVideoCapabilities() {
  auto result = unavailable("No working hardware encoder and app playback combination is available");
  const auto mp4 = av_guess_format("mp4", nullptr, nullptr);
  if (!mp4) return unavailable("MP4 output is unavailable in this build");
  for (auto &support : result) {
    if (avformat_query_codec(mp4, StorageVideoCodecId(support.codec), FF_COMPLIANCE_NORMAL) <= 0) {
      support.reason = "MP4 does not support this codec in this build";
      continue;
    }
    const int bitrate = StorageVideoEncoderBitrate(support.codec, VideoQuality::Balanced, 320, 240, 25);
    for (const auto &candidate : StorageVideoEncoderCandidates(support.codec, 320, 240, 25, bitrate)) {
      try {
        try { probe(candidate, support); }
        catch (...) { probe(candidate, support, false); }
        break;
      }
      catch (const std::exception &error) { support.reason = error.what(); }
    }
  }
  return result;
}
std::shared_future<StorageVideoCapabilities> RequestStorageVideoCapabilities(std::function<void()> wake) {
  static ProbeCache cache;
  std::unique_lock lock(cache.mutex);
  if (!cache.future.valid()) {
    std::promise<StorageVideoCapabilities> promise;
    auto future = promise.get_future().share();
    cache.worker = std::jthread([promise = std::move(promise)]() mutable {
      StorageVideoCapabilities result;
      try { result = ProbeStorageVideoCapabilities(); }
      catch (const std::exception &error) { result = unavailable(error.what()); }
      catch (...) { result = unavailable("Video capability probe failed"); }
      std::vector<std::function<void()>> callbacks;
      {
        std::lock_guard guard(cache.mutex);
        promise.set_value(std::move(result));
        callbacks.swap(cache.callbacks);
      }
      for (auto &callback : callbacks) { try { callback(); } catch (...) {} }
    });
    cache.future = std::move(future);
  }
  const auto result = cache.future;
  if (wake && result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    cache.callbacks.push_back(std::move(wake));
    wake = {};
  }
  lock.unlock();
  if (wake) wake();
  return result;
}
} // namespace storage_management
