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
          .entry = skin::SkinEntryId{
              .package = {.directoryName = "ModernChic"},
              .packageRelativePath = "musicselect.luaskin"},
          .diagnostic = skin::SkinDiagnostic{
              .code = "skin.test.failure",
              .message = "configured type-5 skin failed"}}};
  const auto decision = decideMusicSelectLaunch(std::move(acquisition));
  require(decision.kind == MusicSelectLaunchKind::Error &&
              !decision.request && decision.diagnostics.size() == 1 &&
              decision.diagnostics.front().code == "skin.test.failure" &&
              decision.selectedSkinPath ==
                  "ModernChic/musicselect.luaskin",
          "a failed selected skin produces its diagnostic error route");
}

void testReadyWithoutAnActivationIsAnError() {
  skin::GameplaySkinAcquisition acquisition{
      .disposition = skin::GameplaySkinAcquisitionDisposition::Ready};
  const auto decision = decideMusicSelectLaunch(std::move(acquisition));
  require(decision.kind == MusicSelectLaunchKind::Error && !decision.request &&
              decision.diagnostics.size() == 1 &&
              decision.diagnostics.front().code ==
                  "skin.music_select.acquisition_incomplete",
          "an incomplete ready acquisition has a diagnostic and cannot "
          "become built-in");
}

void testFailureReasonPreservesAvailableSourceLocation() {
  const auto reason = musicSelectSkinFailureReason(
      1, {.code = "skin.test.lua",
          .message = "callback failed",
          .virtualPath = "fallback/path.lua",
          .source = skin::SkinSourceLocation{
              .virtualPath = "select/main.lua", .line = 37, .column = 9}});
  require(reason ==
              "2. skin.test.lua: callback failed (select/main.lua:37:9)",
          "failure reasons preserve ordering and the complete Lua location");
}

} // namespace

int main() {
  testBuiltInOnlyForAnAbsentSelection();
  testSelectedSkinNeverFallsBackAfterFailure();
  testReadyWithoutAnActivationIsAnError();
  testFailureReasonPreservesAvailableSourceLocation();
  if (failures != 0) return 1;
  std::cout << "music-select launch policy tests passed\n";
  return 0;
}
