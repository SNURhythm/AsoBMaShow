#pragma once

#include "StoragePolicy.h"
#include <array>
#include <functional>
#include <future>
#include <string>

namespace storage_management {
struct StorageVideoCodecSupport {
  VideoCodec codec = VideoCodec::H264;
  bool available = false;
  bool hardware = false;
  std::string encoder, reason;
  bool playbackAvailable = false;
  bool playbackHardware = false;
  std::string decoder;
};
using StorageVideoCapabilities = std::array<StorageVideoCodecSupport, 3>;

// The process-wide probe owns its worker independently of any settings view.
// Wake never receives a view pointer; results are ready before it is called.
std::shared_future<StorageVideoCapabilities> RequestStorageVideoCapabilities(std::function<void()> wake = {});
StorageVideoCapabilities ProbeStorageVideoCapabilities();
} // namespace storage_management
