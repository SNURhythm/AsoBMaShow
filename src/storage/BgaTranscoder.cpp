#include "BgaTranscoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace storage_management {
namespace fs = std::filesystem;
namespace {
struct Failure {
  StorageReason reason;
  std::string detail;
  FileOutcome outcome = FileOutcome::Failed;
};
void checked(int status, const char *operation, StorageReason reason = StorageReason::ValidationFailed) {
  if (status >= 0) return;
  char error[AV_ERROR_MAX_STRING_SIZE]{}; av_strerror(status, error, sizeof(error));
  throw Failure{reason, std::string(operation) + ": " + error};
}
void checkpoint(const Checkpoint &check) {
  if (check && !check()) throw Failure{StorageReason::Cancelled, "Cancelled", FileOutcome::Cancelled};
}
void unsupported(const char *message) {
  throw Failure{StorageReason::UnsupportedFormat, message, FileOutcome::Skipped};
}
std::string mediaPath(const fs::path &path) {
  const auto utf8 = path.u8string(); return {utf8.begin(), utf8.end()};
}
struct FrameDeleter { void operator()(AVFrame *frame) const { av_frame_free(&frame); } };
struct PacketDeleter { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
using Frame = std::unique_ptr<AVFrame, FrameDeleter>;
using Packet = std::unique_ptr<AVPacket, PacketDeleter>;
Frame frame() {
  Frame result(av_frame_alloc()); if (!result) throw std::bad_alloc(); return result;
}
Packet packet() {
  Packet result(av_packet_alloc()); if (!result) throw std::bad_alloc(); return result;
}
struct Codec {
  AVCodecContext *value = nullptr;
  ~Codec() { avcodec_free_context(&value); }
};
struct Input {
  AVFormatContext *format = nullptr;
  std::vector<AVCodecContext *> decoders;
  int video = -1, audio = -1;
  explicit Input(const fs::path &path) {
    try {
      checked(avformat_open_input(&format, mediaPath(path).c_str(), nullptr, nullptr), "Open media");
      const std::string_view container = format->iformat->name;
      if (container.find("image2") != std::string_view::npos || container.ends_with("_pipe") ||
          container == "gif" || container == "apng") unsupported("Still images and image sequences are preserved");
      checked(avformat_find_stream_info(format, nullptr), "Read media streams");
      if (format->nb_streams > 2 || format->nb_chapters) unsupported("Extra streams or chapters cannot be preserved");
      for (unsigned i = 0; i < format->nb_streams; ++i) {
        const auto stream = format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video < 0 && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = i;
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio < 0) audio = i;
        else unsupported("Only one video stream and one optional audio stream are supported");
        if (stream->time_base.num <= 0 || stream->time_base.den <= 0) unsupported("Unknown stream time base");
        const auto decoder = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) unsupported("Source decoder is unavailable");
        auto context = avcodec_alloc_context3(decoder);
        if (!context) throw std::bad_alloc();
        decoders.push_back(context);
        checked(avcodec_parameters_to_context(context, stream->codecpar), "Read decoder parameters");
        context->pkt_timebase = stream->time_base;
        context->thread_count = StorageExecutionLimits::videoThreads;
        context->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
        context->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
        checked(avcodec_open2(context, decoder, nullptr), "Open source decoder");
      }
      if (video < 0) unsupported("No ordinary video stream");
    } catch (...) { close(); throw; }
  }
  void close() {
    for (auto &decoder : decoders) avcodec_free_context(&decoder);
    decoders.clear(); avformat_close_input(&format);
  }
  ~Input() { close(); }
};

template<class OnFrame, class OnPacket>
void decode(Input &input, const Checkpoint &check, OnFrame onFrame, OnPacket onPacket) {
  auto decoded = frame(); auto compressed = packet();
  auto drain = [&](int index) {
    int status;
    while ((status = avcodec_receive_frame(input.decoders[index], decoded.get())) >= 0) {
      checkpoint(check);
      if ((decoded->flags & AV_FRAME_FLAG_CORRUPT) || decoded->decode_error_flags)
        throw Failure{StorageReason::ValidationFailed, "Corrupt decoded media frame"};
      onFrame(index, *decoded);
      av_frame_unref(decoded.get());
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) checked(status, "Decode media frame");
  };
  int status;
  while ((status = av_read_frame(input.format, compressed.get())) >= 0) {
    checkpoint(check);
    const int index = compressed->stream_index;
    if (index < 0 || index >= static_cast<int>(input.decoders.size()) || (compressed->flags & AV_PKT_FLAG_CORRUPT))
      throw Failure{StorageReason::ValidationFailed, "Corrupt media packet"};
    onPacket(index, *compressed);
    checked(avcodec_send_packet(input.decoders[index], compressed.get()), "Send media packet");
    drain(index); av_packet_unref(compressed.get());
  }
  if (status != AVERROR_EOF) checked(status, "Read media packet");
  if (input.format->pb && input.format->pb->error < 0 && input.format->pb->error != AVERROR_EOF)
    checked(input.format->pb->error, "Read media file");
  for (int index = 0; index < static_cast<int>(input.decoders.size()); ++index) {
    checked(avcodec_send_packet(input.decoders[index], nullptr), "Flush media decoder"); drain(index);
  }
}

struct Timeline {
  AVRational timeBase{0, 1};
  std::int64_t first = AV_NOPTS_VALUE, last = AV_NOPTS_VALUE, end = AV_NOPTS_VALUE;
  std::int64_t minimumInterval = 0, maximumInterval = 0, maximumDuration = 0;
  std::uint64_t frames = 0;
  bool discontinuous = false;
  void add(std::int64_t pts, std::int64_t duration, std::int64_t overlapTolerance = 0) {
    if (pts == AV_NOPTS_VALUE || duration <= 0 || pts > std::numeric_limits<std::int64_t>::max() - duration ||
        (last != AV_NOPTS_VALUE && pts <= last))
      unsupported("Unknown or non-monotonic presentation timestamps");
    const auto nextEnd = pts + duration;
    // Bound the whole presentation span, not just each adjacent interval.
    // This also makes pts-last, pts-end and end-first representable.
    if (first != AV_NOPTS_VALUE && first < 0 &&
        nextEnd > std::numeric_limits<std::int64_t>::max() + first)
      unsupported("Timestamp range is invalid");
    if (first == AV_NOPTS_VALUE) first = pts;
    else {
      const auto interval = pts - last;
      minimumInterval = minimumInterval ? std::min(minimumInterval, interval) : interval;
      maximumInterval = std::max(maximumInterval, interval);
      if (std::abs(pts - end) > overlapTolerance) discontinuous = true;
    }
    last = pts; end = nextEnd; maximumDuration = std::max(maximumDuration, duration); ++frames;
  }
};
std::uint64_t timestampDistance(std::int64_t left, std::int64_t right) {
  // Unsigned subtraction represents the full signed timestamp span exactly,
  // including distances greater than INT64_MAX, without overflowing abs().
  return left >= right ? static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right) :
                         static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}
