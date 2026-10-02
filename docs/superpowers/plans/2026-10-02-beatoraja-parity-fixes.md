# Beatoraja Skin Parity Fixes Implementation Plan

> **For agentic workers:** Use focused parallel implementation for the independent Lua and renderer domains; the coordinating agent owns selection and course results and serializes all builds. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Fix and verify all nine findings from the October 2 compatibility audit.

**Architecture:** Preserve the existing host, decoded destination, and scene-property boundaries. Match the pinned Java implementation at the point each behavior originates, with shared course-result logic for interactive and video output.

**Tech Stack:** C++23, LuaJIT, CMake/Ninja, CTest; local beatoraja Java source as reference.

**Spec:** `docs/skin-compat/2026-10-02-beatoraja-parity-audit.md`

## Global Constraints

- Work in the current checkout and branch; do not create worktrees.
- External skins and Lua file persistence remain unchanged; no LITONE-specific tests.
- One Ninja/CMake build at a time; coordinator runs builds and integration checks.
- Preserve surrounding formatting; do not format whole files.
- Commit and push verified task changes to the current upstream; no deployment.

## Review Focus

- Timer off sentinels, microsecond units, repeated calls and independent closure state (Task 1).
- Event throttling boundaries, initial eligibility and argument handling (Task 1).
- Duplicate offsets, fixed/changing colors, exact endpoints and unsorted authored frames across formats (Task 2).
- Missing scores, zero notes, course graph denominators and integer fractions (Task 3).
- Failed/ordinary course stages versus Full Combo/Perfect/Max and auto/replay presentation (Task 4).

### Task 1: Lua utilities and timer factories (L1, L2)

**Files:** `src/skin/beatoraja/LuaSkinHostModules.cpp`, `LuaSkinBindingDecoder.cpp`, corresponding `tests/lua_skin_*_tests.cpp`.
**Interfaces:** Existing runtime module installation and binding decoding; no new dependency on other tasks.

- [x] Add regressions for all upstream utility exports and two identical timer factory strings with independent serial values 1000/2000. Cover off/on, timing boundaries, state changes, and discarded adapter arguments plus explicitly forwarded action arguments.
- [x] Build/run relevant `lua_skin_host_modules_tests` and `lua_skin_binding_decoder_tests`; observe missing helpers/shared factory failures.
- [x] Implement utility closures against complete `TimerUtility.java`/`EventUtility.java`; instantiate scripted timer factories per authored property.
- [x] Run both suites and any directly affected Lua runtime/session tests; expected exit 0.

### Task 2: Destination geometry and authored order (R1–R3)

**Files:** `Skin2DRenderer.cpp`, `SkinDestinationEvaluator.cpp`, destination model and JSON/Lua/LR2 decoders as necessary; existing decoder, evaluator and draw-command tests.
**Interfaces:** Existing decoded destination model consumed by the evaluator and renderer; independent of Tasks 1/3/4.

- [x] Add regressions with audit inputs: duplicate x offset produces110, step alpha at 500 ms remains 128/255, unsorted frame easing gives x=25. Pin endpoint/fixed-color behavior and each format's authored order.
- [x] Build/run focused suites; observe old behavior fails those assertions.
- [x] Deduplicate application IDs, correct step alpha handling, and preserve upstream authored acceleration selection before timestamp sorting.
- [x] Run decoder/evaluator/draw suites; expected exit 0, updating old assertions only where the pinned source disproves them.

### Task 3: Selection score properties (S1–S3)

**Files:** `src/music_select/MusicSelectPropertyProjection.cpp`, `tests/music_select_property_projection_tests.cpp`; bridge tests if needed.
**Interfaces:** Existing projection maps consumed by MusicSelectSkinStateBridge; independent of other tasks.

- [x] Add regressions for 85–89, 115/116, 155/156 and course rates 140–145/147 using audit examples; test no-score/zero-note and fractional values.
- [x] Build/run `music_select_property_projection_tests`; observe missing properties.
- [x] Populate selected-score percentages and course graph rates with upstream denominators and sentinels.
- [x] Run projection and bridge suites; expected exit 0.

### Task 4: Course-stage clear lamps (C1)

**Files:** `src/ResultPresentationUtils.h`, `src/scene/ResultScene.cpp`, `src/ReplayVideoExporter.cpp`, existing result presentation tests.
**Interfaces:** Shared course-stage clear-rank projection used by scene and video export.

- [x] Add a table regression: Failed/ordinary clear→NoPlay, FullCombo/Perfect/Max retained; preserve relevant provenance behavior.
- [x] Build/run result presentation tests; observe blanket NoPlay regression.
- [x] Apply source-equivalent stage clear logic consistently to both output paths.
- [x] Run result presentation/skin tests; expected exit 0.

### Task 5: Integration and delivery

- [x] Review all diffs against the audit and request independent code review.
- [x] Build `main` and changed test targets in one serialized invocation; expected exit 0.
- [x] Run `ctest --test-dir cmake-build-debug --output-on-failure -j 6`; resolve failures.
- [x] Record verified outcomes in the audit, mark plan steps, commit task changes and push current upstream.

## Execution notes

- The initial eight Lua/rendering/selection regressions failed in six focused
  targets; the course-stage regression separately failed its lamp assertions.
- Review added LuaJ integer/string conversion cases and the selector clock.
  The first full suite exposed an obsolete fixture requiring empty utility
  modules; it now exercises supported pure helpers while retaining its existing
  capability restrictions.
- Distinct offset IDs preserve their existing order; duplicating Java's
  nondeterministic hash-table iteration is outside this change.
- The audit records the existing configured string-factory clock-binding
  lifetime limitation separately from these nine fixes.

Final verification: full desktop/test build passed; CTest passed 409/409; diff whitespace check passed.
