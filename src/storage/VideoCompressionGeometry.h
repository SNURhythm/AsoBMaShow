#pragma once

#include "StorageTypes.h"
extern "C" {
#include <libavutil/rational.h>
}

namespace storage_management {
enum class VideoAudioTreatment { None, Remux, Aac };
struct VideoDescription {
  int width = 0, height = 0;
  AVRational sampleAspectRatio{1, 1};
  int rotation = 0;
  AVRational timeBase{0, 1}, frameRate{0, 1};
  std::int64_t duration = 0;
  std::int64_t minimumFrameInterval = 0;
  bool variableFrameRate = false;
  bool h264 = false, alpha = false, hdr = false, interlaced = false;
  bool unsupportedContent = false;
  bool hasAudio = false, mp4CompatibleAudio = false;
  int audioChannels = 0, audioSampleRate = 0;
  bool audioLayoutSupported = true;
  bool audioDiscontinuous = false;
  StorageCodecCapabilities codecs;
};
struct VideoCompressionPlan {
  VideoCodec codec = VideoCodec::H264;
  int width = 0, height = 0;
  AVRational sampleAspectRatio{1, 1};
  int rotation = 0, crf = 23;
  AVRational inputTimeBase{0, 1}, timeBase{0, 1}, frameRate{0, 1};
  int maxFps = 0;
  bool reduceFps = false;
  VideoAudioTreatment audio = VideoAudioTreatment::None;
  StorageDiagnostic reason;
};
VideoCompressionPlan PlanVideoCompression(const VideoDescription &, const StoragePolicy &);

struct VideoTimestampSelection {
  bool valid = false, keep = false;
  std::int64_t timestamp = 0;
};
// Bounded state: one grid bucket and the previous presentation timestamp.
class VideoTimestampSelector {
public:
  explicit VideoTimestampSelector(VideoCompressionPlan plan) : plan_(std::move(plan)) {}
  VideoTimestampSelection Select(std::int64_t presentationTimestamp);
private:
  VideoCompressionPlan plan_;
  bool started_ = false;
  std::int64_t first_ = 0, previous_ = 0, bucket_ = -1;
};
} // namespace storage_management
