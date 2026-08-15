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

  require(gameplay::shouldRetryRealtimeGameplayAuthorityAfterSkinFrame(
              true, true) &&
              !gameplay::shouldRetryRealtimeGameplayAuthorityAfterSkinFrame(
                  false, true) &&
              !gameplay::shouldRetryRealtimeGameplayAuthorityAfterSkinFrame(
                  true, false),
          "a deferred iOS authority starts only after a skin publishes lane "
          "geometry");

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

void testSourcePlaytimeCompletesBeforeTrailingWorkerTimeline() {
  require(gameplay::classifyRealtimeGameplayTerminal(
              gameplay::GameplayTerminalReason::None, false, true) ==
              gameplay::RealtimeGameplayTerminalAction::CompleteChart &&
              gameplay::classifyRealtimeGameplayTerminal(
                  gameplay::GameplayTerminalReason::None, true, true) ==
                  gameplay::RealtimeGameplayTerminalAction::Wait,
          "a normal attempt finishes at the source playtime even while the "
          "realtime worker advances trailing empty timelines");
}

void testReplayStatePlayCompletesBeforeTrailingTimeline() {
  require(gameplay::shouldCompleteLegacyGameplayState(
              true, true, false) &&
              !gameplay::shouldCompleteLegacyGameplayState(
                  true, false, true) &&
              gameplay::shouldCompleteLegacyGameplayState(
                  false, false, true),
          "BMSPlayer REPLAY ends at its source playtime instead of waiting "
          "for trailing chart timelines; legacy practice retains its "
          "timeline-bound completion");
}

void testReplayModeUsesLastPlayableNoteInsteadOfAutoplayTimeline() {
  constexpr std::int64_t lastPlayableNoteMicros = 10'000'000;
  constexpr std::int64_t lastTimelineMicros = 80'000'000;
  require(gameplay::terminalMicrosForBeatorajaPlayMode(
              false, false, lastPlayableNoteMicros, lastTimelineMicros) ==
              lastPlayableNoteMicros &&
              gameplay::terminalMicrosForBeatorajaPlayMode(
                  true, false, lastPlayableNoteMicros, lastTimelineMicros) ==
                  lastTimelineMicros &&
              gameplay::terminalMicrosForBeatorajaPlayMode(
                  false, true, lastPlayableNoteMicros, lastTimelineMicros) ==
                  lastPlayableNoteMicros &&
              gameplay::terminalMicrosForBeatorajaPlayMode(
                  true, true, lastPlayableNoteMicros, lastTimelineMicros) ==
                  lastPlayableNoteMicros,
          "BMSPlayer REPLAY retains last-note timing even for a replay "
          "recorded by autoplay");
}

} // namespace

int main() {
  testSessionBackedPracticeEligibility();
  testExistingExclusionsAndNormalActivationRemain();
  testSourcePlaytimeCompletesBeforeTrailingWorkerTimeline();
  testReplayStatePlayCompletesBeforeTrailingTimeline();
  testReplayModeUsesLastPlayableNoteInsteadOfAutoplayTimeline();
  return 0;
}
