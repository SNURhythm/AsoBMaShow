#include "replay/ReplayCapabilities.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

using replay::RecordOrigin;
using replay::ReplayCapabilities;
using replay::ReplayCapabilityInput;
using replay::ReplayState;

ReplayCapabilities modernRecordOnly(bool irEligible) {
  return {
      .recordsList = true,
      .viewResult = true,
      .irUpload = irEligible,
      .profileDuplicateRecord = true,
      .profileArchiveRecord = true,
  };
}

void testVerifiedModernMatrix() {
  ReplayCapabilities chart = modernRecordOnly(true);
  chart.watch = true;
  chart.retrySame = true;
  chart.gBattle = true;
  chart.practiceGhost = true;
  chart.videoExport = true;
  chart.shareOrCopy = true;
  chart.deleteReplayFile = true;
  chart.profileDuplicateReplay = true;
  chart.profileArchiveReplay = true;

  ReplayCapabilities course = chart;
  course.gBattle = false;
  course.practiceGhost = false;

  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernChartResult,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == chart,
         "verified modern chart exposes chart replay actions");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernCourseResult,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == course,
         "verified modern course excludes chart-only actions");
}

void testAbsentReplayKeepsModernResultAndIr() {
  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::UserDeleted,
      ReplayState::Missing,
  };
  for (ReplayState state : states) {
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ModernChartResult,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == modernRecordOnly(true),
           "absent replay changes no modern result or IR capability");
  }
}

void testInvalidReplayIsOnlyDeletable() {
  constexpr std::array states{
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    ReplayCapabilities expected = modernRecordOnly(false);
    expected.deleteReplayFile = true;
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ModernChartResult,
               .replayState = state,
           }) == expected,
           "existing invalid replay is deletable but not playable");
  }
}

void testIrEligibilityNeverComesFromReplayState() {
  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::Verified,
      ReplayState::UserDeleted,
      ReplayState::Missing,
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    const auto eligible = replay::capabilitiesFor({
        .origin = RecordOrigin::ModernCourseResult,
        .replayState = state,
        .postponedIrSnapshotEligible = true,
    });
    const auto ineligible = replay::capabilitiesFor({
        .origin = RecordOrigin::ModernCourseResult,
        .replayState = state,
        .postponedIrSnapshotEligible = false,
    });
    expect(eligible.irUpload && !ineligible.irUpload,
           "IR capability is controlled only by the saved snapshot");
  }
}

void testLegacyAndRemoteRecordsIgnoreReplayState() {
  ReplayCapabilities legacy;
  legacy.recordsList = true;
  legacy.profileDuplicateRecord = true;
  legacy.profileArchiveRecord = true;

  ReplayCapabilities remote = legacy;
  remote.viewResult = true;

  constexpr std::array states{
      ReplayState::NotApplicable,
      ReplayState::Verified,
      ReplayState::UserDeleted,
      ReplayState::Missing,
      ReplayState::Corrupt,
      ReplayState::Mismatched,
      ReplayState::UnsupportedExtension,
  };
  for (ReplayState state : states) {
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::LegacyChartSummary,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == legacy,
           "legacy chart is summary-only for every replay state");
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::LegacyCourseSummary,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == legacy,
           "legacy course is summary-only for every replay state");
    expect(replay::capabilitiesFor({
               .origin = RecordOrigin::ImportedRemoteResult,
               .replayState = state,
               .postponedIrSnapshotEligible = true,
           }) == remote,
           "remote result has detail but no local replay or upload actions");
  }
}

void testImportedStockReplaySurface() {
  ReplayCapabilities verified;
  verified.watch = true;
  verified.shareOrCopy = true;
  verified.deleteReplayFile = true;
  verified.profileDuplicateReplay = true;
  verified.profileArchiveReplay = true;
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == verified,
         "verified stock BRD is playback/file-only evidence");

  ReplayCapabilities invalid;
  invalid.deleteReplayFile = true;
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Corrupt,
         }) == invalid,
         "invalid imported stock file remains deletable");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ImportedStockBrd,
             .replayState = ReplayState::Missing,
         }) == ReplayCapabilities{},
         "missing imported stock file has no record capability");
}

void testUnknownEnumValuesFailClosed() {
  expect(replay::capabilitiesFor({
             .origin = static_cast<RecordOrigin>(255),
             .replayState = ReplayState::Verified,
             .postponedIrSnapshotEligible = true,
         }) == ReplayCapabilities{},
         "unknown origin receives no capability");
  expect(replay::capabilitiesFor({
             .origin = RecordOrigin::ModernChartResult,
             .replayState = static_cast<ReplayState>(255),
         }) == modernRecordOnly(false),
         "unknown replay state grants no replay action");
}

} // namespace

int main() {
  testVerifiedModernMatrix();
  testAbsentReplayKeepsModernResultAndIr();
  testInvalidReplayIsOnlyDeletable();
  testIrEligibilityNeverComesFromReplayState();
  testLegacyAndRemoteRecordsIgnoreReplayState();
  testImportedStockReplaySurface();
  testUnknownEnumValuesFailClosed();
  if (failures != 0) {
    std::cerr << failures << " replay capability test(s) failed\n";
    return 1;
  }
  std::cout << "replay capability tests passed\n";
  return 0;
}
