#include "../src/ApplicationResultRecovery.h"

#include <cassert>
#include <cstdlib>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace {

using application_result_recovery::Dependencies;
using replay::ChartReplayRecoverySummary;

void testCleanRecoveryRunsRuntimeWithoutWarning() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int serviceStartCalls = 0;
  int runtimeCalls = 0;

  application_result_recovery::execute(Dependencies{
      .recover =
          [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return ChartReplayRecoverySummary{};
          },
      .reportWarning =
          [&](const ChartReplayRecoverySummary &) {
            ++warningCalls;
            events.emplace_back("warning");
          },
      .startProfileServices =
          [&] {
            ++serviceStartCalls;
            events.emplace_back("services");
          },
      .runReadyRuntime =
          [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
  });

  assert(recoverCalls == 1);
  assert(warningCalls == 0);
  assert(serviceStartCalls == 1);
  assert(runtimeCalls == 1);
  assert(events ==
         std::vector<std::string>({"recover", "services", "runtime"}));
}

void testPendingRecoveryWarnsBeforeRuntime() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int serviceStartCalls = 0;
  int runtimeCalls = 0;
  ChartReplayRecoverySummary reported;

  application_result_recovery::execute(Dependencies{
      .recover =
          [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return ChartReplayRecoverySummary{
                .attempted = 3,
                .saved = 2,
                .pending = 1,
                .conflicts = 0,
                .diagnostic = "attempt-private: /private/profile/replays.db",
            };
          },
      .reportWarning =
          [&](const ChartReplayRecoverySummary &summary) {
            ++warningCalls;
            events.emplace_back("warning");
            reported = summary;
          },
      .startProfileServices =
          [&] {
            ++serviceStartCalls;
            events.emplace_back("services");
          },
      .runReadyRuntime =
          [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
  });

  assert(recoverCalls == 1);
  assert(warningCalls == 1);
  assert(serviceStartCalls == 1);
  assert(runtimeCalls == 1);
  assert(reported.pending == 1);
  assert(events == std::vector<std::string>(
                       {"recover", "warning", "services", "runtime"}));
}

void testConflictRecoveryWarnsBeforeRuntime() {
  std::vector<std::string> events;
  int recoverCalls = 0;
  int warningCalls = 0;
  int serviceStartCalls = 0;
  int runtimeCalls = 0;
  ChartReplayRecoverySummary reported;

  application_result_recovery::execute(Dependencies{
      .recover =
          [&] {
            ++recoverCalls;
            events.emplace_back("recover");
            return ChartReplayRecoverySummary{
                .attempted = 2,
                .saved = 0,
                .pending = 0,
                .conflicts = 2,
                .diagnostic = "integrity conflict",
            };
          },
      .reportWarning =
          [&](const ChartReplayRecoverySummary &summary) {
            ++warningCalls;
            events.emplace_back("warning");
            reported = summary;
          },
      .startProfileServices =
          [&] {
            ++serviceStartCalls;
            events.emplace_back("services");
          },
      .runReadyRuntime =
          [&] {
            ++runtimeCalls;
            events.emplace_back("runtime");
          },
  });

  assert(recoverCalls == 1);
  assert(warningCalls == 1);
  assert(serviceStartCalls == 1);
  assert(runtimeCalls == 1);
  assert(reported.conflicts == 2);
  assert(events == std::vector<std::string>(
                       {"recover", "warning", "services", "runtime"}));
}

void testIrStartupFailureDoesNotBlockReadyRuntime() {
  std::vector<std::string> events;
  application_result_recovery::execute(Dependencies{
      .recover =
          [&] {
            events.emplace_back("recover");
            return ChartReplayRecoverySummary{};
          },
      .reportWarning =
          [&](const ChartReplayRecoverySummary &) {
            events.emplace_back("warning");
          },
      .startProfileServices =
          [&] {
            events.emplace_back("services");
            throw std::runtime_error("offline IR startup");
          },
      .runReadyRuntime = [&] { events.emplace_back("runtime"); },
  });
  assert(events ==
         std::vector<std::string>({"recover", "services", "runtime"}));
}

} // namespace

int main() {
  testCleanRecoveryRunsRuntimeWithoutWarning();
  testPendingRecoveryWarnsBeforeRuntime();
  testConflictRecoveryWarnsBeforeRuntime();
  testIrStartupFailureDoesNotBlockReadyRuntime();
  return EXIT_SUCCESS;
}
