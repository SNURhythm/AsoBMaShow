# Application Startup Readiness Gate Design

## Context

Application startup already aggregates the initialization state of the chart,
score, replay, and music-library databases. The aggregate correctly reports
failure unless all four databases initialize, and it deliberately attempts all
four initializers so the log can describe the complete failure set.

The runtime discards that decision. `run()` logs a failed aggregate and then
registers scenes and enters the event loop anyway. A failed player-profile
initialization returns before the event loop, but `runApplication()` still
reports process success. As a result, required storage can be unavailable while
the application appears usable, and later score/replay writes can fail as
log-only events.

This is a startup ownership defect: `DatabaseInitializationStatus::ok()` says
the required state is not ready, while `run()` independently decides to start
the application.

## Goals

- Make one coordinator the source of truth for whether application runtime may
  begin.
- Fail closed before scene registration and the event loop when the active
  profile or any required database is unavailable.
- Attempt every database initializer and preserve the full component status for
  diagnostics.
- Show one visible, sanitized startup error and return a failure exit status.
- Preserve shader, renderer, window, and application-context cleanup on every
  outcome.
- Keep the decision logic pure and independently testable without SDL or bgfx.

## Non-goals

- Do not add startup retry, degraded/read-only mode, or a fatal in-app scene.
- Do not change database schemas, initialization order, migration ownership, or
  the profile manager.
- Do not expose filesystem paths, SQLite diagnostics, profile names, or other
  private implementation details in the user-facing message.
- Do not address mid-session persistence failures in this remediation. Those
  require a separate save-outcome design.

## Considered Approaches

### 1. Pure startup coordinator with presentation adapters — selected

A small `application_startup` module owns the readiness decision and invokes
injected database initialization, fatal reporting, and ready-runtime callbacks.
`main.cpp` supplies the real database initializer and SDL reporter. This makes
the allow/deny decision testable and prevents `main.cpp` from reconstructing
startup policy.

### 2. Inline early returns in `run()`

This is mechanically smaller, but the critical profile/database/runtime
ordering would remain embedded in the large bgfx event-loop translation unit.
Tests could cover the database aggregate but not the decision that currently
discards it.

### 3. Fatal scene or retry workflow

This could provide richer recovery UI, but it would construct more application
infrastructure after required storage failed and would add lifecycle/retry
states unrelated to the safety defect. A native error dialog followed by a
clean exit is the narrow fail-closed behavior.

## Coordinator API

Create `src/ApplicationStartup.h/.cpp` with a platform-neutral API:

```cpp
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

`execute` owns the complete gate:

1. If `profileReady` is false, it creates a profile failure result, calls the
   reporter exactly once, skips database initialization and runtime, and
   returns `EXIT_FAILURE`.
2. Otherwise it invokes the database initializer exactly once. The initializer
   retains its existing all-components-attempted contract.
3. If the aggregate is not ready, `execute` preserves the complete status in
   the result, calls the reporter exactly once, skips runtime, and returns
   `EXIT_FAILURE`.
4. If all components are ready, it invokes the runtime body exactly once,
   performs no fatal report, and returns `EXIT_SUCCESS`.

`Dependencies` is an internal construction contract: every callback is
required, and callbacks must not throw. Operational failures are represented by
the profile-ready boolean or database status. Exception containment for
programming faults or the existing event loop is outside this remediation; the
cleanup guarantee covers every modeled status/return outcome.

The coordinator receives only the profile-ready boolean. Raw profile
diagnostics remain in `ApplicationContext` for logs and cannot accidentally
enter the sanitized message builder.

## User Messages

Messages are deterministic and owned by the coordinator.

Profile failure:

> AsoBMaShow could not initialize the active player profile. The application
> will close to protect your data. Check available storage, storage
> permissions, and the app version, then try again.

Database failure:

> AsoBMaShow could not initialize required data: Scores, Replays. The
> application will close to protect your data. Check available storage,
> storage permissions, and the app version, then try again.

The database list includes only failed components, in this fixed order:
`Chart Library`, `Scores`, `Replays`, `Music Library`. The string builder is
covered by exact-message tests for each individual component and a multi-failure
case.

## Runtime Integration

Split the existing `run()` responsibilities without moving the large event
loop out of `main.cpp`:

- Rename the post-startup scene/event-loop body to
  `runReadyApplication(ApplicationContext &)`. It assumes profile and database
  readiness and performs no independent startup checks.
- Change global `run()` and its declaration in `main.h` to return `int`.
- Construct `ApplicationContext` in `run()`, then call
  `application_startup::execute` with:
  - `initializeApplicationDatabases()` as the database callback;
  - a fatal reporter that logs detailed state once and calls
    `SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
    "AsoBMaShow Startup Error", ...)` using the sanitized message;
  - `runReadyApplication(context)` as the ready callback.
- Remove the profile-initialization log from the `ApplicationContext`
  constructor so the fatal reporter is the single startup diagnostic owner.
- In `runApplication()`, preserve the integer returned by `run()`, always
  release shaders/uniforms and shut down bgfx, then return that integer instead
  of unconditional success.

If the native dialog itself fails, log `SDL_GetError()` and still return
failure. `ApplicationContext` destruction continues to shut down partially
initialized score/replay helpers after the dialog is dismissed.

## Build Integration

- Add `ApplicationStartup.cpp` to the desktop/mobile `main` target in
  `src/CMakeLists.txt`.
- Add the new source path to the iOS synchronized-group
  `membershipExceptions`, as required for sources under `src`.
- Add a small `application_startup_tests` executable and CTest registration.
  It links only the coordinator implementation and needs no SDL/bgfx runtime.
- Extend the existing database-initializer test rather than creating a second
  aggregate implementation.

## Test Strategy

Add coordinator tests before production integration. The initial test target
must fail to compile until the new API exists, providing the RED boundary.

Coordinator cases:

- all ready: initializer once, runtime once, reporter never, `EXIT_SUCCESS`;
- profile unavailable: reporter once, initializer/runtime never,
  `EXIT_FAILURE`, no database status, exact sanitized message;
- each single database failure: preserved four-component status, exact failed
  label only, reporter once, runtime never, `EXIT_FAILURE`;
- score plus replay failure: fixed label order and no successful labels;
- all database failure cases preserve the aggregate unchanged for detailed
  logging.

Extend `app_database_initializer_tests` with a table over each individual
component and one multi-failure. Every case must prove that all four callbacks
run exactly once even when an earlier callback fails.

Integration verification:

- the main target compiles with `run()` returning the coordinator exit code;
- the existing full CTest suite passes;
- the iOS project contains the new source membership exception;
- source review confirms scene registration and the event loop exist only in
  the ready callback, after the coordinator gate.

## Acceptance Criteria

- A profile initialization failure shows one sanitized fatal error, performs no
  database initialization or scene/runtime work, and returns failure.
- Failure of any required database shows all and only failed component labels,
  performs no scene/runtime work, and returns failure.
- Every database initializer is attempted once for complete diagnostics.
- Successful startup enters the runtime body exactly once and returns success.
- Renderer/application cleanup runs on success and every modeled startup
  failure.
- The user-facing message contains no raw diagnostic, path, database filename,
  SQLite text, or profile identifier.
- One coordinator, rather than `main.cpp` plus the aggregate status, owns the
  readiness decision.
