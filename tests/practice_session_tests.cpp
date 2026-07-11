#include "practice/PracticeSession.h"

#include <iostream>
#include <utility>

namespace {
bool expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}
} // namespace

int main() {
  practice::Configuration configuration{
      .chartSha256 =
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      .startMicros = 1'000'000,
      .endMicros = 5'000'000,
      .loop = true,
      .countInBeats = 4,
  };

  practice::Session session(configuration);
  if (!expect(session.configuration() == configuration,
              "session retains its configuration snapshot") ||
      !expect(session.loopNumber() == 1, "first loop starts at one")) {
    return 1;
  }

  session.beginAttempt();
  ReplayData completedReplay;
  completedReplay.events.push_back({.action = ReplayEventAction::Press,
                                    .lane = 1,
                                    .noteTimeMicros = 1'000'000,
                                    .songTimeMicros = 1'000'000,
                                    .judgeTimeMicros = 1'005'000,
                                    .judgement = Great,
                                    .diffMicros = 5'000});
  session.completeAttempt(std::move(completedReplay));
  if (!expect(session.completedAttempts().size() == 1,
              "completed attempt retained") ||
      !expect(session.completedAttempts().front().events.size() == 1,
              "completed replay events retained") ||
      !expect(session.loopNumber() == 2,
              "completed attempt advances the loop") ||
      !expect(session.shouldLoop(), "looping follows configuration")) {
    return 1;
  }

  session.beginAttempt();
  session.abandonAttempt();
  if (!expect(session.completedAttempts().size() == 1,
              "abandoned attempt excluded from completed attempts") ||
      !expect(session.abandonedAttemptCount() == 1,
              "abandoned attempt counted")) {
    return 1;
  }

  practice::Session ghostSession(configuration);
  std::vector<std::size_t> publishedEventCounts;
  std::optional<std::size_t> visibleGhostEventCount;
  auto applyGhostUpdate = [&](const ReplayData *completedAttempt) {
    if (completedAttempt != nullptr) {
      publishedEventCounts.push_back(completedAttempt->events.size());
      if (completedAttempt->events.empty()) {
        visibleGhostEventCount.reset();
      } else {
        visibleGhostEventCount = completedAttempt->events.size();
      }
    }
  };

  ghostSession.beginAttempt();
  ReplayData nonEmptyGhost;
  nonEmptyGhost.events.push_back({.action = ReplayEventAction::Press,
                                  .lane = 2,
                                  .noteTimeMicros = 2'000'000});
  ghostSession.completeAttempt(std::move(nonEmptyGhost));
  applyGhostUpdate(
      practice::completedAttemptForGhost(ghostSession, true));
  if (!expect(visibleGhostEventCount == 1,
              "non-empty completion installs visible ghost")) {
    return 1;
  }

  ghostSession.beginAttempt();
  ghostSession.completeAttempt(ReplayData{});
  applyGhostUpdate(
      practice::completedAttemptForGhost(ghostSession, true));
  if (!expect(!visibleGhostEventCount.has_value(),
              "empty completion clears visible ghost")) {
    return 1;
  }

  ghostSession.beginAttempt();
  ghostSession.abandonAttempt();
  applyGhostUpdate(
      practice::completedAttemptForGhost(ghostSession, false));

  if (!expect(publishedEventCounts.size() == 2,
              "abandoned attempt publishes no ghost update") ||
      !expect(publishedEventCounts[0] == 1,
              "non-empty completed attempt replaces ghost") ||
      !expect(publishedEventCounts[1] == 0,
              "empty completed attempt clears ghost") ||
      !expect(!visibleGhostEventCount.has_value(),
              "abandoned attempt leaves cleared ghost unchanged")) {
    return 1;
  }

  return 0;
}
