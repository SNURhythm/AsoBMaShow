# Custom object parity review — 2026-10-02

Baseline: AsoBMaShow `06e258e0`. Reference: local beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Findings and fixes

| ID | Reproduction | Change |
| --- | --- | --- |
| O1 | Play/result custom timer callbacks rejected `"1234.75"`, nil, and nonfinite values; event conditions rejected truthy `0`/empty strings and falsey nil. Selection and ordinary rendered properties already used LuaJ conversion. | Carry the gameplay policy into its bridge and apply the shared LuaJ boolean/long conversion in compatibility mode for Play, Result, and CourseResult. Retain Standard-mode checks. Exclude double +2^63 from the strict gameplay cast to signed 64-bit. |
| O2 | A timer, condition, or action throwing a Lua error aborted the Play/result frame in compatibility mode. | Match SkinLuaAccessor's defaults: failed timers become OFF, failed conditions become false, and failed actions allow subsequent frames to run. Strict mode continues to reject callback errors. |
| O3 | Custom timers lost their previous-frame values in Play/result. `main_state.set_timer` also rejected every write on those surfaces. | Retain timer values for the session. Permit writes to passive/custom timers 10000–19999, preserve timestamp zero and OFF, allow undeclared passive timers, and ignore writes to active timers. The final duplicate definition controls whether a timer is active. |
| O4 | `set_timer(4294977297, "1234.75")` rejected the ID instead of writing timer 10001. Nonfinite and unusual numeric-string values followed LuaJIT conversion. | Use the existing LuaJ int/long conversion helpers for both setter arguments. Authorization applies to the converted ID. |

Reference paths under `src/bms/player/beatoraja/`:

- `skin/lua/SkinLuaAccessor.java`: BooleanProperty, TimerProperty, and Event wrappers.
- `skin/lua/MainStatePropertyLuaApiExporter.java`: `SetTimerFunction`.
- `skin/CustomTimer.java`: retained timestamp, passive writes, and active updates.
- `skin/CustomEvent.java`: callback invocation and automatic update.
- `skin/Skin.java`: duplicate replacement and creation of undeclared passive timers.
- `skin/SkinPropertyMapper.java` and `skin/SkinProperty.java`: writable timer range.

## Verification

Generic fixtures in `tests/play_skin_session_tests.cpp` create real gameplay,
selection, result, and course-result sessions. They exercise two or three frames
and read an emitted trace outside Lua, so compatibility-mode error swallowing
cannot turn a failed Lua assertion into a passing test. Cases cover conversion,
error defaults, prior-frame active values, passive writes, zero/OFF timestamps,
duplicate replacement, writable range boundaries, and wrapped setter IDs.

Each production fix followed an observed failing regression: 29 callback/type
failures, 9 error-default failures, 12 timer-state/write failures, and 7 setter
conversion failures. The final focused session, gameplay-bridge, and Lua host
suites passed (3/3).

Independent source review identified one regression in the initial fix: photo
export could initialize the new persistent timer map and make a later live
timer-gated event fire early. Two result/course-result regressions reproduced
it. Photo evaluation now uses a map copy; video export retains the timer state
of its isolated session. The regression uses a timer with no mutable Lua closure
state, keeping the new host-map behavior distinct from existing callback effects.

Final verification:

- `cmake --build cmake-build-debug --target all -j 6`: passed, including
  the photo-state correction.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`:
  **409/409 passed** (70.13 seconds), including both photo-state regressions.
- `git diff --check`: passed.
- Independent source review found no other actionable issues in the changed
  ownership, conversion, failure-default, duplicate-definition, or test paths.

## Scope

This round covers custom-object execution and timer state in the supported
sessions. It preserves the existing deterministic traversal and event-queue
policies; it does not establish exhaustive compatibility with every external
skin or reproduce libGDX hash iteration. External skin files, Lua file
persistence behavior, and LITONE-specific tests are unchanged.

The review also considered existing Lua closure mutation during photo capture,
configured-load writes before installation of the new skin, and custom-object
iteration order. Those behaviors were not changed by this round. Copying the
host timer map does not claim to clone the Lua runtime for photo capture.
