# IR Score-Contributing Timing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Upload authentic Bokutachi PGREAT/GREAT early/late counts for LR2 classic-long-note plays without counting informational long-note-head replay judgements.

**Architecture:** Keep replay storage unchanged and derive score contribution from each event's existing cumulative EX-score snapshot. Separate timing-evidence selection from gauge-history selection inside `makeIrSubmission()`, retaining the existing result-count completeness guard.

**Tech Stack:** C++23, existing replay/result models, CMake, CTest.

## Global Constraints

- Preserve long-note-head replay judgements for replay playback.
- Do not change the replay/database schema.
- Do not alter adopted-gauge history behavior.
- Do not fabricate timing breakdowns for legacy replays without cumulative score evidence.

---

### Task 1: Count only score-contributing timing events

**Files:**
- Modify: `tests/ir_driver_tests.cpp`
- Modify: `src/ir/IrSubmission.cpp`

**Interfaces:**
- Consumes: `ReplayEvent::score`, `ReplayEvent::judgement`, and `ReplayEvent::diffMicros`.
- Produces: unchanged `IrSubmission` fields with corrected `pGreatFast`, `pGreatSlow`, `earlyPGreat`, `latePGreat`, `earlyGreat`, `lateGreat`, and `judgementTimingBreakdownAvailable` values.

- [ ] **Step 1: Write the failing classic-long-note regression test**

Add a test attempt whose replay contains a PGREAT long-note head at cumulative score 0, a PGREAT release at cumulative score 2, two normal PGREAT events advancing to 4 and 6, and a GREAT advancing to 7. Assert that the head is excluded, the release and normal notes provide the complete `3 PGREAT + 1 GREAT` breakdown, and all gauge mutations remain in gauge history.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_driver_tests$' --output-on-failure
```

Expected: `ir_driver_tests` fails because the current extractor counts the unchanged-score long-note head.

- [ ] **Step 3: Implement score-contribution filtering**

In `makeIrSubmission()`, track the greatest cumulative replay score observed before each event. A PGREAT on a `Press` or `Release` contributes only for an exact `+2` transition; a GREAT on a `Press` or `Release` contributes only for an exact `+1` transition. `Gauge`, `Mine`, and `Miss` events never provide PGREAT/GREAT timing even if a malformed snapshot has an exact transition. Apply the same filter to PGREAT fast/slow evidence. Continue validating and selecting gauge history through the existing gauge-mutation path, independently of timing contribution.

- [ ] **Step 4: Make existing timing fixtures realistic**

Populate monotonically increasing `ReplayEvent::score` values in existing positive timing tests. Add a malformed exact-transition `Gauge`/GREAT regression to prove gauge ticks remain excluded. Leave empty/legacy evidence at zero so it remains unavailable.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_driver_tests$' --output-on-failure
```

Expected: `ir_driver_tests` passes.

- [ ] **Step 6: Run regression verification**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
```

Expected: the desktop target builds, every registered test passes, and `git diff --check` prints no errors.

- [ ] **Step 7: Commit the fix**

```bash
git add src/ir/IrSubmission.cpp tests/ir_driver_tests.cpp docs/superpowers/plans/2026-07-18-ir-score-contributing-timing.md
git commit -m "fix: exclude non-scoring replay timing"
```
