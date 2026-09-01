#include "music_select/MusicSelectLaunchPolicy.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testBuiltInOnlyForAnAbsentSelection() {
  skin::GameplaySkinAcquisition acquisition;
  const auto decision = decideMusicSelectLaunch(std::move(acquisition));
  require(decision.kind == MusicSelectLaunchKind::BuiltIn &&
              !decision.request && decision.diagnostics.empty(),
          "an absent type-5 selection opens the native selector");
}

void testSelectedSkinNeverFallsBackAfterFailure() {
  skin::GameplaySkinAcquisition acquisition{
      .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
      .failure = skin::GameplaySkinAcquisitionFailure{
          .diagnostic = skin::SkinDiagnostic{
              .code = "skin.test.failure",
              .message = "configured type-5 skin failed"}}};
  const auto decision = decideMusicSelectLaunch(std::move(acquisition));
  require(decision.kind == MusicSelectLaunchKind::Error &&
              !decision.request && decision.diagnostics.size() == 1 &&
              decision.diagnostics.front().code == "skin.test.failure",
          "a failed selected skin produces its diagnostic error route");
}

void testReadyWithoutAnActivationIsAnError() {
  skin::GameplaySkinAcquisition acquisition{
      .disposition = skin::GameplaySkinAcquisitionDisposition::Ready};
  const auto decision = decideMusicSelectLaunch(std::move(acquisition));
  require(decision.kind == MusicSelectLaunchKind::Error && !decision.request,
          "a ready disposition without its activation cannot become built-in");
}

} // namespace

int main() {
  testBuiltInOnlyForAnAbsentSelection();
  testSelectedSkinNeverFallsBackAfterFailure();
  testReadyWithoutAnActivationIsAnError();
  if (failures != 0) return 1;
  std::cout << "music-select launch policy tests passed\n";
  return 0;
}
