# Application Startup Readiness Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop application runtime before scene registration when the active
profile or any required database cannot initialize, show one sanitized fatal
message, and propagate a failure exit status.

**Architecture:** A platform-neutral `application_startup` coordinator is the
only readiness decision owner. It invokes injected database initialization,
fatal reporting, and ready-runtime callbacks; `main.cpp` supplies the real
database callback and SDL presentation adapter while retaining renderer and
window cleanup.

**Tech Stack:** C++23, `std::function`, SDL2 native message boxes, CMake/Ninja,
CTest, Bash/Perl source audit, Xcode synchronized groups.

**Design:**
`docs/superpowers/specs/2026-07-13-application-startup-readiness-gate-design.md`

## Global Constraints

- Fail closed before `SceneManager` construction and the event loop for every
  profile or required-database initialization failure.
- Attempt chart, score, replay, and music database initialization exactly once
  each whenever the profile is ready, even when an earlier initializer fails.
- Database labels are fixed and ordered: `Chart Library`, `Scores`, `Replays`,
  `Music Library`.
- User-facing messages contain no raw diagnostic, path, database filename,
  SQLite text, profile name, or profile identifier.
- `Dependencies` callbacks are mandatory and non-throwing; operational failure
  is represented only by the profile-ready boolean or database status.
- Do not add retry, degraded/read-only mode, an in-app fatal scene, schema
  changes, or mid-session persistence handling.
- Preserve shader, uniform, bgfx, application-context, window, and SDL cleanup
  for success and every modeled startup failure.
- Add every new `src` implementation file to iOS
  `membershipExceptions` as required by the repository instructions.

---

### Task 1: Add and prove the pure startup coordinator

**Files:**

- Create: `src/ApplicationStartup.h`
- Create: `src/ApplicationStartup.cpp`
- Create: `tests/application_startup_tests.cpp`
- Modify: `tests/app_database_initializer_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes:
  `app_database_initializer::DatabaseInitializationStatus` from
  `src/AppDatabaseInitializer.h`.
- Produces:
  `application_startup::Failure`, `application_startup::Result`,
  `application_startup::Dependencies`, and
  `int application_startup::execute(bool, const Dependencies &)`. Task 2 must
  use this API without reconstructing readiness policy in `main.cpp`.

- [ ] **Step 1: Establish the focused baseline**

Run:

```sh
cmake --build cmake-build-debug --target app_database_initializer_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^app_database_initializer_tests$' --output-on-failure
```

Expected: the existing database-initializer test passes and `main` builds.
Record the command and result in
`.superpowers/sdd/application-startup-task-1-report.md`.

- [ ] **Step 2: Add the coordinator tests and test target before the API**

Create `tests/application_startup_tests.cpp` with a small `expect` helper and
these complete cases:

```cpp
#include "../src/ApplicationStartup.h"

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
          .runReadyApplication = [&] {
            ++runtimeCalls;
            events.push_back("runtime");
          },
      });
  expect(exitCode == EXIT_SUCCESS, "success returns EXIT_SUCCESS");
  expect(initializeCalls == 1, "success initializes databases once");
  expect(reportCalls == 0, "success does not report fatal");
  expect(runtimeCalls == 1, "success runs application once");
  expect(events == std::vector<std::string_view>{"databases", "runtime"},
         "database readiness precedes runtime");
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
} // namespace

