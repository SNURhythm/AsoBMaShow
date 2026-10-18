#include "VideoCompressionGeometry.h"
extern "C" {
#include <libavutil/mathematics.h>
}
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace storage_management {
VideoCompressionPlan PlanVideoCompression(const VideoDescription &source, const StoragePolicy &policy) {
  VideoCompressionPlan result;
  auto skip = [&](StorageReason code, const char *detail) {
    result.reason = {code, detail}; return result;
  };
  if (!IsValid(policy) || policy.bga != BgaAction::CompressH264)
    return skip(StorageReason::PolicyKeep, "BGA compression is not selected");
  if (source.alpha || source.hdr || source.interlaced || source.unsupportedContent)
    return skip(StorageReason::UnsupportedFormat, "Video contains content or metadata that cannot be preserved");
  if (source.width < 2 || source.height < 2 || source.width > 16384 || source.height > 16384 ||
      source.sampleAspectRatio.num <= 0 || source.sampleAspectRatio.den <= 0 ||
      source.timeBase.num <= 0 || source.timeBase.den <= 0 ||
      source.frameRate.num <= 0 || source.frameRate.den <= 0 ||
      av_cmp_q(source.frameRate, {std::numeric_limits<int>::max() / 2, 1}) > 0 ||
      source.duration <= 0 || source.rotation % 90)
    return skip(StorageReason::UnsupportedFormat, "Unknown video geometry, duration or timestamps");
  int longCap = 0, shortCap = 0;
  switch (policy.resolution) {
  case ResolutionCap::Original: break;
  case ResolutionCap::FullHd: longCap = 1920; shortCap = 1080; break;
  case ResolutionCap::Hd: longCap = 1280; shortCap = 720; break;
  case ResolutionCap::Sd: longCap = 854; shortCap = 480; break;
  case ResolutionCap::Low: longCap = 640; shortCap = 360; break;
  }
  const double displayWidth = source.width * av_q2d(source.sampleAspectRatio);
  const double displayHeight = source.height;
  const double scale = longCap ? std::min({1.0, longCap / std::max(displayWidth, displayHeight),
                                          shortCap / std::min(displayWidth, displayHeight)}) : 1.0;
  result.reduceFps = policy.maxFps > 0 && (!source.variableFrameRate ?
      av_cmp_q(source.frameRate, {policy.maxFps, 1}) > 0 :
      (source.minimumFrameInterval <= 0 ||
       av_compare_ts(source.minimumFrameInterval, source.timeBase, 1, {1, policy.maxFps}) < 0));
  if (source.h264 && scale >= 1.0 && !result.reduceFps)
    return skip(StorageReason::AlreadyOptimized, "H.264 already satisfies the requested caps");
  if (!source.codecs.x264.available || !source.codecs.mp4.available)
    return skip(StorageReason::CodecUnavailable, "libx264 or MP4 is unavailable in this build");
  result.width = static_cast<int>(std::floor(source.width * scale)) & ~1;
  result.height = static_cast<int>(std::floor(source.height * scale)) & ~1;
  if (result.width < 2 || result.height < 2)
    return skip(StorageReason::UnsupportedFormat, "Display geometry is too narrow for 4:2:0 output");
  av_reduce(&result.sampleAspectRatio.num, &result.sampleAspectRatio.den,
            std::int64_t(source.width) * source.sampleAspectRatio.num * result.height,
            std::int64_t(source.height) * source.sampleAspectRatio.den * result.width,
            std::numeric_limits<int>::max());
  result.rotation = source.rotation;
  result.inputTimeBase = source.timeBase;
  result.timeBase = source.timeBase;
  result.frameRate = result.reduceFps ? AVRational{policy.maxFps, 1} : source.frameRate;
  result.maxFps = policy.maxFps;
  result.crf = VideoCrf(policy.videoQuality);
  if (result.reduceFps) {
    const auto denominator = std::int64_t(source.timeBase.den / std::gcd(source.timeBase.den, policy.maxFps)) * policy.maxFps;
    if (denominator > std::numeric_limits<int>::max())
      return skip(StorageReason::UnsupportedFormat, "Time base cannot represent the requested FPS grid exactly");
    result.timeBase = {1, static_cast<int>(denominator)};
  }
  if (source.hasAudio) {
    if (source.audioChannels < 1 || source.audioChannels > 2 || source.audioSampleRate <= 0 || !source.audioLayoutSupported)
      return skip(StorageReason::UnsupportedFormat, "Embedded audio layout cannot be preserved");
    result.audio = source.mp4CompatibleAudio ? VideoAudioTreatment::Remux : VideoAudioTreatment::Aac;
    if (result.audio == VideoAudioTreatment::Aac && source.audioDiscontinuous)
      return skip(StorageReason::UnsupportedFormat, "Discontinuous embedded audio cannot be transcoded faithfully");
    if (result.audio == VideoAudioTreatment::Aac && !source.codecs.aac.available)
      return skip(StorageReason::CodecUnavailable, "AAC is unavailable; embedded audio must be retained");
  }
  return result;
}

VideoTimestampSelection VideoTimestampSelector::Select(std::int64_t pts) {
  if (plan_.reason.code != StorageReason::None || pts == std::numeric_limits<std::int64_t>::min() ||
      plan_.inputTimeBase.num <= 0 || plan_.timeBase.num <= 0 ||
      (started_ && pts <= previous_)) return {};
  if (!started_) { first_ = pts; started_ = true; }
  if (pts < first_ || (first_ < 0 && pts > std::numeric_limits<std::int64_t>::max() + first_)) return {};
  previous_ = pts;
  if (!plan_.reduceFps) return {true, true, av_rescale_q(pts - first_, plan_.inputTimeBase, plan_.timeBase)};
  const auto bucket = av_rescale_q_rnd(pts - first_, plan_.inputTimeBase, {1, plan_.maxFps}, AV_ROUND_DOWN);
  if (bucket <= bucket_) return {true, false, 0};
  bucket_ = bucket;
  return {true, true, av_rescale_q(bucket, {1, plan_.maxFps}, plan_.timeBase)};
}
} // namespace storage_management
