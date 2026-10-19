#pragma once

#include "StoragePolicy.h"

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace storage_management {
struct StorageVideoEncoderCandidate {
  std::string name, deviceName;
  bool hardware = false;
  AVPixelFormat pixelFormat = AV_PIX_FMT_YUV420P;
};

AVCodecID StorageVideoCodecId(VideoCodec codec);
int StorageVideoEncoderBitrate(VideoCodec codec, VideoQuality quality,
                               int width, int height, int fps);
std::vector<StorageVideoEncoderCandidate> StorageVideoEncoderCandidates(
    VideoCodec codec, int width, int height, int fps, int bitrate);
// The caller owns a fresh context allocated for candidate.name, sets geometry,
// timing, color, SAR and global-header flags, and frees it after any failure.
bool ConfigureStorageVideoEncoder(AVCodecContext *context,
                                  const StorageVideoEncoderCandidate &candidate,
                                  const StoragePolicy &policy, std::string &reason);
} // namespace storage_management