int main() {
  testSuccessRunsBodyExactlyOnce();
  testProfileFailureShortCircuitsEverything();
  testDatabaseFailuresFailClosed();
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
```

Add the RED target near `app_database_initializer_tests` in `CMakeLists.txt`
without adding a nonexistent implementation source yet:

```cmake
add_executable(application_startup_tests
    tests/application_startup_tests.cpp
)
target_include_directories(application_startup_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_compile_features(application_startup_tests PRIVATE cxx_std_23)
```

Add `application_startup_tests` to the existing registered-test target list.

- [ ] **Step 3: Run the coordinator test to prove RED**

Run:

```sh
cmake --build cmake-build-debug --target application_startup_tests -j 6
```

Expected: compilation fails because `src/ApplicationStartup.h` does not exist.
Record the exact diagnostic. A CMake syntax or unrelated target failure is not
valid RED evidence.

- [ ] **Step 4: Implement the minimal coordinator**

Create `src/ApplicationStartup.h`:

```cpp
#pragma once

#include "AppDatabaseInitializer.h"

#include <functional>
#include <optional>
#include <string>

namespace application_startup {

enum class Failure {
  None,
  ProfileInitialization,
  DatabaseInitialization,
};

struct Result {
  Failure failure = Failure::None;
  std::optional<
      app_database_initializer::DatabaseInitializationStatus> databaseStatus;
  std::string userMessage;

  [[nodiscard]] bool ok() const noexcept {
    return failure == Failure::None;
  }
};

struct Dependencies {
  std::function<app_database_initializer::DatabaseInitializationStatus()>
      initializeDatabases;
  std::function<void(const Result &)> reportFatal;
  std::function<void()> runReadyApplication;
};

int execute(bool profileReady, const Dependencies &dependencies);

} // namespace application_startup
```

Create `src/ApplicationStartup.cpp`:

```cpp
#include "ApplicationStartup.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace application_startup {
namespace {
constexpr std::string_view kProfileMessage =
    "AsoBMaShow could not initialize the active player profile. The "
    "application will close to protect your data. Check available storage, "
    "storage permissions, and the app version, then try again.";
constexpr std::string_view kDatabasePrefix =
    "AsoBMaShow could not initialize required data: ";
constexpr std::string_view kFailureSuffix =
    ". The application will close to protect your data. Check available "
    "storage, storage permissions, and the app version, then try again.";

std::string databaseFailureMessage(
    const app_database_initializer::DatabaseInitializationStatus &status) {
  const std::array<std::pair<bool, std::string_view>, 4> components{{
      {status.chart, "Chart Library"},
      {status.score, "Scores"},
      {status.replay, "Replays"},
      {status.music, "Music Library"},
  }};
  std::string message{kDatabasePrefix};
  bool first = true;
  for (const auto &[ready, label] : components) {
    if (ready) {
      continue;
    }
    if (!first) {
      message += ", ";
    }
    message += label;
    first = false;
  }
  message += kFailureSuffix;
  return message;
}
} // namespace

int execute(bool profileReady, const Dependencies &dependencies) {
  if (!profileReady) {
    const Result result{
        .failure = Failure::ProfileInitialization,
        .databaseStatus = std::nullopt,
        .userMessage = std::string{kProfileMessage},
    };
    dependencies.reportFatal(result);
    return EXIT_FAILURE;
  }

  const auto status = dependencies.initializeDatabases();
  if (!status.ok()) {
    const Result result{
        .failure = Failure::DatabaseInitialization,
        .databaseStatus = status,
        .userMessage = databaseFailureMessage(status),
    };
    dependencies.reportFatal(result);
    return EXIT_FAILURE;
  }

  dependencies.runReadyApplication();
  return EXIT_SUCCESS;
}

} // namespace application_startup
```

Add `src/ApplicationStartup.cpp` to the test target:

```cmake
add_executable(application_startup_tests
    tests/application_startup_tests.cpp
    src/ApplicationStartup.cpp
)
```

- [ ] **Step 5: Make coordinator tests GREEN**

Run:

```sh
cmake --build cmake-build-debug --target application_startup_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^application_startup_tests$' --output-on-failure
```

Expected: 1/1 passes. Inspect the test output; do not infer success from the
build alone.

Temporarily move `dependencies.runReadyApplication()` before
`dependencies.initializeDatabases()` in `ApplicationStartup.cpp`, rebuild, and
rerun `application_startup_tests`. Expected: the database-before-runtime order
assertion fails (and database-failure cases continue to prove runtime is
blocked). Restore the implementation immediately and require 1/1 to pass. This
mutation proves the success test detects an ordering regression rather than
only final call counts.

- [ ] **Step 6: Expand the aggregate initializer characterization**

Refactor `tests/app_database_initializer_tests.cpp` so a table covers each
single component failure and score-plus-replay failure. For every row, count
all four callbacks and assert each equals one:

```cpp
struct Case {
  const char *label;
  app_database_initializer::DatabaseInitializationStatus expected;
};
const Case cases[]{
    {"chart", {.chart = false, .score = true, .replay = true, .music = true}},
    {"score", {.chart = true, .score = false, .replay = true, .music = true}},
    {"replay", {.chart = true, .score = true, .replay = false, .music = true}},
    {"music", {.chart = true, .score = true, .replay = true, .music = false}},
    {"score-replay",
     {.chart = true, .score = false, .replay = false, .music = true}},
};

for (const Case &testCase : cases) {
  int chartCalls = 0;
  int scoreCalls = 0;
  int replayCalls = 0;
  int musicCalls = 0;
  const auto result =
      app_database_initializer::initializeApplicationDatabasesWith(
          [&] { ++chartCalls; return testCase.expected.chart; },
          [&] { ++scoreCalls; return testCase.expected.score; },
          [&] { ++replayCalls; return testCase.expected.replay; },
          [&] { ++musicCalls; return testCase.expected.music; });
  ASSERT_TRUE(chartCalls == 1, "chart initializer called once");
  ASSERT_TRUE(scoreCalls == 1, "score initializer called once");
  ASSERT_TRUE(replayCalls == 1, "replay initializer called once");
  ASSERT_TRUE(musicCalls == 1, "music initializer called once");
  ASSERT_TRUE(result.chart == testCase.expected.chart, "chart status retained");
  ASSERT_TRUE(result.score == testCase.expected.score, "score status retained");
  ASSERT_TRUE(result.replay == testCase.expected.replay,
              "replay status retained");
  ASSERT_TRUE(result.music == testCase.expected.music, "music status retained");
  ASSERT_FALSE(result.ok(), "failure aggregate is not ready");
}
```

Keep the existing all-success case. Do not change production initializer
ordering or short-circuit behavior.

- [ ] **Step 7: Verify, report, and commit Task 1**

Run:

```sh
cmake --build cmake-build-debug \
  --target application_startup_tests app_database_initializer_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(application_startup_tests|app_database_initializer_tests)$'
git diff --check
```

Expected: 2/2 passes. Update the Task 1 report with baseline, exact RED,
GREEN, scope, and self-review.

Commit:

```sh
git add CMakeLists.txt src/ApplicationStartup.h src/ApplicationStartup.cpp \
  tests/application_startup_tests.cpp tests/app_database_initializer_tests.cpp
git commit -m "fix: add application startup readiness coordinator"
```

Request an independent review before Task 2. Fix every Critical/Important
finding and rerun both focused tests.

---

### Task 2: Route application runtime through the fail-closed gate

**Files:**

- Modify: `src/main.cpp`
- Modify: `src/main.h`
- Modify: `src/context.h`
- Modify: `src/CMakeLists.txt`
- Modify:
  `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`
- Create: `scripts/check_application_startup_gate.sh`

**Interfaces:**

- Consumes: Task 1's exact `application_startup::execute` API.
- Produces: `int run()`, a private
  `runReadyApplication(ApplicationContext &)`, one SDL fatal reporter, main
  target/iOS source membership, and a deterministic integration audit.

- [ ] **Step 1: Add a source audit that is RED against the fail-open wiring**

Create executable `scripts/check_application_startup_gate.sh`. Implement
whole-file matching with this prelude:

```sh
#!/usr/bin/env bash
set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
  echo "application startup gate audit failed: $*" >&2
  exit 1
}

