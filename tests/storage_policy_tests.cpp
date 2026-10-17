#include "../src/storage/StoragePolicy.h"

#include <iostream>

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
  return failures ? 1 : 0;
}
