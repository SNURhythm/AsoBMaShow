#include "../src/storage/StoragePolicy.h"
#include "../src/storage/VideoCompressionGeometry.h"

#include <iostream>
#include <array>
#include <cmath>

using namespace storage_management;

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}

int main() {
  StoragePreferences preferences;
  expect(IsNoOp(preferences.manual) && IsNoOp(preferences.download) &&
             !preferences.afterDownload,
         "new preferences never mutate media");
  expect(preferences.manual.videoQuality == VideoQuality::Balanced &&
             preferences.manual.resolution == ResolutionCap::Original &&
             preferences.manual.maxFps == 0,
         "default compression preserves geometry and frame timing");
  preferences.download.audio = AudioAction::LosslessFlac;
  expect(IsNoOp(EffectiveDownloadPolicy(preferences)),
         "saved policy stays inactive until automation is enabled");
  preferences.afterDownload = true;
  expect(EffectiveDownloadPolicy(preferences).audio == AudioAction::LosslessFlac,
         "enabled automation uses the download policy");
  preferences.manual.bga = BgaAction::Delete;
  expect(EffectiveDownloadPolicy(preferences).bga == BgaAction::Keep,
         "manual destructive choice never affects downloads");

  for (int field = 0; field < 5; ++field) {
    auto invalid = preferences;
    switch (field) {
    case 0: invalid.download.audio = static_cast<AudioAction>(-1); break;
    case 1: invalid.download.bga = static_cast<BgaAction>(99); break;
    case 2: invalid.download.videoQuality = static_cast<VideoQuality>(99); break;
    case 3: invalid.download.resolution = static_cast<ResolutionCap>(99); break;
    case 4: invalid.download.maxFps = 29; break;
    }
    auto safe = Sanitize(invalid);
    expect(!safe.afterDownload && IsNoOp(safe.download),
           "invalid policy resets all its operations and disables automation");
    expect(safe.manual == preferences.manual,
           "a malformed download policy does not rewrite the valid manual policy");
    expect(IsNoOp(EffectiveDownloadPolicy(invalid)),
           "effective policy cannot bypass invalid-value protection");
  }
  auto invalidManual = preferences;
  invalidManual.manual.maxFps = -1;
  auto safe = Sanitize(invalidManual);
  expect(IsNoOp(safe.manual) && !safe.afterDownload &&
             safe.download == preferences.download,
         "invalid manual policy also disables automatic processing");
  for (int cap : {0, 15, 24, 30, 60}) {
    preferences.download.maxFps = cap;
    expect(Sanitize(preferences) == preferences,
           "supported FPS caps survive sanitation");
  }
  expect(VideoCrf(VideoQuality::High) == 18 &&
             VideoCrf(VideoQuality::Balanced) == 23 &&
             VideoCrf(VideoQuality::Smaller) == 28,
         "quality maps to the specified encoder CRF");
  StoragePolicy videoPolicy;
  videoPolicy.bga = BgaAction::CompressH264;
  videoPolicy.resolution = ResolutionCap::Hd;
  VideoDescription video;
  video.width = 1920; video.height = 1080;
  video.sampleAspectRatio = {1, 1}; video.timeBase = {1, 90000};
  video.frameRate = {30000, 1001}; video.duration = 90000;
  video.codecs.flac.available = video.codecs.vorbis.available = true;
  video.codecs.x264.available = video.codecs.aac.available = video.codecs.mp4.available = true;
  for (const auto &size : {std::array{1920, 1080, 1280, 720},
                           std::array{1024, 1024, 720, 720},
                           std::array{1080, 1920, 720, 1280},
                           std::array{320, 240, 320, 240}}) {
    video.width = size[0]; video.height = size[1];
    const auto plan = PlanVideoCompression(video, videoPolicy);
    expect(plan.reason.code == StorageReason::None && plan.width == size[2] && plan.height == size[3],
           "orientation-independent caps scale once without upscaling");
  }
  video.width = 1920; video.height = 1080;
  for (const auto &entry : {std::array{ResolutionCap::Original, ResolutionCap::FullHd},
                            std::array{ResolutionCap::Hd, ResolutionCap::Sd}}) {
    for (const auto cap : entry) {
      videoPolicy.resolution = cap;
      auto plan = PlanVideoCompression(video, videoPolicy);
      const int expectedWidth = cap == ResolutionCap::Hd ? 1280 : cap == ResolutionCap::Sd ? 852 : 1920;
      expect(plan.width == expectedWidth, "every resolution preset uses its declared bounding box and even alignment");
    }
  }
  videoPolicy.resolution = ResolutionCap::Low;
  expect(PlanVideoCompression(video, videoPolicy).width == 640, "low cap is 640 by 360");
  video.width = 721; video.height = 481; video.sampleAspectRatio = {8, 9}; video.rotation = 90;
  videoPolicy.resolution = ResolutionCap::Original;
  auto aligned = PlanVideoCompression(video, videoPolicy);
  expect(aligned.width == 720 && aligned.height == 480 && aligned.rotation == 90,
         "odd geometry shrinks at most one pixel and keeps rotation");
  expect(std::abs((double(aligned.width) * aligned.sampleAspectRatio.num /
                   aligned.sampleAspectRatio.den / aligned.height) - (721.0 * 8 / 9 / 481)) < 0.000001,
         "sample aspect ratio compensates encoder alignment");
  video.width = 320; video.height = 240; video.sampleAspectRatio = {1, 1}; video.rotation = 0;
  videoPolicy.maxFps = 30;
  auto under = PlanVideoCompression(video, videoPolicy);
  expect(!under.reduceFps && under.frameRate.num == 30000 && under.frameRate.den == 1001,
         "30000/1001 stays rational below a 30 FPS cap");
  video.frameRate = {60000, 1001}; video.timeBase = {1, 60000};
  auto capped = PlanVideoCompression(video, videoPolicy);
  VideoTimestampSelector selector(capped);
  int retained = 0;
  std::int64_t previous = -1;
  for (int i = 0; i < 60; ++i) {
    auto selected = selector.Select(i * 1001);
    expect(selected.valid, "valid monotonically increasing rational timestamps accepted");
    if (selected.keep) {
      if (previous >= 0) expect(selected.timestamp - previous >= 2000, "adjacent retained timestamps obey actual cap");
      previous = selected.timestamp; ++retained;
    }
  }
  expect(retained == 30 && previous == 58000, "60 fractional-rate frames retain 30 grid frames without speeding playback");
  expect(!selector.Select(100).valid, "backward timestamps never guess a playback speed");
  VideoTimestampSelector gaps(capped);
  expect(gaps.Select(0).keep && gaps.Select(12000).timestamp == 12000,
         "time-grid selection preserves VFR gaps without duplicate fill frames");
  video.h264 = true; video.frameRate = {30000, 1001};
  expect(PlanVideoCompression(video, videoPolicy).reason.code == StorageReason::AlreadyOptimized,
         "H264 under caps is not repeatedly reencoded");
  video.h264 = false;
  for (int flag = 0; flag < 3; ++flag) {
    auto unsupported = video;
    if (flag == 0) unsupported.alpha = true;
    if (flag == 1) unsupported.hdr = true;
    if (flag == 2) unsupported.interlaced = true;
    expect(PlanVideoCompression(unsupported, videoPolicy).reason.code == StorageReason::UnsupportedFormat,
           "alpha HDR and interlaced content are retained");
  }
  auto unavailableVideo = video; unavailableVideo.codecs.x264.available = false;
  expect(PlanVideoCompression(unavailableVideo, videoPolicy).reason.code == StorageReason::CodecUnavailable,
         "unavailable x264 does not select another encoder");
  unavailableVideo = video; unavailableVideo.hasAudio = true; unavailableVideo.audioChannels = 2;
  unavailableVideo.audioSampleRate = 48000; unavailableVideo.codecs.aac.available = false;
  expect(PlanVideoCompression(unavailableVideo, videoPolicy).reason.code == StorageReason::CodecUnavailable,
         "unavailable AAC never drops embedded audio");
  return failures ? 1 : 0;
}