match_count() {
  local relative_file=$1
  local pattern=$2
  PATTERN="$pattern" perl -0777 -ne '
    BEGIN { $pattern = $ENV{"PATTERN"}; $count = 0; }
    $count += () = /$pattern/g;
    END { print "$count\n"; }
  ' "$repository_root/$relative_file"
}

require_count() {
  local relative_file=$1
  local pattern=$2
  local expected=$3
  local invariant=$4
  local actual
  actual=$(match_count "$relative_file" "$pattern")
  if [[ "$actual" != "$expected" ]]; then
    fail "$invariant: expected $expected match(es), found $actual"
  fi
}

require_order() {
  local relative_file=$1
  local first=$2
  local second=$3
  local third=$4
  local invariant=$5
  FIRST="$first" SECOND="$second" THIRD="$third" perl -0777 -ne '
    $first = index($_, $ENV{"FIRST"});
    $second = index($_, $ENV{"SECOND"});
    $third = index($_, $ENV{"THIRD"});
    exit(($first >= 0 && $first < $second && $second < $third) ? 0 : 1);
  ' "$repository_root/$relative_file" || fail "$invariant"
}
```

Then enforce:

```sh
require_count src/main.h 'int\s+run\(\);' 1 "failure-capable run declaration"
require_count src/main.h 'void\s+run\(\);' 0 "obsolete void run declaration"
require_count src/main.cpp '#include\s+"ApplicationStartup\.h"' 1 \
  "startup coordinator include"
