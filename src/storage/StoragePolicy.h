#pragma once

namespace storage_management {

enum class AudioAction { Keep, LosslessFlac, LossyVorbis };
enum class BgaAction { Keep, CompressH264, Delete };
enum class VideoQuality { High, Balanced, Smaller };
enum class ResolutionCap { Original, FullHd, Hd, Sd, Low };

struct StoragePolicy {
  AudioAction audio = AudioAction::Keep;
  BgaAction bga = BgaAction::Keep;
  VideoQuality videoQuality = VideoQuality::Balanced;
  ResolutionCap resolution = ResolutionCap::Original;
  int maxFps = 0;
  bool operator==(const StoragePolicy &) const = default;
};

struct StoragePreferences {
  StoragePolicy manual;
  StoragePolicy download;
  bool afterDownload = false;
  bool operator==(const StoragePreferences &) const = default;
};

bool IsValid(const StoragePolicy &policy);
bool IsNoOp(const StoragePolicy &policy);
StoragePolicy EffectiveDownloadPolicy(const StoragePreferences &preferences);
StoragePreferences Sanitize(StoragePreferences preferences);
int VideoCrf(VideoQuality quality);

} // namespace storage_management
