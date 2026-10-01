#include "../src/scene/ProfileRuntimeReapply.h"
#include "../src/i18n/Localization.h"

#include <stdexcept>

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

ProfileRuntimeReapplyCallbacks
callbacks(std::vector<std::string> &events, std::string audioWarning = {},
          ProfileDisplayRuntimeResult display = {
              .outcome = ProfileDisplayRuntimeOutcome::Applied}) {
  return {.sanitize = [&]() { events.emplace_back("sanitize"); },
          .applyTheme = [&]() { events.emplace_back("theme"); },
          .applyJukebox = [&]() { events.emplace_back("jukebox"); },
          .applyMetadata =
              [&]() {
                events.emplace_back("metadata");
                return std::string{};
              },
          .applyAudio =
              [&, warning = std::move(audioWarning)]() {
                events.emplace_back("audio");
                return warning;
              },
          .refreshDrafts = [&]() { events.emplace_back("drafts"); },
          .applyDisplay =
              [&, result = std::move(display)]() {
                events.emplace_back("display");
                return result;
              }};
}

void testFailedSwitchDoesNoRuntimeWork() {
  std::vector<std::string> events;
  const auto result = ReapplyProfileRuntimeAfterSwitch(
      {.error = ProfileError::IoFailure, .message = "switch failed"},
      callbacks(events));
  REQUIRE(!result.profileCommitted);
  REQUIRE(events.empty());
  REQUIRE(result.warnings.empty());
}

void testSuccessfulSwitchAppliesExactlyOnceInOrder() {
  std::vector<std::string> events;
  const auto result = ReapplyProfileRuntimeAfterSwitch({}, callbacks(events));
  REQUIRE(result.profileCommitted);
  REQUIRE((events == std::vector<std::string>{"sanitize", "theme", "jukebox",
                                              "metadata", "audio", "drafts",
                                              "display"}));
  REQUIRE(result.warnings.empty());
}

void testRuntimeFailuresWarnWithoutUndoingCommittedProfile() {
  std::vector<std::string> events;
  const auto result = ReapplyProfileRuntimeAfterSwitch(
      {}, callbacks(events, "audio restart failed",
                    {.outcome = ProfileDisplayRuntimeOutcome::Failed,
                     .message = "display apply failed"}));
  REQUIRE(result.profileCommitted);
  REQUIRE((events == std::vector<std::string>{"sanitize", "theme", "jukebox",
                                              "metadata", "audio", "drafts",
                                              "display"}));
  REQUIRE(result.warnings.size() == 2);
  REQUIRE(i18n::Text(result.warnings[0]).resolve() == "audio restart failed");
  REQUIRE(i18n::Text(result.warnings[1]).resolve() == "display apply failed");
}

void testPreviewPendingIsAccepted() {
  std::vector<std::string> events;
  const auto result = ReapplyProfileRuntimeAfterSwitch(
      {}, callbacks(events, {},
                    {.outcome = ProfileDisplayRuntimeOutcome::PreviewPending,
                     .message = "confirm within 15 seconds"}));
  REQUIRE(result.profileCommitted);
  REQUIRE(result.warnings.empty());
  REQUIRE(events.back() == "display");
}
void testOwnedRuntimeFailuresLocalizeAndKeepBackendDetailsLiteral() {
  std::vector<std::string> events;
  i18n::setLanguage(i18n::Language::Korean);
  auto result = ReapplyProfileRuntimeAfterSwitch(
      {}, callbacks(events, {}, {.outcome = ProfileDisplayRuntimeOutcome::Failed}));
  REQUIRE(result.warnings.size() == 1);
  REQUIRE(i18n::Text(result.warnings[0]).resolve() == "디스플레이 재적용에 실패했습니다.");
  auto throwing = callbacks(events);
  throwing.sanitize = [] { throw std::runtime_error("Theme reapplication"); };
  i18n::setLanguage(i18n::Language::Japanese);
  result = ReapplyProfileRuntimeAfterSwitch({}, throwing);
  REQUIRE(result.warnings.size() == 1);
  REQUIRE(i18n::Text(result.warnings[0]).resolve() == "設定の検証に失敗しました: Theme reapplication");
  i18n::setLanguage(i18n::Language::English);
  REQUIRE(i18n::Text(result.warnings[0]).resolve() ==
          "Settings sanitization failed: Theme reapplication");
}
} // namespace

int main() {
  testOwnedRuntimeFailuresLocalizeAndKeepBackendDetailsLiteral();
  testFailedSwitchDoesNoRuntimeWork();
  testSuccessfulSwitchAppliesExactlyOnceInOrder();
  testRuntimeFailuresWarnWithoutUndoingCommittedProfile();
  testPreviewPendingIsAccepted();
  std::cout << "profile runtime reapply tests passed\n";
  return 0;
}