require_count src/main.cpp 'application_startup::execute\s*\(' 1 \
  "single startup readiness owner"
require_count src/main.cpp 'int\s+run\(\)' 1 \
  "single failure-capable run definition"
require_count src/main.cpp \
  'int\s+run\(\)\s*\{\s*ApplicationContext\s+context;\s*return\s+application_startup::execute\(\s*context\.profileReady\(\),\s*application_startup::Dependencies\s*\{\s*\.initializeDatabases\s*=\s*\[\]\s*\{\s*return\s+app_database_initializer::initializeApplicationDatabases\(\);\s*\},\s*\.reportFatal\s*=\s*\[&context\]\(const\s+application_startup::Result\s*&result\)\s*\{\s*reportStartupFailure\(context,\s*result\);\s*\},\s*\.runReadyApplication\s*=\s*\[&context\]\s*\{\s*runReadyApplication\(context\);\s*\},\s*\}\);\s*\}' \
  1 "profile predicate and all callbacks are bound inside the gate"
require_count src/main.cpp 'runReadyApplication\(context\)' 1 \
  "runtime body is reachable only through the gate callback"
require_count src/main.cpp \
  'const\s+int\s+runExitCode\s*=\s*run\(\);[\s\S]{0,500}?return\s+runExitCode\s*;' \
  1 "run exit propagation after renderer cleanup"
require_count src/main.cpp \
  'static\s+void\s+runReadyApplication\(ApplicationContext\s*&context\)' 1 \
  "ready-only runtime body"
require_count src/main.cpp 'SceneManager\s+sceneManager\(context\)' 1 \
  "single ready runtime scene manager"
require_count src/main.cpp \
  'app_database_initializer::initializeApplicationDatabases\(\)' 1 \
  "single injected database initialization"
require_count src/main.cpp 'SDL_ShowSimpleMessageBox\s*\(' 1 \
  "single native fatal reporter"
require_count src/main.cpp \
  'Unable to show the startup error dialog:[\s\S]{0,120}?SDL_GetError\s*\(\)' 1 \
  "message-box failure logging"
require_count src/main.cpp \
  'Application startup stopped because|if\s*\(!databaseStatus\.ok\(\)\)' 0 \
  "obsolete inline readiness branches"
require_count src/context.h 'Player profile initialization failed' 0 \
  "duplicate context startup log"
require_count src/CMakeLists.txt '\bApplicationStartup\.cpp\b' 1 \
  "main target startup source"
require_count ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  '[[:space:]]ApplicationStartup\.cpp,' 1 "iOS startup source membership"
require_order src/main.cpp \
  'static void runReadyApplication(ApplicationContext &context)' \
  'SceneManager sceneManager(context)' \
  'int run()' \
  "SceneManager must remain inside the ready body before the gate definition"