std::int64_t timestampAdd(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    unsupported("Timestamp addition is out of range");
  const auto result = left + right;
  if (result == AV_NOPTS_VALUE) unsupported("Timestamp is out of range");
  return result;
}
std::int64_t timestampSubtract(std::int64_t left, std::int64_t right) {
  if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
      (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right))
    unsupported("Timestamp difference is out of range");
  const auto result = left - right;
  if (result == AV_NOPTS_VALUE) unsupported("Timestamp is out of range");
  return result;
}
std::int64_t rescaleTimestamp(std::int64_t value, AVRational source, AVRational destination) {
  if (value == AV_NOPTS_VALUE || source.num <= 0 || source.den <= 0 || destination.num <= 0 || destination.den <= 0)
    unsupported("Unknown timestamp or time base");
  const auto result = av_rescale_q(value, source, destination);
  // FFmpeg returns INT64_MIN when the rescaled value is unrepresentable.
  if (result == AV_NOPTS_VALUE) unsupported("Rescaled timestamp is out of range");
  return result;
}
std::int64_t normalizedTimestamp(std::int64_t value, AVRational source, std::int64_t originMicros,
                                 AVRational destination) {
  return timestampSubtract(rescaleTimestamp(value, source, destination),
                           rescaleTimestamp(originMicros, AV_TIME_BASE_Q, destination));
}
void validateTimelineOrigin(const Timeline &timeline, std::int64_t originMicros, AVRational destination) {
  normalizedTimestamp(timeline.first, timeline.timeBase, originMicros, destination);
  normalizedTimestamp(timeline.end, timeline.timeBase, originMicros, destination);
}
void rescalePacketTimestamps(AVPacket &packet, AVRational source, AVRational destination) {
  if (packet.pts != AV_NOPTS_VALUE) packet.pts = rescaleTimestamp(packet.pts, source, destination);
  if (packet.dts != AV_NOPTS_VALUE) packet.dts = rescaleTimestamp(packet.dts, source, destination);
  if (packet.duration > 0) packet.duration = rescaleTimestamp(packet.duration, source, destination);
}
void advanceAudioInput(std::int64_t pts, int samples, AVRational sourceClock, AVRational sampleClock,
                       std::int64_t &next) {
  if (samples <= 0 || samples > 1048576) unsupported("Unsupported audio frame size");
  const auto sourceTick = av_rescale_q_rnd(1, sourceClock, sampleClock, AV_ROUND_UP);
  if (sourceTick <= 0) unsupported("Audio clock is out of range");
  if (next == AV_NOPTS_VALUE) next = pts;
  if (timestampDistance(pts, next) > static_cast<std::uint64_t>(sourceTick))
    unsupported("Discontinuous audio cannot be transcoded faithfully");
  // Coarse container timestamps round sample boundaries. Keep the first-frame
  // sample clock as the anchor so bounded rounding cannot accumulate drift.
  next = timestampAdd(next, samples);
}
void validateDeclaredVideoTimeline(const AVStream &video, const Timeline &decoded) {
  if (video.start_time == AV_NOPTS_VALUE) return;
  if (timestampDistance(decoded.first, video.start_time) > 1)
    throw Failure{StorageReason::ValidationFailed, "Decoder lost the opening presentation interval"};
  if (video.duration <= 0) return;
  if (video.start_time > std::numeric_limits<std::int64_t>::max() - video.duration)
    throw Failure{StorageReason::ValidationFailed, "Declared video end is out of range"};
  const auto declaredEnd = video.start_time + video.duration;
  if (timestampDistance(declaredEnd, decoded.end) > static_cast<std::uint64_t>(decoded.maximumDuration))
    throw Failure{StorageReason::ValidationFailed, "Decoded video does not match its declared duration (declared end=" +
        std::to_string(declaredEnd) + ", decoded end=" + std::to_string(decoded.end) + ", tolerance=" +
        std::to_string(decoded.maximumDuration) + ", time base=" + std::to_string(video.time_base.num) + "/" +
        std::to_string(video.time_base.den) + ")"};
}
struct Scan {
  VideoDescription description;
  Timeline video, audio;
  int videoDisposition = 0, audioDisposition = 0;
  Frame audioFormat;
  std::int64_t originMicros = 0;
  AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
  AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
};
bool sameVideoColor(const Scan &left, const Scan &right) {
  return left.colorRange == right.colorRange && left.colorSpace == right.colorSpace &&
         left.colorPrimaries == right.colorPrimaries && left.colorTransfer == right.colorTransfer;
}
void observeVideoColor(Scan &result, const AVCodecParameters &params, const AVFrame &decoded) {
  Scan effective;
  effective.colorRange = decoded.color_range == AVCOL_RANGE_UNSPECIFIED ? params.color_range : decoded.color_range;
  effective.colorSpace = decoded.colorspace == AVCOL_SPC_UNSPECIFIED ? params.color_space : decoded.colorspace;
  effective.colorPrimaries = decoded.color_primaries == AVCOL_PRI_UNSPECIFIED ? params.color_primaries : decoded.color_primaries;
  effective.colorTransfer = decoded.color_trc == AVCOL_TRC_UNSPECIFIED ? params.color_trc : decoded.color_trc;
  // sws_getCoefficients silently substitutes its default for unsupported matrices.
  // Only accept matrices that this conversion path can actually preserve.
  switch (effective.colorSpace) {
  case AVCOL_SPC_UNSPECIFIED: case AVCOL_SPC_BT709: case AVCOL_SPC_FCC:
  case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_SMPTE240M:
  case AVCOL_SPC_BT2020_NCL: break;
  default: unsupported("Video color matrix cannot be preserved");
  }
  if (!av_color_range_name(effective.colorRange) || !av_color_primaries_name(effective.colorPrimaries) ||
      !av_color_transfer_name(effective.colorTransfer)) unsupported("Unknown video color metadata");
  if (result.video.frames && !sameVideoColor(result, effective)) unsupported("Video changes color metadata midstream");
  result.colorRange = effective.colorRange; result.colorSpace = effective.colorSpace;
  result.colorPrimaries = effective.colorPrimaries; result.colorTransfer = effective.colorTransfer;
}
void validateVideoFramePresentation(const AVCodecParameters &params, const AVFrame &decoded) {
  // The encoder receives newly scaled frames, so these playback properties
  // would disappear. Preserve the source instead. Encoder hints, timecodes and
  // descriptive SEI are intentionally allowed to disappear; they do not change
  // the decoded presentation. Existing-target reuse never calls this check.
  for (int i = 0; i < decoded.nb_side_data; ++i) {
    const auto &side = *decoded.side_data[i];
    switch (side.type) {
    case AV_FRAME_DATA_PANSCAN: {
      if (side.size != sizeof(AVPanScan)) unsupported("Invalid decoded pan-scan metadata");
      const auto &pan = *reinterpret_cast<const AVPanScan *>(side.data);
      // MPEG decoders attach an empty default to ordinary frames. A full-frame
      // rectangle with no offset is also redundant with the copied geometry.
      // Only an actual crop/offset needs preservation beyond the scaled pixels.
      const bool unchangedSize = (!pan.width && !pan.height) ||
          (pan.width == std::int64_t(decoded.width) * 16 && pan.height == std::int64_t(decoded.height) * 16);
      if (!unchangedSize) unsupported("Decoded pan-scan crop cannot be preserved");
      for (const auto &position : pan.position)
        if (position[0] || position[1]) unsupported("Decoded pan-scan offset cannot be preserved");
      break;
    }
    case AV_FRAME_DATA_A53_CC:
    case AV_FRAME_DATA_AFD:
    case AV_FRAME_DATA_STEREO3D: case AV_FRAME_DATA_SPHERICAL:
    case AV_FRAME_DATA_VIEW_ID: case AV_FRAME_DATA_3D_REFERENCE_DISPLAYS:
    case AV_FRAME_DATA_ICC_PROFILE:
    case AV_FRAME_DATA_MASTERING_DISPLAY_METADATA: case AV_FRAME_DATA_CONTENT_LIGHT_LEVEL:
    case AV_FRAME_DATA_DYNAMIC_HDR_PLUS: case AV_FRAME_DATA_DYNAMIC_HDR_VIVID:
    case AV_FRAME_DATA_DOVI_RPU_BUFFER: case AV_FRAME_DATA_DOVI_METADATA:
    case AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT:
    case AV_FRAME_DATA_FILM_GRAIN_PARAMS: case AV_FRAME_DATA_LCEVC:
      unsupported("Decoded video playback metadata cannot be preserved");
    default: break;
    }
  }
  const auto frameMatrix = av_frame_get_side_data(&decoded, AV_FRAME_DATA_DISPLAYMATRIX);
  if (!frameMatrix) return;
  // Decoders may repeat the validated stream transform on each frame. That
  // exact matrix survives the MP4 copy; frame-only transforms and changes do
  // not (even identity can reset an earlier orientation).
  const auto streamMatrix = av_packet_side_data_get(params.coded_side_data, params.nb_coded_side_data,
                                                    AV_PKT_DATA_DISPLAYMATRIX);
  constexpr auto matrixBytes = 9 * sizeof(std::int32_t);
  if (!streamMatrix || frameMatrix->size != matrixBytes || streamMatrix->size != matrixBytes ||
      std::memcmp(frameMatrix->data, streamMatrix->data, matrixBytes))
    unsupported("Decoded video frame display transforms cannot be preserved");
}
void validateAudioFrame(const AVFrame &expected, const AVFrame &decoded) {
  if (decoded.sample_rate != expected.sample_rate || decoded.format != expected.format ||
      av_channel_layout_compare(&decoded.ch_layout, &expected.ch_layout) != 0)
    unsupported("Embedded audio changes format midstream");
}
void validatePlaybackGainMetadata(const AVDictionary *metadata) {
  // Descriptions remain best effort, including unknown custom text. These
  // known gain/peak tags instead control playback volume and cannot disappear
  // when MP4 drops the source dictionary. Matching is case-insensitive.
  for (const auto key : {"REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK",
                         "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK",
                         "R128_TRACK_GAIN", "R128_ALBUM_GAIN"})
    if (av_dict_get(metadata, key, nullptr, 0))
      unsupported("Audio playback gain metadata cannot be preserved in MP4");
}
bool hdr(AVColorTransferCharacteristic transfer) {
  return transfer == AVCOL_TRC_SMPTE2084 || transfer == AVCOL_TRC_ARIB_STD_B67;
}
bool audioCanRemux(AVCodecID codec) {
  return codec == AV_CODEC_ID_AAC || codec == AV_CODEC_ID_MP3 || codec == AV_CODEC_ID_MP2 ||
         codec == AV_CODEC_ID_ALAC || codec == AV_CODEC_ID_AC3 || codec == AV_CODEC_ID_EAC3;
}
void validateDisposition(int disposition) {
  // MP4 track-kind tags combine each accessibility pair. A lone flag would
  // gain a different meaning on decode-back, so only accept complete pairs.
  constexpr int captions = AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_CAPTIONS;
  constexpr int descriptions = AV_DISPOSITION_VISUAL_IMPAIRED | AV_DISPOSITION_DESCRIPTIONS;
  constexpr int supported = AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED | AV_DISPOSITION_COMMENT |
                            AV_DISPOSITION_DUB | captions | descriptions;
  if ((disposition & ~supported) ||
      ((disposition & captions) && (disposition & captions) != captions) ||
      ((disposition & descriptions) && (disposition & descriptions) != descriptions))
    unsupported("Stream disposition cannot be preserved in MP4");
}
enum class ScanPurpose { SourceConversion, ExistingDestination, EncodedOutput };
Scan scan(const fs::path &path, const Checkpoint &check, ScanPurpose purpose = ScanPurpose::SourceConversion) {
  checkpoint(check);
  Input input(path);
  const bool existingDestination = purpose == ScanPurpose::ExistingDestination;
  if (existingDestination && (!std::strstr(input.format->iformat->name, "mp4") ||
      input.format->streams[input.video]->codecpar->codec_id != AV_CODEC_ID_H264))
    unsupported("Existing video must contain H264 in a native MP4 container");
  Scan result;
  auto &description = result.description;
  if (!existingDestination) {
    description.codecs = ProbeStorageCodecs();
    validatePlaybackGainMetadata(input.format->metadata);
  }
  const auto video = input.format->streams[input.video];
  result.videoDisposition = video->disposition;
  if (!existingDestination) validateDisposition(result.videoDisposition);
  const auto params = video->codecpar;
  description.width = params->width; description.height = params->height;
  description.sampleAspectRatio = av_guess_sample_aspect_ratio(input.format, video, nullptr);
  if (description.sampleAspectRatio.num <= 0) description.sampleAspectRatio = {1, 1};
  description.timeBase = result.video.timeBase = video->time_base;
  description.frameRate = av_guess_frame_rate(input.format, video, nullptr);
  description.h264 = params->codec_id == AV_CODEC_ID_H264;
  description.interlaced = params->field_order != AV_FIELD_UNKNOWN && params->field_order != AV_FIELD_PROGRESSIVE;
  for (int i = 0; i < params->nb_coded_side_data; ++i) {
    const auto &side = params->coded_side_data[i];
    if (side.type == AV_PKT_DATA_DISPLAYMATRIX && side.size == 9 * sizeof(std::int32_t)) {
      const auto matrix = reinterpret_cast<const std::int32_t *>(side.data);
      const double angle = av_display_rotation_get(matrix);
      if (!std::isfinite(angle) || std::abs(angle - std::round(angle / 90) * 90) > .01 ||
          matrix[2] || matrix[5] || matrix[6] || matrix[7] || matrix[8] != (1 << 30))
        description.unsupportedContent = true;
      else {
        description.rotation = static_cast<int>(std::round(angle / 90)) * 90;
        std::int32_t expected[9]{};
        av_display_rotation_set(expected, -description.rotation);
        if (!std::equal(expected, expected + 9, matrix)) description.unsupportedContent = true;
      }
    } else if (side.type != AV_PKT_DATA_CPB_PROPERTIES) description.unsupportedContent = true;
  }
  if (input.audio >= 0) {
    const auto audio = input.format->streams[input.audio];
    if (!existingDestination) {
      validatePlaybackGainMetadata(audio->metadata);
      if (av_packet_side_data_get(audio->codecpar->coded_side_data, audio->codecpar->nb_coded_side_data,
                                  AV_PKT_DATA_REPLAYGAIN))
        unsupported("Embedded audio playback gain cannot be preserved in MP4");
    }
    result.audioDisposition = audio->disposition;
    if (!existingDestination) validateDisposition(result.audioDisposition);
    description.hasAudio = true; result.audio.timeBase = audio->time_base;
    description.audioChannels = audio->codecpar->ch_layout.nb_channels;
    description.audioSampleRate = audio->codecpar->sample_rate;
    description.mp4CompatibleAudio = audioCanRemux(audio->codecpar->codec_id);
    const auto &layout = audio->codecpar->ch_layout;
    description.audioLayoutSupported = layout.order == AV_CHANNEL_ORDER_UNSPEC ||
        (layout.order == AV_CHANNEL_ORDER_NATIVE && (layout.u.mask == AV_CH_LAYOUT_MONO || layout.u.mask == AV_CH_LAYOUT_STEREO));
  }
  AVRational decodedAspectRatio{0, 1};
  decode(input, check, [&](int index, const AVFrame &decoded) {
    const auto pts = decoded.best_effort_timestamp;
    if (index == input.video) {
      if (!existingDestination) validateVideoFramePresentation(*params, decoded);
      if (!result.video.frames && !(decoded.flags & AV_FRAME_FLAG_KEY))
        throw Failure{StorageReason::ValidationFailed, "Video does not start with an independently decodable frame"};
      if (!existingDestination && (decoded.width != description.width || decoded.height != description.height))
        unsupported("Video changes resolution midstream");
      // Container SAR takes precedence in FFmpeg's display guess. Also track
      // coded frame SAR so that a fixed container value cannot hide changes.
      const auto codedAspectRatio = decoded.sample_aspect_ratio;
      if (codedAspectRatio.num > 0 && codedAspectRatio.den > 0) {
        if (!existingDestination && decodedAspectRatio.num > 0 && av_cmp_q(decodedAspectRatio, codedAspectRatio))
          unsupported("Video changes sample aspect ratio midstream");
        decodedAspectRatio = codedAspectRatio;
      }
      auto aspectRatio = av_guess_sample_aspect_ratio(input.format, video, const_cast<AVFrame *>(&decoded));
      if (aspectRatio.num <= 0 || aspectRatio.den <= 0) aspectRatio = {1, 1};
      if (!result.video.frames) description.sampleAspectRatio = aspectRatio;
      else if (!existingDestination && av_cmp_q(description.sampleAspectRatio, aspectRatio))
        unsupported("Video changes display aspect ratio midstream");
      // Reusing the existing bitstream does not transform its colors, geometry
      // or display metadata. Only conversion needs these preservation checks.
      if (!existingDestination) observeVideoColor(result, *params, decoded);
      const auto pixel = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decoded.format));
      if (!pixel) unsupported("Unknown pixel format");
      description.alpha |= (pixel->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
      description.hdr |= hdr(result.colorTransfer) || av_frame_get_side_data(&decoded, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) ||
                         av_frame_get_side_data(&decoded, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
      description.interlaced |= (decoded.flags & AV_FRAME_FLAG_INTERLACED) != 0;
      if (av_frame_get_side_data(&decoded, AV_FRAME_DATA_STEREO3D)) description.unsupportedContent = true;
      auto duration = decoded.duration;
      if (duration <= 0 && description.frameRate.num > 0 && description.frameRate.den > 0)
        duration = rescaleTimestamp(1, av_inv_q(description.frameRate), video->time_base);
      result.video.add(pts, duration);
      // Container clocks (notably Matroska milliseconds) round rational CFR
      // timestamps. Check the entire cadence within one input tick, rather
      // than treating alternating rounded intervals as genuine VFR bursts.
      if (description.frameRate.num > 0 && description.frameRate.den > 0) {
        const auto expected = rescaleTimestamp(result.video.frames - 1,
            av_inv_q(description.frameRate), video->time_base);
        if (timestampDistance(pts - result.video.first, expected) > 1) description.variableFrameRate = true;
      } else description.variableFrameRate = true;
    } else {
      if (!existingDestination && av_frame_get_side_data(&decoded, AV_FRAME_DATA_REPLAYGAIN))
        unsupported("Decoded audio playback gain cannot be preserved in MP4");
      if (!result.audioFormat) {
        if ((!existingDestination && (decoded.sample_rate != description.audioSampleRate || decoded.ch_layout.nb_channels != description.audioChannels)) ||
            av_get_bytes_per_sample(static_cast<AVSampleFormat>(decoded.format)) <= 0 ||
            !av_channel_layout_check(&decoded.ch_layout)) unsupported("Unknown embedded audio format");
        const auto &layout = decoded.ch_layout;
        description.audioLayoutSupported = layout.order == AV_CHANNEL_ORDER_UNSPEC ||
            (layout.order == AV_CHANNEL_ORDER_NATIVE && (layout.u.mask == AV_CH_LAYOUT_MONO || layout.u.mask == AV_CH_LAYOUT_STEREO));
        result.audioFormat = frame();
        result.audioFormat->sample_rate = decoded.sample_rate; result.audioFormat->format = decoded.format;
        checked(av_channel_layout_copy(&result.audioFormat->ch_layout, &layout), "Remember decoded audio layout");
      }
      if (!existingDestination) validateAudioFrame(*result.audioFormat, decoded);
      const auto duration = rescaleTimestamp(decoded.nb_samples, {1, decoded.sample_rate}, result.audio.timeBase);
      result.audio.add(pts, duration, std::max<std::int64_t>(1, rescaleTimestamp(1, {1, decoded.sample_rate}, result.audio.timeBase)));
    }
  }, [&](int index, const AVPacket &compressed) {
    if (!existingDestination && index == input.audio &&
        av_packet_get_side_data(&compressed, AV_PKT_DATA_REPLAYGAIN, nullptr))
      unsupported("Embedded audio packet playback gain cannot be preserved in MP4");
  });
  if (!result.video.frames || (description.hasAudio && !result.audio.frames))
    throw Failure{StorageReason::ValidationFailed, "Empty media stream"};
  // Source declarations are intentionally advisory: old files can have stale
  // starts, durations, ASF sizes/counts or Matroska endpoints despite a clean
  // decode. Preserve the actual decoded timeline, including clean early EOF.
  // Targets still need strict completeness checks before replacing a source.
  if (purpose != ScanPurpose::SourceConversion) validateDeclaredVideoTimeline(*video, result.video);
  description.duration = result.video.end - result.video.first;
  description.minimumFrameInterval = result.video.minimumInterval;
  description.audioDiscontinuous = result.audio.discontinuous;
  result.originMicros = rescaleTimestamp(result.video.first, result.video.timeBase, AV_TIME_BASE_Q);
  if (description.hasAudio) result.originMicros = std::min(result.originMicros, rescaleTimestamp(result.audio.first, result.audio.timeBase, AV_TIME_BASE_Q));
  validateTimelineOrigin(result.video, result.originMicros, AV_TIME_BASE_Q);
  if (description.hasAudio) validateTimelineOrigin(result.audio, result.originMicros, AV_TIME_BASE_Q);
  return result;
}

