#include "skin/GameplaySkinActivationRequest.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
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
}

void constructionExceptionFailsClosedAfterConsumingTheRequest() {
  struct FakeCoordinator {
    bool skinInstalled = false;
  } coordinator;
  int acquisitionCount = 0;
  int constructionCount = 0;
  int failureCount = 0;

  const bool installed = skin::runGameplaySkinAttemptInstallFailClosed(
      [&]() -> std::optional<int> {
        ++acquisitionCount;
        return 7;
      },
      [&](int) -> std::unique_ptr<int> {
        ++constructionCount;
        throw std::runtime_error("texture factory failed");
      },
      [&](std::unique_ptr<int>) { coordinator.skinInstalled = true; },
      [&]() { ++failureCount; });

  expect(!installed, "a construction exception does not install a skin");
  expect(acquisitionCount == 1,
         "the next-attempt activation is consumed exactly once");
  expect(constructionCount == 1,
         "the throwing construction factory is invoked exactly once");
  expect(failureCount == 1,
         "the fail-closed boundary reports one construction failure");
  expect(!coordinator.skinInstalled,
         "the warmed coordinator presentation remains built-in");
}

void gameplaySceneOwnsOnlyThePresentationBoundary() {
  const auto header = readSource("src/scene/play/GamePlayScene.h");
  const auto source = readSource("src/scene/play/GamePlayScene.cpp");

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
  expectContains(source, "context.acquireGameplaySkinForNextChart",
                 "gameplay calls only the injected next-chart acquisition seam");
  expectContains(source, "skin::PlaySkinSession::create(",
                 "gameplay creates an owning chart-lifetime skin session");
  expectContains(source, "runGameplaySkinAttemptInstallFailClosed(",
                 "request consumption and session construction share one fail-closed boundary");
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
  expectContains(source, "chart->Meta.KeyMode != 7",
                 "unsupported charts cannot consume a 7-key activation");
  expectContains(source, "presentation->reset();\n  gameplaySkinSafeBoundsInitialized = false;\n  acquireGameplaySkinForAttempt();",
                 "every retry reset reacquires through the injected attempt boundary");
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
  constructionExceptionFailsClosedAfterConsumingTheRequest();
  gameplaySceneOwnsOnlyThePresentationBoundary();
  contextPublishesTheGuardedInjectionSeam();
  if (failures == 0) {
    std::cout << "gameplay skin integration tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}
