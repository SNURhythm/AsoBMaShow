#pragma once

#include "StorageTestSupport.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/display.h>
}
#include <cmath>
#include <vector>

namespace storage_test {
struct VideoFixture {
  int width = 160, height = 120;
  AVRational rate{25, 1};
  std::vector<std::int64_t> timestamps;
  bool h264 = false, audio = false;
  bool alpha = false, hdr = false, interlaced = false;
  AVRational sampleAspectRatio{1, 1};
  int rotation = 0;
  bool scaledDisplayMatrix = false;
};
inline void makeVideo(const std::filesystem::path &path, VideoFixture fixture) {
  struct Resources {
    AVFormatContext *mux = nullptr;
    AVCodecContext *encoder = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    ~Resources() {
      av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&encoder);
      if (mux) { if (mux->pb) avio_closep(&mux->pb); avformat_free_context(mux); }
    }
  } resources;
  if (fixture.timestamps.empty()) for (int i = 0; i < 30; ++i) fixture.timestamps.push_back(i);
  // NUT preserves the codec's exact rational timestamps, including deliberate
  // VFR gaps, without a millisecond container clock rounding the fixture.
  const char *container = fixture.rotation || fixture.scaledDisplayMatrix ? "mp4" : "nut";
  require(avformat_alloc_output_context2(&resources.mux, nullptr, container, path.string().c_str()) >= 0,
          "create fixture container");
  const auto codec = fixture.alpha ? avcodec_find_encoder(AV_CODEC_ID_FFV1) :
                     fixture.h264 ? avcodec_find_encoder_by_name("libx264") : avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);
  require(codec, "fixture encoder present");
  auto stream = avformat_new_stream(resources.mux, nullptr);
  resources.encoder = avcodec_alloc_context3(codec);
  auto &encoder = *resources.encoder;
  encoder.width = fixture.width; encoder.height = fixture.height;
  encoder.pix_fmt = fixture.alpha ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P;
  encoder.time_base = av_inv_q(fixture.rate); encoder.framerate = fixture.rate;
  encoder.sample_aspect_ratio = fixture.sampleAspectRatio; encoder.thread_count = 1;
  if (fixture.hdr) { encoder.color_trc = AVCOL_TRC_SMPTE2084; encoder.color_primaries = AVCOL_PRI_BT2020; }
  if (fixture.interlaced) { encoder.flags |= AV_CODEC_FLAG_INTERLACED_DCT; encoder.field_order = AV_FIELD_TT; }
  encoder.max_b_frames = 0; encoder.gop_size = 12; encoder.bit_rate = 3000000;
  encoder.qmin = 2; encoder.qmax = 2;
  if (resources.mux->oformat->flags & AVFMT_GLOBALHEADER) encoder.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (fixture.h264) {
    av_opt_set(encoder.priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder.priv_data, "crf", "5", 0);
  }
  require(avcodec_open2(&encoder, codec, nullptr) >= 0, "open fixture encoder");
  require(avcodec_parameters_from_context(stream->codecpar, &encoder) >= 0, "fixture video parameters");
  stream->time_base = encoder.time_base; stream->avg_frame_rate = fixture.rate;
  if (fixture.rotation || fixture.scaledDisplayMatrix) {
    auto side = av_packet_side_data_new(&stream->codecpar->coded_side_data, &stream->codecpar->nb_coded_side_data,
                                        AV_PKT_DATA_DISPLAYMATRIX, sizeof(std::int32_t) * 9, 0);
    require(side, "fixture display matrix");
    av_display_rotation_set(reinterpret_cast<std::int32_t *>(side->data), fixture.rotation);
    if (fixture.scaledDisplayMatrix) reinterpret_cast<std::int32_t *>(side->data)[0] *= 2;
  }
  AVStream *audio = nullptr;
  if (fixture.audio) {
    audio = avformat_new_stream(resources.mux, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->format = AV_SAMPLE_FMT_S16;
    audio->codecpar->sample_rate = 48000;
    av_channel_layout_default(&audio->codecpar->ch_layout, 2);
    audio->codecpar->bits_per_coded_sample = 16;
    audio->codecpar->block_align = 4;
    audio->codecpar->bit_rate = 48000 * 32;
    audio->time_base = {1, 48000};
  }
  require(avio_open(&resources.mux->pb, path.string().c_str(), AVIO_FLAG_WRITE) >= 0 &&
              avformat_write_header(resources.mux, nullptr) >= 0, "fixture header");
  auto &frame = *resources.frame;
  frame.width = encoder.width; frame.height = encoder.height; frame.format = encoder.pix_fmt;
  require(av_frame_get_buffer(&frame, 32) >= 0, "fixture frame buffer");
  auto drain = [&] {
    int status;
    while ((status = avcodec_receive_packet(&encoder, resources.packet)) >= 0) {
      resources.packet->duration = 1;
      av_packet_rescale_ts(resources.packet, encoder.time_base, stream->time_base);
      resources.packet->stream_index = stream->index;
      require(av_interleaved_write_frame(resources.mux, resources.packet) >= 0, "fixture video packet");
      av_packet_unref(resources.packet);
    }
    require(status == AVERROR(EAGAIN) || status == AVERROR_EOF, "fixture encoder drain");
  };
  for (std::size_t i = 0; i < fixture.timestamps.size(); ++i) {
    require(av_frame_make_writable(&frame) >= 0, "fixture writable frame");
    if (fixture.alpha) {
      for (int y = 0; y < encoder.height; ++y)
        for (int x = 0; x < encoder.width * 4; ++x) frame.data[0][y * frame.linesize[0] + x] = (x + y + i) % 255;
    } else {
    for (int y = 0; y < encoder.height; ++y)
      for (int x = 0; x < encoder.width; ++x)
        frame.data[0][y * frame.linesize[0] + x] = 16 + ((x * 7 + y * 3 + int(i) * 11) % 220);
    for (int plane = 1; plane < 3; ++plane)
      for (int y = 0; y < encoder.height / 2; ++y)
        for (int x = 0; x < encoder.width / 2; ++x)
          frame.data[plane][y * frame.linesize[plane] + x] = 90 + ((x + y + plane * 19) % 60);
    }
    if (fixture.interlaced) frame.flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    frame.pts = fixture.timestamps[i]; frame.duration = 1;
    require(avcodec_send_frame(&encoder, &frame) >= 0, "fixture encode frame"); drain();
  }
  require(avcodec_send_frame(&encoder, nullptr) >= 0, "fixture encoder flush"); drain();
  if (audio) {
    const auto total = av_rescale_q(fixture.timestamps.back() + 1, encoder.time_base, audio->time_base);
    for (std::int64_t position = 0; position < total; position += 1024) {
      const int frames = static_cast<int>(std::min<std::int64_t>(1024, total - position));
      require(av_new_packet(resources.packet, frames * 4) >= 0, "fixture audio packet");
      for (int i = 0; i < frames; ++i) {
        const auto value = static_cast<std::int16_t>(std::sin((position + i) * .06) * 10000);
        for (int channel = 0; channel < 2; ++channel) {
          resources.packet->data[(i * 2 + channel) * 2] = value & 255;
          resources.packet->data[(i * 2 + channel) * 2 + 1] = (value >> 8) & 255;
        }
      }
      resources.packet->pts = resources.packet->dts = position;
      resources.packet->duration = frames; resources.packet->stream_index = audio->index;
      require(av_interleaved_write_frame(resources.mux, resources.packet) >= 0, "fixture audio packet mux");
      av_packet_unref(resources.packet);
    }
  }
  require(av_write_trailer(resources.mux) >= 0, "fixture trailer");
}

