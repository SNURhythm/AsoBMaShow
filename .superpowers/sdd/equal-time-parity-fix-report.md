# Equal-Time Parity Fix Report

## Scope and Baseline

- Repository: `/Users/xf/workspace/SNURhythm/AsoBMaShow`
- Baseline: `c89b63eb1752b3c0594e803a7e759530fd74a96a`
- Scope: parser-independent `GameplaySimulation`, focused tests, and the
  executed input-transaction plan only.
- Explicitly unchanged: generated parser files, `RhythmLaneInputController`,
  platform/backend code, and the approved asynchronous-gameplay design.

## Phase 1 Evidence

The authoritative `RhythmLaneInputController::pressLane` builds its candidate
array in main-lane then compensation-lane order. On a shared timeline it visits
both candidates in that order, but after the first selection it still calls
`shouldPreferCandidate` for the second. That comparator has no equal-timestamp
override: `Lowest` never replaces, `Duration` compares equal distances, and
late `Combo`/`Score` candidates can replace through their Good/Great window
rules.

At the baseline, `GameplaySimulation::selectPressCandidate` merged equal-time
lane heads in the same main-first order, then returned `false` before its
priority switch whenever the selected and next timings were equal. This made
late `Combo` and `Score` keep the main lane, diverging in selected note,
keysound identity, lane/judge data, and replay payload from the current
controller. The existing simulation-only late-priority test encoded that
shortcut, while the Task 6 single-lane matrix could not expose it.

The baseline `testReleaseSearchStopsAtPracticeEnd` called `releaseLane` on an
unpressed lane. `releaseLane` therefore returned before
`selectReleaseCandidate`, reset stats to zero, and made both bounds assertions
vacuously pass without exercising the practice-end stop or cursor update.

## Hypothesis

Removing only the simulation's equal-timestamp early return should restore the
authoritative priority-dependent result because the chronological merge already
preserves main-first visitation, and the remaining `shouldPrefer` branches
match the controller. Pressing an in-range note before the first release, then
re-pressing the lane before the second, should expose the existing release
boundary stop and monotonic cursor behavior without changing production release
semantics.

## RED

I first added independent, identical two-lane charts for the old controller and
new simulation. Both notes are at `1,000,000` microseconds, main lane 2 is
requested before compensation lane 1, and input is late-but-hittable at
`1,100,000`. The regression runs `Lowest`, `Duration`, `Combo`, and `Score` and
compares the existing complete `PressSummary`: selected and sound note identity,
judge presence/value/diff, and the complete normalized replay payload.

Before production changes:

```text
cmake --build cmake-build-debug --target gameplay_simulation_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Result: exit 8, 0/1 passed, with
`equal-time two-lane press matches current controller outcome`. `Lowest` and
`Duration` passed before the loop reached the expected late `Combo` mismatch;
the same shortcut also affected the covered `Score` row.

## Minimal Fix

- Deleted only the three-line equal-timestamp early return in
  `GameplaySimulation`'s `shouldPrefer` lambda. No priority formula, merge
  order, transaction state, sound, replay, release, or live-controller code
  changed.
- Renamed the Lowest-only equal-time test to state its actual contract and
  changed the late `Combo`/`Score` simulation-only expectation to lane 1.
- Added the real two-lane parity regression using the complete existing
  semantic summaries.
- Repaired the release-boundary test with an in-range note at `999,999`, a
  press before each release, and exact scan assertions: the first release
  examines the played note and boundary note (`2`); the repeated release
  examines none (`0`).
- Added an execution-correction note to Task 3 of the executed plan: only
  `Lowest` guarantees equal-time main-lane precedence; Task 6 parity requires
  the current controller's priority-dependent behavior and forbids an
  unconditional equal-time return. The approved design was not edited.

## GREEN and Verification

Focused GREEN after the minimal fix:

```text
ctest --test-dir cmake-build-debug -R '^gameplay_simulation_tests$' --output-on-failure
```

Result: exit 0, 1/1 passed.

Release-regression mutation check: temporarily removing the practice-end
stop/cursor block made the same focused test fail with exit 8 and
`repeated release search does not rescan the excluded practice tail`. Restoring
the block returned the focused gate to green, proving the repaired test is not
vacuous.

Focused simulation/boundary gate:

```text
cmake --build cmake-build-debug --target gameplay_simulation_tests gameplay_practice_input_boundary_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_simulation_tests|gameplay_practice_input_boundary_tests)$' --output-on-failure
```

Result: exit 0, 2/2 passed.

Full gate:

```text
ctest --test-dir cmake-build-debug --output-on-failure -j 6
cmake --build cmake-build-debug --target main -j 6
```

Results: CTest exit 0, 71/71 passed; desktop `main` build exit 0.

## Files

- `src/scene/play/GameplaySimulation.cpp`
- `tests/gameplay_simulation_tests.cpp`
- `docs/superpowers/plans/2026-07-17-gameplay-input-transaction-foundation.md`
- `.superpowers/sdd/equal-time-parity-fix-report.md`

## Self-Review

- Production scope is one removed guard; the authoritative controller remains
  untouched.
- The parity charts are separately constructed, structurally identical, and
  compared through the same complete summaries already hardened against
  identity and payload perturbations.
- Existing empty-lane behavior, LongTail rejection, search-stat reset, atomic
  state/sound intent, release semantics, and the non-authoritative boundary are
  unchanged and remain covered by the focused and full suites.
- The release test now proves both boundary reach and cursor monotonicity, and
  its mutation check fails when those production properties are removed.
- Final diff/status checks and the focused commit are recorded at handoff.

## Concerns

No functional concern remains. The focused target continues to emit the
pre-existing bgfx GNU variadic-macro warnings and duplicate static-library
linker warnings; neither affects test or build exit status. This task did not
run mobile builds because it contains no platform/backend change and the
requested gate is focused tests, full desktop CTest, and desktop `main`.
