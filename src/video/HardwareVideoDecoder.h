#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace video {

// Decoder negotiation is shared by playback and the capability probe. Native
// frame import stays behind HardwareVideoFrame's platform boundary.
inline AVPixelFormat selectHardwareVideoFormat(AVCodecContext *context,
                                               const AVPixelFormat *formats) {
#if defined(__APPLE__)
  const auto *description = av_pix_fmt_desc_get(context->sw_pix_fmt);
  const bool supported = description && description->nb_components == 3 &&
      description->comp[0].depth == 8 && description->log2_chroma_w == 1 &&
      description->log2_chroma_h == 1 &&
      context->color_trc != AVCOL_TRC_SMPTE2084 &&
      context->color_trc != AVCOL_TRC_ARIB_STD_B67;
  if (supported) {
    for (const auto *format = formats; *format != AV_PIX_FMT_NONE; ++format)
      if (*format == AV_PIX_FMT_VIDEOTOOLBOX) return *format;
  }
#endif
  for (const auto *format = formats; *format != AV_PIX_FMT_NONE; ++format) {
    const auto *description = av_pix_fmt_desc_get(*format);
    if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *format;
  }
  return AV_PIX_FMT_NONE;
}

inline bool configureHardwareVideoDecoder(AVCodecContext *context) {
#if defined(__APPLE__)
  if (context->codec_id != AV_CODEC_ID_H264 && context->codec_id != AV_CODEC_ID_HEVC)
    return false;
  AVBufferRef *device = nullptr;
  if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0)
    return false;
  context->hw_device_ctx = device;
  context->get_format = selectHardwareVideoFormat;
  // Surface counts stay bounded by the decoder and the player's small queue.
  context->thread_count = 1;
  return true;
#else
  return false;
#endif
}

} // namespace video
