#include "scene/play/RealtimeGameplayAuthorityPolicy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testSessionBackedPracticeEligibility() {
  const gameplay::GameplayTimeRange range{.startMicros = 1'000'000,
                                          .endMicros = 2'000'000};
  const auto manual = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .practiceMode = true,
      .practiceRange = range,
      .startPositionMicros = range.startMicros,
      .preparationActivationSongTimeMicros = range.startMicros,
  });
  require(manual.eligible && manual.allowedNoteRange.has_value() &&
              manual.allowedNoteRange->startMicros == range.startMicros &&
              manual.allowedNoteRange->endMicros == range.endMicros &&
              manual.practiceCompletionSongTimeMicros == range.endMicros &&
              !manual.activationSongTimeMicros.has_value(),
          "session-backed manual practice uses exact realtime boundaries");

  const auto autoplay = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = false,
      .autoPlay = true,
      .inputHandlerAvailable = false,
      .practiceMode = true,
      .practiceRange = range,
      .startPositionMicros = range.startMicros,
      .preparationActivationSongTimeMicros = range.startMicros,
  });
  require(autoplay.eligible,
          "practice autoplay does not require native input or an input handler");
  require(!gameplay::preparationInputUsesVisualOnlyPath(true, true) &&
              gameplay::preparationInputUsesVisualOnlyPath(true, false),
          "only session-backed practice judges through preparation");
}

void testExistingExclusionsAndNormalActivationRemain() {
  const auto legacyPractice =
      gameplay::makeRealtimeGameplayAuthorityPolicy({
          .nativeManualInputAvailable = true,
          .autoPlay = false,
          .inputHandlerAvailable = true,
          .practiceMode = true,
      });
  require(!legacyPractice.eligible,
          "legacy practice without a session remains excluded");

  const auto replay = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .replayPlayback = true,
  });
  require(!replay.eligible, "replay playback remains excluded");

  const auto unsupportedManual =
      gameplay::makeRealtimeGameplayAuthorityPolicy({
          .nativeManualInputAvailable = false,
          .autoPlay = false,
          .inputHandlerAvailable = true,
          .practiceMode = true,
          .practiceRange = gameplay::GameplayTimeRange{
              .startMicros = 1'000'000, .endMicros = 2'000'000},
      });
  require(!unsupportedManual.eligible,
          "manual practice keeps fallback on platforms without native input");

  require(gameplay::shouldAttemptRealtimeGameplayReset(true, false, true) &&
              gameplay::shouldAttemptRealtimeGameplayReset(true, true, false) &&
              !gameplay::shouldAttemptRealtimeGameplayReset(false, true, true),
          "loop reset restarts only with a controller and an input authority");

  require(gameplay::classifyRealtimeGameplayTerminal(
              gameplay::GameplayTerminalReason::PracticeComplete, true) ==
              gameplay::RealtimeGameplayTerminalAction::CompletePractice &&
              gameplay::classifyRealtimeGameplayTerminal(
                  gameplay::GameplayTerminalReason::PracticeComplete, false) ==
                  gameplay::RealtimeGameplayTerminalAction::IntegrityFailure,
          "PracticeComplete is accepted only for a session-backed attempt");

  const auto normal = gameplay::makeRealtimeGameplayAuthorityPolicy({
      .nativeManualInputAvailable = true,
      .autoPlay = false,
      .inputHandlerAvailable = true,
      .startPositionMicros = 500'000,
      .preparationActivationSongTimeMicros = 500'000,
  });
  require(normal.eligible && normal.allowedNoteRange.has_value() &&
              normal.allowedNoteRange->startMicros == 500'000 &&
              normal.allowedNoteRange->endMicros ==
                  std::numeric_limits<std::int64_t>::max() &&
              normal.activationSongTimeMicros == 500'000,
          "ordinary partial starts retain open range and activation gate");

  require(gameplay::shouldSuspendRealtimeGameplayForPause(true) &&
              !gameplay::shouldSuspendRealtimeGameplayForPause(false),
          "realtime scoring suspends only when playback also pauses");

  const std::size_t longSparseReplayCapacity =
      gameplay::realtimeGameplayReplayCapacity(2, 600'000'000);
  require(longSparseReplayCapacity > 4096 &&
              longSparseReplayCapacity > 2 * 3 + 1024,
          "long sparse charts reserve replay space for input transitions, not "
          "only notes");
  require(gameplay::realtimeGameplayReplayCapacity(
              2, std::numeric_limits<std::int64_t>::max()) ==
              (1U << 20U) &&
              gameplay::realtimeGameplayReplayCapacity(400'000, 0) ==
                  1'201'024,
          "duration-based replay reserve is bounded without truncating a "
          "larger note-derived requirement");
}

} // namespace

int main() {
  testSessionBackedPracticeEligibility();
  testExistingExclusionsAndNormalActivationRemain();
  return 0;
}
