#include "StoragePolicy.h"

#include <initializer_list>

namespace storage_management {

bool IsValid(const StoragePolicy &policy) {
  return policy.audio >= AudioAction::Keep && policy.audio <= AudioAction::LossyVorbis &&
         policy.bga >= BgaAction::Keep && policy.bga <= BgaAction::Delete &&
         policy.videoQuality >= VideoQuality::High && policy.videoQuality <= VideoQuality::Smaller &&
         policy.resolution >= ResolutionCap::Original && policy.resolution <= ResolutionCap::Low &&
         (policy.maxFps == 0 || policy.maxFps == 15 || policy.maxFps == 24 ||
          policy.maxFps == 30 || policy.maxFps == 60);
}

bool IsNoOp(const StoragePolicy &policy) {
  return policy.audio == AudioAction::Keep && policy.bga == BgaAction::Keep;
}

StoragePreferences Sanitize(StoragePreferences preferences) {
  for (auto *policy : {&preferences.manual, &preferences.download}) {
    if (!IsValid(*policy)) {
      *policy = {};
      preferences.afterDownload = false;
    }
  }
  return preferences;
}

StoragePolicy EffectiveDownloadPolicy(const StoragePreferences &preferences) {
  const auto safe = Sanitize(preferences);
  return safe.afterDownload ? safe.download : StoragePolicy{};
}

int VideoCrf(VideoQuality quality) {
  switch (quality) {
  case VideoQuality::High: return 18;
  case VideoQuality::Smaller: return 28;
  default: return 23;
  }
}

} // namespace storage_management