struct Output {
  AVFormatContext *format = nullptr;
  AVIOContext *io = nullptr;
  std::FILE *file = nullptr;
  static std::int64_t tell(std::FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
  }
  static int seekFile(std::FILE *file, std::int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(file, offset, whence);
#else
    return fseeko(file, offset, whence);
#endif
  }
  static int write(void *opaque, const std::uint8_t *data, int bytes) {
    return std::fwrite(data, 1, bytes, static_cast<std::FILE *>(opaque)) == static_cast<std::size_t>(bytes) ? bytes : AVERROR(EIO);
  }
  static std::int64_t seek(void *opaque, std::int64_t offset, int whence) {
    auto file = static_cast<std::FILE *>(opaque);
    if (whence & AVSEEK_SIZE) {
      const auto position = tell(file);
      if (seekFile(file, 0, SEEK_END)) return AVERROR(EIO);
      const auto size = tell(file); seekFile(file, position, SEEK_SET); return size;
    }
    return seekFile(file, offset, whence & ~AVSEEK_FORCE) ? AVERROR(EIO) : tell(file);
  }
  void open(const fs::path &path, bool &created) {
#ifdef _WIN32
    int descriptor = _wopen(path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (descriptor >= 0) { created = true; file = _fdopen(descriptor, "w+b"); }
    if (descriptor >= 0 && !file) _close(descriptor);
#else
    int descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) { created = true; file = fdopen(descriptor, "w+b"); }
    if (descriptor >= 0 && !file) ::close(descriptor);
#endif
    if (!file) throw Failure{StorageReason::IoError, "Cannot create exclusive temporary video"};
    auto buffer = static_cast<unsigned char *>(av_malloc(65536));
    if (!buffer) throw std::bad_alloc();
    io = avio_alloc_context(buffer, 65536, 1, file, nullptr, write, seek);
    if (!io) { av_free(buffer); throw std::bad_alloc(); }
    checked(avformat_alloc_output_context2(&format, nullptr, "mp4", nullptr), "Create MP4", StorageReason::CodecUnavailable);
    format->pb = io; format->flags |= AVFMT_FLAG_CUSTOM_IO;
    format->max_interleave_delta = AV_TIME_BASE;
    format->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
  }
  void finish() {
    checked(av_write_trailer(format), "Finalize MP4", StorageReason::IoError);
    avio_flush(io);
    if (io->error < 0) checked(io->error, "Flush MP4", StorageReason::IoError);
    if (std::fflush(file)) throw Failure{StorageReason::IoError, "Flush temporary video failed"};
  }
  ~Output() {
    if (format) avformat_free_context(format);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    if (file) std::fclose(file);
  }
};