struct VideoObservation {
  int width = 0, height = 0, audioChannels = 0, audioRate = 0;
  bool h264 = false;
  int rotation = 0;
  AVRational sampleAspectRatio{1, 1};
  std::vector<double> timestamps;
  double videoEnd = 0, audioEnd = 0;
};
inline VideoObservation inspectVideo(const std::filesystem::path &path) {
  struct Resources {
    AVFormatContext *demux = nullptr;
    std::vector<AVCodecContext *> decoders;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    ~Resources() {
      for (auto &decoder : decoders) avcodec_free_context(&decoder);
      av_packet_free(&packet); av_frame_free(&frame); avformat_close_input(&demux);
    }
  } r;
  require(avformat_open_input(&r.demux, path.string().c_str(), nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(r.demux, nullptr) >= 0, "open encoded video");
  VideoObservation result;
  for (unsigned i = 0; i < r.demux->nb_streams; ++i) {
    auto stream = r.demux->streams[i];
    auto decoder = avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id));
    require(decoder && avcodec_parameters_to_context(decoder, stream->codecpar) >= 0, "output decoder parameters");
    decoder->pkt_timebase = stream->time_base;
    require(avcodec_open2(decoder, decoder->codec, nullptr) >= 0, "open output decoder");
    r.decoders.push_back(decoder);
    if (decoder->codec_type == AVMEDIA_TYPE_VIDEO) {
      result.width = decoder->width; result.height = decoder->height;
      result.h264 = decoder->codec_id == AV_CODEC_ID_H264;
      result.sampleAspectRatio = av_guess_sample_aspect_ratio(r.demux, stream, nullptr);
      if (const auto side = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX))
        result.rotation = static_cast<int>(std::round(av_display_rotation_get(reinterpret_cast<const std::int32_t *>(side->data))));
    } else {
      result.audioChannels = decoder->ch_layout.nb_channels; result.audioRate = decoder->sample_rate;
    }
  }
  auto drain = [&](unsigned index) {
    auto decoder = r.decoders[index]; auto stream = r.demux->streams[index];
    int status;
    while ((status = avcodec_receive_frame(decoder, r.frame)) >= 0) {
      require(r.frame->best_effort_timestamp != AV_NOPTS_VALUE, "decoded output has presentation timestamp");
      const double time = r.frame->best_effort_timestamp * av_q2d(stream->time_base);
      if (decoder->codec_type == AVMEDIA_TYPE_VIDEO) {
        result.timestamps.push_back(time);
        result.videoEnd = time + r.frame->duration * av_q2d(stream->time_base);
      } else result.audioEnd = time + double(r.frame->nb_samples) / r.frame->sample_rate;
      av_frame_unref(r.frame);
    }
    require(status == AVERROR(EAGAIN) || status == AVERROR_EOF, "output fully decodes");
  };
  while (av_read_frame(r.demux, r.packet) >= 0) {
    require(avcodec_send_packet(r.decoders[r.packet->stream_index], r.packet) >= 0, "output packet decodes");
    drain(r.packet->stream_index); av_packet_unref(r.packet);
  }
  for (unsigned i = 0; i < r.decoders.size(); ++i) {
    require(avcodec_send_packet(r.decoders[i], nullptr) >= 0, "output decoder flush"); drain(i);
  }
  return result;
}
inline std::vector<std::vector<std::uint8_t>> audioPackets(const std::filesystem::path &path) {
  AVFormatContext *input = nullptr;
  require(avformat_open_input(&input, path.string().c_str(), nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(input, nullptr) >= 0, "open audio remux observation");
  auto packet = av_packet_alloc();
  std::vector<std::vector<std::uint8_t>> result;
  while (av_read_frame(input, packet) >= 0) {
    if (input->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      result.emplace_back(packet->data, packet->data + packet->size);
    av_packet_unref(packet);
  }
  av_packet_free(&packet); avformat_close_input(&input);
  return result;
}
} // namespace storage_test
