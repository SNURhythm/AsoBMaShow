#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace replay_video_export {

class ReplayPixelConverter {
public:
  ReplayPixelConverter() = default;
  ~ReplayPixelConverter() { reset(); }
  ReplayPixelConverter(const ReplayPixelConverter &) = delete;
  ReplayPixelConverter &operator=(const ReplayPixelConverter &) = delete;

  int initialize(const AVFrame *destination, int threads) {
    reset();
    if (destination == nullptr || destination->width <= 0 ||
        destination->width > INT_MAX / 4 || destination->height <= 0) {
      return AVERROR(EINVAL);
    }
    context_ = sws_alloc_context();
    source_ = av_frame_alloc();
    if (context_ == nullptr || source_ == nullptr) return AVERROR(ENOMEM);
    context_->flags = SWS_FAST_BILINEAR;
    context_->threads = threads;
    source_->format = AV_PIX_FMT_BGRA;
    source_->width = destination->width;
    source_->height = destination->height;
    source_->linesize[0] = destination->width * 4;
    // Use the frame API's dynamic context. sws_scale() only runs slice zero,
    // even when threads > 1. Legacy frame initialization also copies unowned
    // input buffers; the dynamic API can borrow our completed readback directly.
    return sws_frame_setup(context_, destination, source_);
  }

  int convert(const uint8_t *bgra, AVFrame *destination) {
    if (source_ == nullptr || context_ == nullptr || bgra == nullptr ||
        destination == nullptr) return AVERROR(EINVAL);
    source_->data[0] = const_cast<uint8_t *>(bgra);
    const int result = sws_scale_frame(context_, destination, source_);
    // Conversion is synchronous; never retain the caller's reusable readback.
    source_->data[0] = nullptr;
    return result;
  }

  void reset() {
    sws_freeContext(context_);
    context_ = nullptr;
    av_frame_free(&source_);
  }

private:
  SwsContext *context_ = nullptr;
  AVFrame *source_ = nullptr;
};

} // namespace replay_video_export