struct AudioBuffers {
  SwrContext *resampler = nullptr;
  AVAudioFifo *fifo = nullptr;
  ~AudioBuffers() { swr_free(&resampler); if (fifo) av_audio_fifo_free(fifo); }
};
struct ScaleBuffer {
  SwsContext *value = nullptr;
  ~ScaleBuffer() { sws_freeContext(value); }
};
struct EncodedSummary { std::uint64_t frames = 0; };
EncodedSummary encode(const fs::path &source, const fs::path &destination, const Scan &sourceScan,
                       const VideoCompressionPlan &plan, const Checkpoint &check, bool &created) {
  validateTimelineOrigin(sourceScan.video, sourceScan.originMicros, plan.timeBase);
  if (sourceScan.description.hasAudio)
    validateTimelineOrigin(sourceScan.audio, sourceScan.originMicros,
        plan.audio == VideoAudioTreatment::Remux ? sourceScan.audio.timeBase : AVRational{1, sourceScan.description.audioSampleRate});
  Input input(source); Output output; output.open(destination, created);
  Codec videoEncoder, audioEncoder;
  ScaleBuffer scale; AudioBuffers audioBuffers;
  const auto videoCodec = avcodec_find_encoder_by_name("libx264");
  if (!videoCodec) throw Failure{StorageReason::CodecUnavailable, "libx264 is unavailable", FileOutcome::Skipped};
  videoEncoder.value = avcodec_alloc_context3(videoCodec);
  if (!videoEncoder.value) throw std::bad_alloc();
  auto &video = *videoEncoder.value;
  video.width = plan.width; video.height = plan.height; video.pix_fmt = AV_PIX_FMT_YUV420P;
  video.time_base = plan.timeBase; video.framerate = plan.frameRate;
  video.sample_aspect_ratio = plan.sampleAspectRatio;
  video.color_range = sourceScan.colorRange; video.colorspace = sourceScan.colorSpace;
  video.color_primaries = sourceScan.colorPrimaries; video.color_trc = sourceScan.colorTransfer;
  video.thread_count = StorageExecutionLimits::videoThreads; video.max_b_frames = 0;
  const auto gop = ((std::int64_t(plan.frameRate.num) + plan.frameRate.den - 1) / plan.frameRate.den) * 2;
  if (gop <= 0 || gop > std::numeric_limits<int>::max()) unsupported("Video frame rate exceeds encoder limits");
  video.gop_size = static_cast<int>(gop);
  if (output.format->oformat->flags & AVFMT_GLOBALHEADER) video.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  checked(av_opt_set(video.priv_data, "preset", "medium", 0), "Set x264 preset");
  checked(av_opt_set_int(video.priv_data, "crf", plan.crf, 0), "Set x264 CRF");
  checked(avcodec_open2(&video, videoCodec, nullptr), "Open x264", StorageReason::CodecUnavailable);
  auto videoStream = avformat_new_stream(output.format, nullptr);
  if (!videoStream) throw std::bad_alloc();
  checked(avcodec_parameters_from_context(videoStream->codecpar, &video), "Configure MP4 video");
  videoStream->time_base = plan.timeBase; videoStream->sample_aspect_ratio = plan.sampleAspectRatio;
  const auto sourceVideo = input.format->streams[input.video];
  // Dictionary copies are best effort: MP4 cannot represent every descriptive
  // source tag. Harmless tag loss is intentional; known playback gain tags and
  // other playback properties are validated separately before this copy.
  av_dict_copy(&videoStream->metadata, sourceVideo->metadata, 0);
  videoStream->disposition = sourceVideo->disposition;
  av_dict_copy(&output.format->metadata, input.format->metadata, 0);
  if (const auto matrix = av_packet_side_data_get(sourceVideo->codecpar->coded_side_data,
                                                 sourceVideo->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX)) {
    auto side = av_packet_side_data_new(&videoStream->codecpar->coded_side_data,
                                        &videoStream->codecpar->nb_coded_side_data, matrix->type, matrix->size, 0);
    if (!side) throw std::bad_alloc();
    std::memcpy(side->data, matrix->data, matrix->size);
  }
  AVStream *audioStream = nullptr;
  const AVStream *sourceAudio = input.audio >= 0 ? input.format->streams[input.audio] : nullptr;
  if (sourceAudio) {
    audioStream = avformat_new_stream(output.format, nullptr);
    if (!audioStream) throw std::bad_alloc();
    av_dict_copy(&audioStream->metadata, sourceAudio->metadata, 0);
    audioStream->disposition = sourceAudio->disposition;
    if (plan.audio == VideoAudioTreatment::Remux) {
      checked(avcodec_parameters_copy(audioStream->codecpar, sourceAudio->codecpar), "Copy embedded audio");
      audioStream->codecpar->codec_tag = 0; audioStream->time_base = sourceAudio->time_base;
    } else {
      const auto audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
      if (!audioCodec) throw Failure{StorageReason::CodecUnavailable, "AAC is unavailable", FileOutcome::Skipped};
      audioEncoder.value = avcodec_alloc_context3(audioCodec);
      if (!audioEncoder.value) throw std::bad_alloc();
      auto &audio = *audioEncoder.value;
      audio.sample_rate = sourceScan.description.audioSampleRate; audio.time_base = {1, audio.sample_rate};
      audio.sample_fmt = AV_SAMPLE_FMT_FLTP; audio.bit_rate = 128000; audio.thread_count = 1;
      const auto &sourceFormat = *sourceScan.audioFormat;
      if (sourceFormat.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&audio.ch_layout, sourceScan.description.audioChannels);
      else checked(av_channel_layout_copy(&audio.ch_layout, &sourceFormat.ch_layout), "Copy native audio layout");
      if (output.format->oformat->flags & AVFMT_GLOBALHEADER) audio.flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      checked(avcodec_open2(&audio, audioCodec, nullptr), "Open AAC", StorageReason::UnsupportedFormat);
      checked(avcodec_parameters_from_context(audioStream->codecpar, &audio), "Configure MP4 audio");
      audioStream->time_base = audio.time_base;
      checked(swr_alloc_set_opts2(&audioBuffers.resampler, &audio.ch_layout, audio.sample_fmt, audio.sample_rate,
                                  &audio.ch_layout, static_cast<AVSampleFormat>(sourceFormat.format), sourceFormat.sample_rate, 0, nullptr), "Configure native audio conversion");
      checked(swr_init(audioBuffers.resampler), "Initialize native audio conversion");
      audioBuffers.fifo = av_audio_fifo_alloc(audio.sample_fmt, audio.ch_layout.nb_channels, audio.frame_size * 2);
      if (!audioBuffers.fifo) throw std::bad_alloc();
    }
  }
  checked(avformat_write_header(output.format, nullptr), "Write MP4 header", StorageReason::IoError);
  auto compressed = packet();
  // x264 buffers a bounded lookahead. Associate packet durations with its input
  // timestamps so VFR gaps and the final presentation interval survive muxing.
  std::deque<std::pair<std::int64_t, std::int64_t>> durations;
  auto send = [&](AVCodecContext &encoder, AVStream *stream, AVFrame *decoded) {
    checkpoint(check);
    if (&encoder == &video && decoded) durations.emplace_back(decoded->pts, decoded->duration);
    checked(avcodec_send_frame(&encoder, decoded), "Encode media frame");
    int status;
    while ((status = avcodec_receive_packet(&encoder, compressed.get())) >= 0) {
      checkpoint(check);
      if (&encoder == &video) {
        if (durations.empty() || durations.front().first != compressed->pts)
          throw Failure{StorageReason::ValidationFailed, "Encoder changed video presentation timestamps"};
        compressed->duration = durations.front().second; durations.pop_front();
      } else {
        const auto end = normalizedTimestamp(sourceScan.audio.end, sourceScan.audio.timeBase, sourceScan.originMicros, encoder.time_base);
        if (compressed->pts != AV_NOPTS_VALUE && compressed->duration > 0)
          compressed->duration = compressed->pts >= end ? 0 :
              std::min(compressed->duration, timestampSubtract(end, compressed->pts));
      }
      rescalePacketTimestamps(*compressed, encoder.time_base, stream->time_base);
      compressed->stream_index = stream->index;
      checked(av_interleaved_write_frame(output.format, compressed.get()), "Write MP4 packet", StorageReason::IoError);
      av_packet_unref(compressed.get());
    }
    if (status != AVERROR(EAGAIN) && status != AVERROR_EOF) checked(status, "Read encoded packet");
  };
  auto pending = frame(); bool hasPending = false;
  VideoTimestampSelector selector(plan);
  const auto videoOffset = normalizedTimestamp(sourceScan.video.first, sourceScan.video.timeBase, sourceScan.originMicros, video.time_base);
  EncodedSummary summary;
  std::int64_t audioPosition = AV_NOPTS_VALUE, audioNextInput = AV_NOPTS_VALUE;
  auto flushAudio = [&](bool final) {
    if (!audioEncoder.value) return;
    auto &audio = *audioEncoder.value;
    while (av_audio_fifo_size(audioBuffers.fifo) >= audio.frame_size || (final && av_audio_fifo_size(audioBuffers.fifo))) {
      auto samples = frame();
      samples->format = audio.sample_fmt; samples->sample_rate = audio.sample_rate;
      samples->nb_samples = std::min(audio.frame_size, av_audio_fifo_size(audioBuffers.fifo));
      samples->pts = audioPosition; samples->duration = samples->nb_samples;
      checked(av_channel_layout_copy(&samples->ch_layout, &audio.ch_layout), "Prepare audio layout");
      checked(av_frame_get_buffer(samples.get(), 0), "Allocate bounded AAC frame");
      if (av_audio_fifo_read(audioBuffers.fifo, reinterpret_cast<void **>(samples->extended_data), samples->nb_samples) != samples->nb_samples)
        throw Failure{StorageReason::ValidationFailed, "Read native audio samples failed"};
      audioPosition = timestampAdd(audioPosition, samples->nb_samples); send(audio, audioStream, samples.get());
    }
  };
  decode(input, check, [&](int index, const AVFrame &decoded) {
    if (index == input.video) {
      validateVideoFramePresentation(*sourceVideo->codecpar, decoded);
      Scan currentColor;
      observeVideoColor(currentColor, *sourceVideo->codecpar, decoded);
      if (!sameVideoColor(sourceScan, currentColor)) unsupported("Video color metadata changed after scanning");
      auto selected = selector.Select(decoded.best_effort_timestamp);
      if (!selected.valid) unsupported("Invalid video presentation timestamps");
      if (!selected.keep) return;
      if (selected.timestamp == AV_NOPTS_VALUE) unsupported("Selected video timestamp is out of range");
      selected.timestamp = timestampAdd(selected.timestamp, videoOffset);
      if (hasPending) {
        pending->duration = timestampSubtract(selected.timestamp, pending->pts);
        send(video, videoStream, pending.get()); ++summary.frames;
        av_frame_unref(pending.get());
      }
      pending->format = video.pix_fmt; pending->width = video.width; pending->height = video.height;
      pending->pts = selected.timestamp; pending->sample_aspect_ratio = video.sample_aspect_ratio;
      pending->color_range = video.color_range; pending->colorspace = video.colorspace;
      pending->color_primaries = video.color_primaries; pending->color_trc = video.color_trc;
      checked(av_frame_get_buffer(pending.get(), 32), "Allocate bounded video frame");
      scale.value = sws_getCachedContext(scale.value, decoded.width, decoded.height, static_cast<AVPixelFormat>(decoded.format),
                                         video.width, video.height, video.pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
      if (!scale.value) throw Failure{StorageReason::UnsupportedFormat, "Pixel conversion is unavailable", FileOutcome::Skipped};
      const int matrix = video.colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : static_cast<int>(video.colorspace);
      const auto coefficients = sws_getCoefficients(matrix);
      checked(sws_setColorspaceDetails(scale.value, coefficients, video.color_range == AVCOL_RANGE_JPEG,
                                       coefficients, video.color_range == AVCOL_RANGE_JPEG, 0, 1 << 16, 1 << 16), "Preserve video color matrix");
      if (sws_scale(scale.value, decoded.data, decoded.linesize, 0, decoded.height, pending->data, pending->linesize) != video.height)
        throw Failure{StorageReason::ValidationFailed, "Incomplete pixel conversion"};
      hasPending = true;
    } else if (audioEncoder.value) {
      validateAudioFrame(*sourceScan.audioFormat, decoded);
      auto &audio = *audioEncoder.value;
      const auto pts = normalizedTimestamp(decoded.best_effort_timestamp, sourceAudio->time_base, sourceScan.originMicros, audio.time_base);
      if (audioPosition == AV_NOPTS_VALUE) audioPosition = pts;
      advanceAudioInput(pts, decoded.nb_samples, sourceAudio->time_base, audio.time_base, audioNextInput);
      auto converted = frame();
      converted->format = audio.sample_fmt; converted->sample_rate = audio.sample_rate; converted->nb_samples = decoded.nb_samples;
      checked(av_channel_layout_copy(&converted->ch_layout, &audio.ch_layout), "Preserve converted audio layout");
      checked(av_frame_get_buffer(converted.get(), 0), "Allocate bounded audio conversion buffer");
      const int count = swr_convert(audioBuffers.resampler, converted->extended_data, converted->nb_samples,
                                     const_cast<const std::uint8_t **>(decoded.extended_data), decoded.nb_samples);
      checked(count, "Convert native audio sample format");
      if (count != decoded.nb_samples) unsupported("Audio conversion changed sample timing");
      checked(av_audio_fifo_realloc(audioBuffers.fifo, av_audio_fifo_size(audioBuffers.fifo) + count), "Allocate bounded audio FIFO");
      if (av_audio_fifo_write(audioBuffers.fifo, reinterpret_cast<void **>(converted->extended_data), count) != count)
        throw Failure{StorageReason::IoError, "Buffer native audio samples failed"};
      flushAudio(false);
    }
  }, [&](int index, const AVPacket &sourcePacket) {
    if (index != input.audio || plan.audio != VideoAudioTreatment::Remux) return;
    if (sourcePacket.pts == AV_NOPTS_VALUE || sourcePacket.dts == AV_NOPTS_VALUE) unsupported("Embedded audio packet has no timestamp");
    auto copy = packet(); checked(av_packet_ref(copy.get(), &sourcePacket), "Retain embedded audio packet");
    const auto origin = rescaleTimestamp(sourceScan.originMicros, AV_TIME_BASE_Q, sourceAudio->time_base);
    copy->pts = timestampSubtract(copy->pts, origin); copy->dts = timestampSubtract(copy->dts, origin);
    rescalePacketTimestamps(*copy, sourceAudio->time_base, audioStream->time_base); copy->stream_index = audioStream->index;
    checked(av_interleaved_write_frame(output.format, copy.get()), "Remux embedded audio", StorageReason::IoError);
  });
  if (!hasPending) throw Failure{StorageReason::ValidationFailed, "No selected video frames"};
  pending->duration = timestampSubtract(normalizedTimestamp(sourceScan.video.end, sourceScan.video.timeBase,
                                                           sourceScan.originMicros, video.time_base), pending->pts);
  if (pending->duration <= 0) unsupported("Invalid final video duration");
  send(video, videoStream, pending.get()); ++summary.frames;
  send(video, videoStream, nullptr);
  if (!durations.empty()) throw Failure{StorageReason::ValidationFailed, "Encoder did not produce all video frames"};
  flushAudio(true);
  if (audioEncoder.value) send(*audioEncoder.value, audioStream, nullptr);
  checkpoint(check); output.finish();
  return summary;
}

void verify(const Scan &source, const Scan &output, const VideoCompressionPlan &plan, const EncodedSummary &summary) {
  const auto &description = output.description;
  // H.264 omits the VUI full-range flag for its default limited range, so
  // a correctly tagged limited-range encode may decode as unspecified.
  const auto outputRange = description.h264 && output.colorRange == AVCOL_RANGE_UNSPECIFIED ? AVCOL_RANGE_MPEG : output.colorRange;
  auto invalid = [] { throw Failure{StorageReason::ValidationFailed, "MP4 decode-back geometry, content or timeline validation failed"}; };
  // The MP4 muxer enables the sole track of each type, adding DEFAULT even
  // when the source container omitted it. No other disposition may change.
  if (output.videoDisposition != (source.videoDisposition | AV_DISPOSITION_DEFAULT) ||
      (description.hasAudio && output.audioDisposition != (source.audioDisposition | AV_DISPOSITION_DEFAULT)))
    throw Failure{StorageReason::ValidationFailed, "MP4 decode-back stream disposition validation failed"};
  if (!description.h264 || description.width != plan.width || description.height != plan.height ||
      av_cmp_q(description.sampleAspectRatio, plan.sampleAspectRatio) || description.rotation != plan.rotation ||
      description.alpha || description.hdr || description.interlaced || description.unsupportedContent ||
      description.hasAudio != source.description.hasAudio || output.video.frames != summary.frames ||
      (source.colorRange != AVCOL_RANGE_UNSPECIFIED && source.colorRange != outputRange) ||
      (source.colorSpace != AVCOL_SPC_UNSPECIFIED && source.colorSpace != output.colorSpace) ||
      (source.colorPrimaries != AVCOL_PRI_UNSPECIFIED && source.colorPrimaries != output.colorPrimaries) ||
      (source.colorTransfer != AVCOL_TRC_UNSPECIFIED && source.colorTransfer != output.colorTransfer)) invalid();
  const auto videoTolerance = timestampAdd(std::max(rescaleTimestamp(source.video.maximumDuration, source.video.timeBase, AV_TIME_BASE_Q),
                                       rescaleTimestamp(1, av_inv_q(plan.frameRate), AV_TIME_BASE_Q)), 1);
  const auto expectedStart = normalizedTimestamp(source.video.first, source.video.timeBase, source.originMicros, AV_TIME_BASE_Q);
  const auto expectedEnd = normalizedTimestamp(source.video.end, source.video.timeBase, source.originMicros, AV_TIME_BASE_Q);
  if (timestampDistance(rescaleTimestamp(output.video.first, output.video.timeBase, AV_TIME_BASE_Q), expectedStart) > 2 ||
      timestampDistance(rescaleTimestamp(output.video.end, output.video.timeBase, AV_TIME_BASE_Q), expectedEnd) > videoTolerance) invalid();
  // Unreduced CFR retains its source-clock rounding; the encoder has already
  // verified every presentation timestamp. Enforce the exact grid on reductions.
  if (plan.reduceFps && output.video.minimumInterval > 0 &&
      av_compare_ts(output.video.minimumInterval, output.video.timeBase, 1, {1, plan.maxFps}) < 0) invalid();
  if (description.hasAudio) {
    if (description.audioChannels != source.description.audioChannels || description.audioSampleRate != source.description.audioSampleRate) invalid();
    const auto audioTolerance = timestampAdd(std::max(rescaleTimestamp(source.audio.maximumDuration, source.audio.timeBase, AV_TIME_BASE_Q),
                                         rescaleTimestamp(output.audio.maximumDuration, output.audio.timeBase, AV_TIME_BASE_Q)), 2);
    const auto start = normalizedTimestamp(source.audio.first, source.audio.timeBase, source.originMicros, AV_TIME_BASE_Q);
    const auto end = normalizedTimestamp(source.audio.end, source.audio.timeBase, source.originMicros, AV_TIME_BASE_Q);
    if (timestampDistance(rescaleTimestamp(output.audio.first, output.audio.timeBase, AV_TIME_BASE_Q), start) > audioTolerance ||
        timestampDistance(rescaleTimestamp(output.audio.end, output.audio.timeBase, AV_TIME_BASE_Q), end) > audioTolerance) invalid();
  }
}
}