echo "application startup gate audit passed"
```

Mark the script executable with `chmod +x
scripts/check_application_startup_gate.sh`.

- [ ] **Step 2: Prove the source audit is RED**

Run:

```sh
scripts/check_application_startup_gate.sh
```

Expected: failure at the first missing `int run()`/coordinator invariant. A
shell syntax failure is not valid RED evidence.

- [ ] **Step 3: Add the coordinator to platform build metadata**

Add `ApplicationStartup.cpp` to `target_sources(main PRIVATE ...)` in
`src/CMakeLists.txt`.

Add exactly one `ApplicationStartup.cpp,` entry to the iOS `src`
`membershipExceptions` list, alphabetically beside `AppSettings.cpp`.

- [ ] **Step 4: Isolate the existing ready-only runtime body**

In `src/main.cpp`, include `ApplicationStartup.h`. Rename the current `run()`
body to:

```cpp
static void runReadyApplication(ApplicationContext &context) {
  context.bgfxResetFlags.store(s_bgfxResetFlags, std::memory_order_relaxed);
  // Existing view setup, SceneManager registration, event loop, and shutdown
  // remain here unchanged.
}
```

Remove the local `ApplicationContext`, profile-ready branch, database
initializer, and log-only database branch from that body. Do not move or alter
scene registration, frame behavior, or runtime shutdown logic.

- [ ] **Step 5: Add one SDL presentation adapter and gated `run()`**

Add a private reporter near the runtime body:

```cpp
static void reportStartupFailure(
    const ApplicationContext &context,
    const application_startup::Result &result) {
  switch (result.failure) {
  case application_startup::Failure::ProfileInitialization:
    SDL_Log("Application profile initialization failed: %s",
            context.profileInitializationResult.message.empty()
                ? "no diagnostic available"
                : context.profileInitializationResult.message.c_str());
    break;
  case application_startup::Failure::DatabaseInitialization:
    if (result.databaseStatus) {
      const auto &status = *result.databaseStatus;
      SDL_Log("Application database initialization failed: chart=%d score=%d "
              "replay=%d music=%d",
              status.chart ? 1 : 0, status.score ? 1 : 0,
              status.replay ? 1 : 0, status.music ? 1 : 0);
    }
    break;
  case application_startup::Failure::None:
    SDL_Log("Application startup reported an unspecified fatal failure");
    break;
  }

  if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                               "AsoBMaShow Startup Error",
                               result.userMessage.c_str(), s_window) != 0) {
    SDL_Log("Unable to show the startup error dialog: %s", SDL_GetError());
  }
}
```

Add the global gate after `runReadyApplication`:

```cpp
int run() {
  ApplicationContext context;
  return application_startup::execute(
      context.profileReady(),
      application_startup::Dependencies{
          .initializeDatabases = [] {
            return app_database_initializer::initializeApplicationDatabases();
          },
          .reportFatal = [&context](const application_startup::Result &result) {
            reportStartupFailure(context, result);
          },
          .runReadyApplication = [&context] {
            runReadyApplication(context);
          },
      });
}
```

Change `src/main.h` to declare `int run();`.

In `src/context.h`, remove only the constructor's duplicate
`SDL_Log("Player profile initialization failed...")`; keep the early return and
all profile-ready semantics unchanged.

- [ ] **Step 6: Propagate the exit status without bypassing renderer cleanup**

Replace the unconditional success path inside `runApplication` with:

```cpp
const int runExitCode = run();
rendering::ShaderManager::getInstance().release();
rendering::UniformCache::getInstance().destroyAll();
bgfx::shutdown();
return runExitCode;
```

Do not return before these cleanup calls and do not retry another renderer after
the application-level startup gate fails.

- [ ] **Step 7: Make the integration audit and focused builds GREEN**

Run:

```sh
scripts/check_application_startup_gate.sh
cmake --build cmake-build-debug \
  --target application_startup_tests app_database_initializer_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(application_startup_tests|app_database_initializer_tests)$'
