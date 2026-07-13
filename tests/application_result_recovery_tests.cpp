#include "../src/ApplicationResultRecovery.h"

#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

using application_result_recovery::Dependencies;
using result_persistence::RecoverySummary;

constexpr std::string_view kRecoveryMessage =
    "Some previously completed results are still waiting to be saved. They "
    "were kept safely and will be retried later.";

void testCleanRecoveryRunsRuntimeWithoutWarning() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int runtimeCalls = 0;

  application_result_recovery::execute(
      Dependencies{
          .recover = [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return RecoverySummary{};
          },
          .reportWarning = [&](const RecoverySummary &) {
            ++warningCalls;
            events.emplace_back("warning");
          },
          .runReadyRuntime = [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
      });

  assert(recoverCalls == 1);
  assert(warningCalls == 0);
  assert(runtimeCalls == 1);
  assert(events == std::vector<std::string>({"recover", "runtime"}));
}

void testPendingRecoveryWarnsBeforeRuntime() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int runtimeCalls = 0;
  RecoverySummary reported;

  application_result_recovery::execute(
      Dependencies{
          .recover = [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return RecoverySummary{
                .attempted = 3,
                .saved = 2,
                .pending = 1,
                .conflicts = 0,
                .userMessage = std::string(kRecoveryMessage),
                .diagnostic = "attempt-private: /private/profile/replays.db",
            };
          },
          .reportWarning = [&](const RecoverySummary &summary) {
            ++warningCalls;
            events.emplace_back("warning");
            reported = summary;
          },
          .runReadyRuntime = [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
      });

  assert(recoverCalls == 1);
  assert(warningCalls == 1);
  assert(runtimeCalls == 1);
  assert(reported.pending == 1);
  assert(reported.userMessage == kRecoveryMessage);
  assert(events ==
         std::vector<std::string>({"recover", "warning", "runtime"}));
}

void testConflictRecoveryWarnsBeforeRuntime() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int runtimeCalls = 0;
  RecoverySummary reported;

  application_result_recovery::execute(
      Dependencies{
          .recover = [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return RecoverySummary{
                .attempted = 2,
                .saved = 0,
                .pending = 0,
                .conflicts = 2,
                .userMessage = std::string(kRecoveryMessage),
                .diagnostic = "integrity conflict",
            };
          },
          .reportWarning = [&](const RecoverySummary &summary) {
            ++warningCalls;
            events.emplace_back("warning");
            reported = summary;
          },
          .runReadyRuntime = [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
      });

  assert(recoverCalls == 1);
  assert(warningCalls == 1);
  assert(runtimeCalls == 1);
  assert(reported.conflicts == 2);
  assert(reported.userMessage == kRecoveryMessage);
  assert(events ==
         std::vector<std::string>({"recover", "warning", "runtime"}));
}

} // namespace

int main() {
  testCleanRecoveryRunsRuntimeWithoutWarning();
  testPendingRecoveryWarnsBeforeRuntime();
  testConflictRecoveryWarnsBeforeRuntime();
  return EXIT_SUCCESS;
}
