#include "../src/ApplicationStartup.h"
#include "../src/rendering/BgfxInitLimits.h"

#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view label) {
  if (!condition) {
    std::cerr << "FAILED: " << label << '\n';
    ++failures;
  }
}

using Status = app_database_initializer::DatabaseInitializationStatus;
using application_startup::Dependencies;
using application_startup::Failure;
using application_startup::Result;

bool sameStatus(const Status &left, const Status &right) {
  return left.chart == right.chart && left.score == right.score &&
         left.replay == right.replay && left.music == right.music;
}

constexpr std::string_view kProfileMessage =
    "AsoBMaShow could not initialize the active player profile. The "
    "application will close to protect your data. Check available storage, "
    "storage permissions, and the app version, then try again.";

std::string databaseMessage(std::string_view failedLabels) {
  return "AsoBMaShow could not initialize required data: " +
         std::string(failedLabels) +
         ". The application will close to protect your data. Check available "
         "storage, storage permissions, and the app version, then try again.";
}

void testSuccessRunsBodyExactlyOnce() {
  int initializeCalls = 0;
  int reportCalls = 0;
  int runtimeCalls = 0;
  int reconciliationCalls = 0;
  std::vector<std::string_view> events;
  const int exitCode = application_startup::execute(
      true,
      Dependencies{
          .initializeDatabases = [&] {
            ++initializeCalls;
            events.push_back("databases");
            return Status{.chart = true,
                          .score = true,
                          .replay = true,
                          .music = true};
          },
          .reportFatal = [&](const Result &) { ++reportCalls; },
          .reconcileReplayFiles = [&] {
            ++reconciliationCalls;
            events.push_back("reconcile");
          },
          .runReadyApplication = [&] {
            ++runtimeCalls;
            events.push_back("runtime");
          },
      });
  expect(exitCode == EXIT_SUCCESS, "success returns EXIT_SUCCESS");
  expect(initializeCalls == 1, "success initializes databases once");
  expect(reportCalls == 0, "success does not report fatal");
  expect(runtimeCalls == 1, "success runs application once");
  expect(reconciliationCalls == 1, "success reconciles replay files once");
  expect(events == std::vector<std::string_view>{"databases", "reconcile",
                                                 "runtime"},
         "database readiness and reconciliation precede runtime");
}

void testReplayReconciliationFailureDoesNotBlockResultsOrRuntime() {
  int runtimeCalls = 0;
  const int exitCode = application_startup::execute(
      true,
      Dependencies{
          .initializeDatabases = [] {
            return Status{.chart = true,
                          .score = true,
                          .replay = true,
                          .music = true};
          },
          .reportFatal = [](const Result &) { assert(false); },
          .reconcileReplayFiles = [] { throw 42; },
          .runReadyApplication = [&] { ++runtimeCalls; },
      });
  expect(exitCode == EXIT_SUCCESS && runtimeCalls == 1,
         "best-effort replay reconciliation cannot block startup");
}

void testProfileFailureShortCircuitsEverything() {
  int initializeCalls = 0;
  int reportCalls = 0;
  int runtimeCalls = 0;
  std::optional<Result> reported;
  const int exitCode = application_startup::execute(
      false,
      Dependencies{
          .initializeDatabases = [&] {
            ++initializeCalls;
            return Status{};
          },
          .reportFatal = [&](const Result &result) {
            ++reportCalls;
            reported = result;
          },
          .runReadyApplication = [&] { ++runtimeCalls; },
      });
  expect(exitCode == EXIT_FAILURE, "profile failure returns EXIT_FAILURE");
  expect(initializeCalls == 0, "profile failure skips databases");
  expect(runtimeCalls == 0, "profile failure skips runtime");
  expect(reportCalls == 1 && reported.has_value(),
         "profile failure reports exactly once");
  expect(reported && reported->failure == Failure::ProfileInitialization,
         "profile failure keeps its kind");
  expect(reported && !reported->databaseStatus.has_value(),
         "profile failure has no database status");
  expect(reported && reported->userMessage == kProfileMessage,
         "profile failure message is exact and sanitized");
}

struct DatabaseFailureCase {
  std::string_view label;
  Status status;
  std::string_view failedLabels;
  std::vector<std::string_view> absentLabels;
};

void testDatabaseFailuresFailClosed() {
  const std::vector<DatabaseFailureCase> cases{
      {"chart", {.chart = false, .score = true, .replay = true, .music = true},
       "Chart Library", {"Scores", "Replays", "Music Library"}},
      {"score", {.chart = true, .score = false, .replay = true, .music = true},
       "Scores", {"Chart Library", "Replays", "Music Library"}},
      {"replay", {.chart = true, .score = true, .replay = false, .music = true},
       "Replays", {"Chart Library", "Scores", "Music Library"}},
      {"music", {.chart = true, .score = true, .replay = true, .music = false},
       "Music Library", {"Chart Library", "Scores", "Replays"}},
      {"score and replay",
       {.chart = true, .score = false, .replay = false, .music = true},
       "Scores, Replays", {"Chart Library", "Music Library"}},
  };

  for (const auto &testCase : cases) {
    int initializeCalls = 0;
    int reportCalls = 0;
    int runtimeCalls = 0;
    std::optional<Result> reported;
    const int exitCode = application_startup::execute(
        true,
        Dependencies{
            .initializeDatabases = [&] {
              ++initializeCalls;
              return testCase.status;
            },
            .reportFatal = [&](const Result &result) {
              ++reportCalls;
              reported = result;
            },
            .runReadyApplication = [&] { ++runtimeCalls; },
        });
    const std::string prefix = std::string(testCase.label) + ": ";
    expect(exitCode == EXIT_FAILURE,
           prefix + "database failure returns EXIT_FAILURE");
    expect(initializeCalls == 1, prefix + "initializer runs once");
    expect(runtimeCalls == 0, prefix + "runtime is blocked");
    expect(reportCalls == 1 && reported.has_value(),
           prefix + "fatal report runs once");
    expect(reported && reported->failure == Failure::DatabaseInitialization,
           prefix + "failure kind is preserved");
    expect(reported && reported->databaseStatus &&
               sameStatus(*reported->databaseStatus, testCase.status),
           prefix + "complete database status is preserved");
    expect(reported &&
               reported->userMessage == databaseMessage(testCase.failedLabels),
           prefix + "database message is exact");
    for (const std::string_view absent : testCase.absentLabels) {
      expect(reported && reported->userMessage.find(absent) ==
                             std::string::npos,
             prefix + "successful component is absent");
    }
  }
}

void testBgfxTransientBuffersAreExpanded() {
  struct Limits {
    uint32_t maxTransientVbSize = 0;
    uint32_t maxTransientIbSize = 0;
  } limits;

  rendering::applyBgfxTransientBufferLimits(limits);

  expect(limits.maxTransientVbSize == 16U * 1024U * 1024U,
         "bgfx transient vertex buffer is expanded to 16 MiB");
  expect(limits.maxTransientIbSize == 4U * 1024U * 1024U,
         "bgfx transient index buffer is expanded to 4 MiB");
}
} // namespace

int main() {
  testSuccessRunsBodyExactlyOnce();
  testReplayReconciliationFailureDoesNotBlockResultsOrRuntime();
  testProfileFailureShortCircuitsEverything();
  testDatabaseFailuresFailClosed();
  testBgfxTransientBuffersAreExpanded();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
