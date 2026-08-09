#include "skin/GameplaySkinActivationRequest.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string readSource(std::string_view relative) {
  const auto path = std::filesystem::path{ASOBMASHOW_SOURCE_DIR} / relative;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::cerr << "FAIL: unable to read " << path << '\n';
    ++failures;
    return {};
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

void expectContains(const std::string &source, std::string_view needle,
                    std::string_view message) {
  expect(source.find(needle) != std::string::npos, message);
}

void expectAbsent(const std::string &source, std::string_view needle,
                  std::string_view message) {
  expect(source.find(needle) == std::string::npos, message);
}

std::size_t countOccurrences(const std::string &source,
                             std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = source.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

void activationRequestBoundaryIsDefaultEmpty() {
  skin::AcquireGameplaySkinForNextChart acquire;
  expect(!acquire, "the injected next-chart acquisition callback defaults empty");
  const skin::GameplaySkinAcquisition noSelection;
  expect(noSelection.disposition ==
                 skin::GameplaySkinAcquisitionDisposition::BuiltIn &&
             !noSelection.request && !noSelection.failure,
         "an unselected trait is the only acquisition state that selects "
         "built-in gameplay");

  const skin::GameplaySkinAcquisition selectedFailure{
      .disposition = skin::GameplaySkinAcquisitionDisposition::Failed,
      .failure = skin::GameplaySkinAcquisitionFailure{
          .diagnostic = {.code = "skin.test.selected_failure",
                         .message = "selected skin failed",
                         .severity = skin::DiagnosticSeverity::Error}}};
  expect(selectedFailure.disposition ==
                 skin::GameplaySkinAcquisitionDisposition::Failed &&
             !selectedFailure.request && selectedFailure.failure,
         "a selected skin failure cannot be represented as built-in gameplay");
}

void gameplaySceneOwnsOnlyThePresentationBoundary() {
  const auto header = readSource("src/scene/play/GamePlayScene.h");
  const auto source = readSource("src/scene/play/GamePlayScene.cpp");
  const auto factory =
      readSource("src/scene/play/GameplaySkinSessionFactory.cpp");

  expectAbsent(header, "ownedRenderer",
               "GamePlayScene header has no direct renderer owner");
  expectAbsent(header, "BMSRenderer",
               "GamePlayScene header does not name BMSRenderer");
  expectAbsent(source, "BMSRenderer",
               "GamePlayScene source does not name BMSRenderer");
  expectAbsent(source, "renderer->",
               "GamePlayScene has no direct renderer calls");

  expectContains(source, "createBuiltInPlayfieldPresentation(",
                 "gameplay constructs the built-in through its factory");
  expectContains(source, "PlayfieldPresentationCoordinatorDependencies",
                 "gameplay owns the atomic presentation coordinator");
  expectContains(source, "#include \"GameplaySkinSessionFactory.h\"",
                 "gameplay uses the shared selected-skin session factory");
  expectContains(source, "createGameplaySkinSession(",
                 "gameplay requests a selected session through the shared factory");
  expectContains(source, "gameplaySkinSessionServices(context)",
                 "gameplay supplies lifecycle-owned services to the shared factory");
  expectContains(factory, "skin::GameplaySkinAcquisition acquisition",
                 "the factory keeps the move-only selected-skin activation mutable "
                 "until ownership transfers into its session");
  expectContains(factory, "skin::PlaySkinSession::create(",
                 "the factory creates an owning chart-lifetime skin session");
  expectContains(source,
                 "if (result.disposition == GameplaySkinSessionDisposition::Failed)",
                 "gameplay distinguishes a selected-skin acquisition failure");
  expectContains(source,
                 "gameplaySkinFailureMessage(result.failure->diagnostic)",
                 "selected-skin acquisition and session failures open the full-screen error page");
  expectContains(source,
                 "if (playbackInitializationFailed) {\n    return false;\n  }\n  updateSkinResetLayoutVisibility();",
                 "a selected skin failure stops reset before audio and input start");
  expectContains(source,
                 "presentationFrame.outcome == PresentationFrameOutcome::CriticalFailure",
                 "critical skin frame fallback opens the full-screen error page");
  expectContains(source,
                 "presentationFrame.submittedMode == PresentationMode::BuiltIn",
                 "a non-critical selected-skin fallback also opens the full-screen error page");
  expectContains(source,
                 "if (state != nullptr) {\n    state->isPlaying = false;\n    state->isEnding = true;\n  }",
                 "the full-screen failure page stops the current gameplay state");
  expectContains(source,
                 "if (skinResetLayoutButton != nullptr) {\n    skinResetLayoutButton->setVisible(false);\n  }\n  if (playbackFailureLayout != nullptr)",
                 "the full-screen failure page hides the skin-only control");
  expectAbsent(source, "runGameplaySkinAttemptInstallFailClosed(",
               "gameplay no longer merges selected-skin failure with built-in selection");
  expectContains(source, "presentation->prepareFrame(",
                 "the matched state/projection pair is prepared through the presentation");
  expectContains(source, "presentation->render(renderContext)",
                 "the selected presentation renders exactly through its public boundary");
  expectAbsent(source, "prepareActivation(",
               "gameplay never prepares a package activation directly");
  expectAbsent(source, "submitActivation(",
               "gameplay never commits a package activation directly");
  expect(countOccurrences(source, "presentation->prepareFrame(") == 1,
         "gameplay prepares the matched presentation exactly once per frame");
  expect(countOccurrences(source, "presentation->render(renderContext)") == 1,
         "gameplay renders the matched presentation exactly once per frame");
  expectAbsent(source, "chart->Meta.KeyMode != 7",
               "gameplay does not hard-code 7K skin acquisition");
  expectContains(source, ".keyMode = chart->Meta.KeyMode",
                 "gameplay requests the selected skin for the chart keymode");
  const auto initialCapture = source.find("capturePlayfieldVisualState(");
  const auto acquireAfterInitialCapture =
      source.find("acquireGameplaySkinForAttempt();", initialCapture);
  expect(initialCapture != std::string::npos &&
             acquireAfterInitialCapture != std::string::npos &&
             initialCapture < acquireAfterInitialCapture,
         "every retry captures authoritative initial state before reacquiring "
         "through the injected attempt boundary");
  expectContains(source,
                 "acquireGameplaySkinForAttempt();\n  if (playbackInitializationFailed) {\n    return false;\n  }\n  updateSkinResetLayoutVisibility();\n#endif\n  context.jukebox.play",
                 "configured skin loading either completes or stops before attempt audio starts");
  expectContains(source, "*playfieldVisualStateStore, *presentation",
                 "the event fanout targets the coordinator presentation exactly once");
  expectContains(source, ".replayData = options.replayData.get()",
                 "the built-in factory preprocesses replay presentation data");
  expectContains(source, "practiceRestartButton, skinResetLayoutButton",
                 "Reset Layout occupies the third native-overlay slot");
  expectContains(source, "presentation->activeMode() == PresentationMode::Skin",
                 "Reset Layout visibility is gated by the current submitted skin mode");
  expectContains(source, "#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS\n  context.gameplayBgaCompositeState = {",
                 "only coordinator-enabled gameplay publishes the nonlegacy BGA decision");

  const auto stopRealtime = source.find("stopRealtimeGameplayAuthority(false);",
                                        source.find("void GamePlayScene::cleanupScene()"));
  const auto dropFanout = source.find("ownedPresentationEventFanout.reset();",
                                      stopRealtime);
  const auto dropPresentation = source.find("ownedPresentation.reset();",
                                            dropFanout);
  const auto dropVisualStore = source.find("ownedPlayfieldVisualStateStore.reset();",
                                           dropPresentation);
  expect(stopRealtime < dropFanout && dropFanout < dropPresentation &&
             dropPresentation < dropVisualStore,
         "teardown stops ingress before fanout, session presentation, and visual state");
}

void contextPublishesTheGuardedInjectionSeam() {
  const auto source = readSource("src/context.h");
  expectContains(source, "skin::AcquireGameplaySkinForNextChart",
                 "the enabled context exposes the typed acquisition callback");
  expectContains(source, "acquireGameplaySkinForNextChart",
                 "the acquisition callback has one application-owned field");
}

} // namespace

int main() {
  activationRequestBoundaryIsDefaultEmpty();
  gameplaySceneOwnsOnlyThePresentationBoundary();
  contextPublishesTheGuardedInjectionSeam();
  if (failures == 0) {
    std::cout << "gameplay skin integration tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
