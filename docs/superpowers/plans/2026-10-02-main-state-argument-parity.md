# Main-state argument parity implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline with test-driven development.

**Goal:** Continue the requested parity audit through main_state argument conversion.

**Architecture:** Reuse LuaJ conversion helpers at the host API boundary. Keep the frame-state interfaces and callback lifetime unchanged. Distinguish numeric-string recognition from conversion-to-zero when key_pressed chooses between numeric and named keys.

**Tech Stack:** C++20, LuaJIT, existing host-module tests, CMake/CTest, pinned LuaJ jar.

**Spec:** User's “keep going”; local Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`, `MainStatePropertyLuaApiExporter.java` and bundled `luaj-jse-3.0.2-custom.jar`.

## Global constraints

- Current checkout/branch; no worktrees, deployments, external skin edits, Lua persistence changes, or new LITONE-specific tests.
- Preserve surrounding formatting and run only one build per build directory.
- Commit and push verified task changes to the current upstream.

## Review focus

- Wrapped numeric IDs must reach the same property or event as their low 32 bits.
- Nil, booleans, malformed strings, and fractional event arguments use LuaJ conversion without bypassing the existing destination's authorization.
- Numeric strings in key_pressed use LuaJ recognition; tabs/newlines differ from LuaJIT numeric strings.
- Volume setters convert string inputs through LuaJ before narrowing to float.
- Existing supported IDs, key names, and callback error propagation must remain covered.

### Task 1: Property and event integer arguments

**Files:** `src/skin/beatoraja/LuaSkinHostModules.cpp`, `tests/lua_skin_host_modules_tests.cpp`.

- [x] Add host-boundary regressions for option/number/float_number/text/offset/timer/event_index/judge IDs and event_exec ID/arguments, with literal expected results.
- [x] Build and run the host-module suite; observe failures before production changes.
- [x] Reuse LuaJ int conversion in the affected main_state wrappers, preserving existing dispatch and unsupported-property errors.
- [x] Rebuild and verify the host suite.

### Task 2: Numeric-string recognition and float arguments

**Files:** The same host source/tests and `src/skin/beatoraja/LuaJValueCoercion.h` if recognition needs a shared parser result.

- [x] Compare key_pressed numeric strings and volume strings against the pinned LuaJ jar; add failing regressions.
- [x] Keep conversion and numeric recognition consistent using the shared parser; preserve existing conversion callers' zero fallback.
- [x] Run host, numeric-rendering, and session suites.

### Task 3: Review and delivery

- [x] Review the diff and adjacent affected boundaries against pinned source; reproduce and fix any actionable regressions.
- [x] Build all targets, run full parallel CTest, and check the diff.
- [x] Record findings and limits, then commit and push verified changes.

Report: `docs/skin-compat/2026-10-02-main-state-argument-review.md`.
Delivery: commit verified changes and push to the current upstream.