git diff --check
```

Expected: audit passes, both tests pass, and `main` links.

- [ ] **Step 8: Prove the audit detects integration regressions**

Temporarily make and immediately restore each mutation, running the audit after
each mutation:

1. Change `int run();` back to `void run();`.
2. Change `return runExitCode;` back to `return EXIT_SUCCESS;`.
3. Remove `ApplicationStartup.cpp,` from the iOS membership exception list.
4. Move `runReadyApplication(context);` immediately before the
   `application_startup::execute(...)` call and replace the injected ready
   callback body with `{}`. This must fail the complete gate-wiring invariant
   even though there remains one call to the runtime body and one call to
   `execute`.

Expected: each mutation makes the audit fail for its named invariant. Restore
the exact tracked source after every mutation, rerun the audit, and require it
to pass before continuing.

- [ ] **Step 9: Report and commit Task 2**

Write `.superpowers/sdd/application-startup-task-2-report.md` with audit RED,
GREEN, mutation evidence, build/test results, source scope, and self-review.

Commit:

```sh
git add src/main.cpp src/main.h src/context.h src/CMakeLists.txt \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj \
  scripts/check_application_startup_gate.sh
git commit -m "fix: fail closed when application storage is unavailable"
```

Request an independent review. Fix every Critical/Important finding and rerun
the audit, both tests, and `main` build before Task 3.

---

### Task 3: Verify startup safety and complete independent reviews

**Files:**

- Verify only; modify production/tests only to resolve review findings.

**Interfaces:**

- Consumes: Tasks 1 and 2 as an integrated startup path.
- Produces: fresh build/test/platform-metadata evidence and two independent
  READY verdicts.

- [ ] **Step 1: Build affected targets and the desktop application**

Run:

```sh
cmake --build cmake-build-debug \
  --target application_startup_tests app_database_initializer_tests main -j 6
```

Expected: every target builds and links. Existing third-party compiler warnings
do not hide a nonzero exit.

- [ ] **Step 2: Run focused and complete desktop tests**

Run:

```sh
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(application_startup_tests|app_database_initializer_tests)$'
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: focused 2/2 and the complete suite pass. The configured suite should
increase from 57 to 58 entries after adding `application_startup_tests`.

- [ ] **Step 3: Verify source-of-truth and platform metadata**

Run:

```sh
scripts/check_application_startup_gate.sh
STARTUP_PLAN_BASE=$(git log -1 --format=%H -- \
  docs/superpowers/plans/2026-07-13-application-startup-readiness-gate.md)
git diff --check "$STARTUP_PLAN_BASE"..HEAD
git diff --name-only "$STARTUP_PLAN_BASE"..HEAD
git status --short --branch
```

Expected implementation scope:

- `CMakeLists.txt`
- `src/ApplicationStartup.h`
- `src/ApplicationStartup.cpp`
- `src/main.cpp`
- `src/main.h`
- `src/context.h`
- `src/CMakeLists.txt`
- `tests/application_startup_tests.cpp`
- `tests/app_database_initializer_tests.cpp`
- `scripts/check_application_startup_gate.sh`
- `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

- [ ] **Step 4: Run safe mobile compile checks when configured**

Run the non-deploying iOS compile check:

```sh
scripts/ios_firebase_deploy.sh --build-only
```

If Android release signing environment is present, also run:

```sh
scripts/android_firebase_deploy.sh --build-only
```

Neither command may be run without `--build-only`; this remediation does not
authorize deployment. If a required private build environment is absent,
record the exact configuration blocker and rely on desktop compilation plus the
deterministic iOS membership audit rather than changing credentials.

- [ ] **Step 5: Request two independent final reviews**

Acceptance reviewer checks every design criterion, exact sanitized messages,
all-component attempts, profile short-circuiting, runtime blocking, exit status,
cleanup ordering, source scope, and mobile build membership.

Quality reviewer checks callback lifetime/non-throwing assumptions, full-status
preservation, message-label ordering/privacy, reporter/log ownership, absence of
duplicate readiness branches, mutation-sensitive tests/audit, and CMake/CTest
registration.

Fix every Critical/Important finding, rerun affected focused tests plus the
complete audit, and re-review. Report this remediation complete only after both
reviewers explicitly return READY and all fresh verification commands exit
zero.
