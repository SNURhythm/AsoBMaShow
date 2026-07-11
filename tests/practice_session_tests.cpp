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

  return 0;
}