StorageDiagnostic ValidateExistingBga(const fs::path &path, Checkpoint check) {
  try {
    scan(path, check, ScanPurpose::ExistingDestination);
    return {};
  } catch (const Failure &failure) {
    return {failure.reason, failure.detail};
  } catch (const std::exception &error) {
    return {StorageReason::IoError, error.what()};
  }
}

EncodeResult EncodeBga(const AssetCandidate &candidate, const StoragePolicy &policy,
                       const fs::path &temporaryOutput, Checkpoint check, ProgressCallback progress) {
  bool created = false;
  auto discard = [&] { if (created) { std::error_code ignored; fs::remove(temporaryOutput, ignored); } };
  try {
    checkpoint(check);
    if (policy.bga != BgaAction::CompressH264) return {FileOutcome::Skipped, {}, {StorageReason::PolicyKeep, "BGA compression is not selected"}};
    if (asobmshow::chart_assets::ClassifyMediaPath(candidate.source) != asobmshow::chart_assets::MediaKind::Video)
      return {FileOutcome::Skipped, {}, {StorageReason::UnsupportedSource, "Only supported video paths are eligible"}};
    const auto source = scan(candidate.source, check);
    const auto plan = PlanVideoCompression(source.description, policy);
    if (plan.reason.code != StorageReason::None) return {FileOutcome::Skipped, {}, plan.reason};
    if (progress) progress({StoragePhase::Encode, candidate.source});
    const auto summary = encode(candidate.source, temporaryOutput, source, plan, check, created);
    if (progress) progress({StoragePhase::Verify, candidate.source});
    try { verify(source, scan(temporaryOutput, check, ScanPurpose::EncodedOutput), plan, summary); }
    catch (const Failure &failure) {
      if (failure.reason == StorageReason::Cancelled) throw;
      throw Failure{StorageReason::ValidationFailed, failure.detail};
    }
    checkpoint(check);
    std::error_code error;
    const auto before = fs::file_size(candidate.source, error);
    if (error) throw Failure{StorageReason::IoError, "Cannot measure source video"};
    const auto after = fs::file_size(temporaryOutput, error);
    if (error) throw Failure{StorageReason::IoError, "Cannot measure encoded video"};
    if (after >= before) {
      discard();
      return {FileOutcome::Skipped, {}, {StorageReason::NotSmaller, "Encoded video is not smaller; requested caps were not applied"}};
    }
    return {FileOutcome::Converted, temporaryOutput, {}};
  } catch (const Failure &failure) {
    discard(); return {failure.outcome, {}, {failure.reason, failure.detail}};
  } catch (const std::exception &error) {
    discard(); return {FileOutcome::Failed, {}, {StorageReason::IoError, error.what()}};
  }
}
} // namespace storage_management
