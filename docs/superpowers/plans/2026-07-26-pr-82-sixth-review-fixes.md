# PR #82 Sixth Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three verified replay defects from the sixth PR #82 review and correct the inconsistent stock Beatoraja fixture without making its redundant lane pattern authoritative.

**Architecture:** Derive stock starting gauge state from the existing gameplay gauge definitions, pair realtime physical and logical transitions through an ordered FIFO, and recover fileless abandoned reservations only when adopting a new replay database session. Recovery deletes reservations and rebuilds sequence high-water marks in one SQLite transaction, while filesystem ambiguity retains the reservation conservatively.

**Tech Stack:** C++20, SQLite, CMake/CTest, Java fixture generator with pinned Beatoraja classes, Xcode iOS build-only verification.

## Global Constraints

- Keep Beatoraja `randomoption` and its seed authoritative for stock replay chart reconstruction.
- Do not change the BRD extension schema, replay path grammar, or replay database schema version.
- Reclaim reservations only on a newly opened profile database session; preserve same-session save retry idempotency.
- Retain reservations when a canonical final BRD exists or filesystem status is ambiguous.
- Do not modify deferred Beatoraja slot relocation or slot-selection UI.
- Do not reply to or resolve GitHub review threads without explicit authorization.
- Use `scripts/ios_firebase_deploy.sh --build-only`; do not upload a build.

---

### Task 1: Stock Gauge Start and Fixture Consistency

**Files:**
- Modify: `tests/beatoraja_replay_codec_tests.cpp`
- Modify: `tests/fixtures/replay/BeatorajaFixtureGenerator.java`
- Modify: `tests/fixtures/replay/beatoraja-chart.brd`
- Modify: `tests/fixtures/replay/beatoraja-course.brd`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`

**Interfaces:**
- Consumes: `gaugeInitialValue(GaugeType, GaugeProfile)`.
- Produces: stock `ChartPlaybackSetup::startingGaugePercent` derived after key-mode gauge-profile resolution.

- [ ] **Step 1: Add the failing stock survival-gauge regression**

In the stock chart fixture assertions, require:

```cpp
expectEqual(value.setup.startingGaugePercent, 100.0F,
            "stock HARD gauge starts at one hundred percent");
```

- [ ] **Step 2: Run the codec target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6
ctest --test-dir cmake-build-debug -R '^beatoraja_replay_codec_tests$' --output-on-failure
```

Expected: the new assertion reports 20% instead of 100%.

- [ ] **Step 3: Derive the stock gauge start from resolved profile data**

After `decodeStage()` resolves the stock key mode and sets the stock-only
gauge profile, assign:

```cpp
decoded.data.setup.startingGaugePercent = gaugeInitialValue(
    decoded.data.setup.initialGaugeType, decoded.data.setup.gaugeProfile);
```

Use the existing gameplay gauge definition rather than duplicating gauge
constants in the codec.

- [ ] **Step 4: Run the codec target and verify GREEN**

Run the command from Step 2. Expected: the codec test passes.

- [ ] **Step 5: Correct and regenerate the stock fixtures**

Change `BeatorajaFixtureGenerator.stage()` to derive
`laneShufflePattern` from the same option and `optionSeed` with helpers that
mirror pinned Beatoraja's lane modifiers:

- MIRROR returns a null display row because upstream marks it non-displayable.
- RANDOM removes source lanes 0 through 6 using `java.util.Random.nextInt()`
  and appends scratch lane 7 unchanged.
- R-RANDOM chooses rotation direction and start using the two upstream
  `nextInt()` calls and appends scratch lane 7 unchanged.

Regenerate both BRD fixtures with deterministic gzip headers (`gzip -n`) and
update the SHA-256 checksum comments in the codec test.

- [ ] **Step 6: Verify the corrected fixture contract**

Run the codec target again. Expected: the fixture decodes with the same option,
seed, key records, and 100% HARD start.

---

### Task 2: Ordered Realtime Scratch Reversal

**Files:**
- Modify: `tests/logical_gameplay_input_tests.cpp`
- Modify: `src/input/RealtimePhysicalInputRouter.h`
- Modify: `src/input/RealtimePhysicalInputRouter.cpp`

**Interfaces:**
- Replaces: `std::optional<RealtimePhysicalInputTransition> pendingTransition_`.
- Produces: FIFO pending transitions consumed one-for-one by `emitApplied()`.

- [ ] **Step 1: Add the failing realtime reversal regression**

Create clockwise and counter-clockwise bindings on separate controller
buttons. Press clockwise, then press counter-clockwise without releasing
clockwise. Require output to contain:

```cpp
{
    {.type = RealtimePhysicalInputTransitionType::Press,
     .lane = 7,
     .backSpin = false,
     .replayControl = LogicalControlKind::ScratchClockwise},
    {.type = RealtimePhysicalInputTransitionType::Release,
     .lane = 7,
     .backSpin = true,
     .replayControl = LogicalControlKind::ScratchClockwise},
    {.type = RealtimePhysicalInputTransitionType::Press,
     .lane = 7,
     .backSpin = false,
     .replayControl = LogicalControlKind::ScratchCounterClockwise},
}
```

The two reversal edges must retain the second input's source timestamp.

- [ ] **Step 2: Run the logical input target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^logical_gameplay_input_tests$' --output-on-failure
```

Expected: only the original press is published; both reversal edges are
missing.

- [ ] **Step 3: Replace the pending scalar with an ordered container**

Use `std::deque<RealtimePhysicalInputTransition>` in the router. `prepare()`
pushes to the back; `emitApplied()` returns when empty, otherwise moves the
front transition, pops it, applies the logical replay control, and emits it.

- [ ] **Step 4: Run the logical input target and verify GREEN**

Run the command from Step 2. Expected: the ordered reversal regression and all
existing logical input tests pass.

---

### Task 3: Startup Reservation Recovery

**Files:**
- Modify: `tests/replay_repository_v11_tests.cpp`
- Modify: `src/repositories/ReplayRepository.cpp`

**Interfaces:**
- Produces: private `recoverReplayFileReservations(sqlite3 *, const std::filesystem::path &) -> bool` in the repository session owner.
- Consumes: `replay::pathForStem`, the opened profile root, `replay_files`, `replay_file_reservations`, and `replay_stem_sequences`.

- [ ] **Step 1: Add failing abandoned-reservation reopen regressions**

Add one test that reserves indexes 0 and 1 without writing files, shuts down,
reopens the repository, and expects a new attempt to receive index 0. Add a
second test that installs the index-0 final path, leaves index 1 fileless,
reopens, verifies the index-0 attempt is still `AlreadyReserved`, and expects a
new attempt to receive index 1.

- [ ] **Step 2: Run the repository target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_v11_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_repository_v11_tests$' --output-on-failure
```

Expected: reopened allocation continues at index 2 because abandoned rows and
the sequence high-water mark remain.

- [ ] **Step 3: Implement transactional recovery**

Under `BEGIN IMMEDIATE`, select resultless reservation rows. For each row:

1. reconstruct and compare its canonical `pathForStem()` path,
2. inspect the final entry with `symlink_status`,
3. delete the row only when the canonical final path is definitely absent.

Then replace the sequence table contents using the maximum history index per
stem across `replay_files` and retained reservations. Any statement,
filesystem-safety, or commit error that is not a definite missing entry keeps
the row or rolls back the transaction.

- [ ] **Step 4: Invoke recovery only for newly opened database sessions**

Call the recovery function after schema creation/migration succeeds and before
the candidate connection is adopted in `BindDatabasePath()` and
`EnsureSessionDatabaseLocked()`. Do not run it for the already-open same-path
fast path.

- [ ] **Step 5: Run the repository target and verify GREEN**

Run the command from Step 2. Expected: abandoned trailing indexes are reused,
installed final paths remain reserved, and existing concurrency/idempotency
tests pass.

---

### Task 4: Verification and PR Update

**Files:**
- Verify and commit the planned files only.

**Interfaces:**
- Consumes: all three fixes and corrected fixture.
- Produces: verified branch commits pushed to PR #82.

- [ ] **Step 1: Run focused regressions together**

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests logical_gameplay_input_tests replay_repository_v11_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(beatoraja_replay_codec_tests|logical_gameplay_input_tests|replay_repository_v11_tests)$' --output-on-failure
```

Expected: 3/3 focused tests pass and `main` builds.

- [ ] **Step 2: Run the complete desktop suite**

```bash
ctest --test-dir cmake-build-debug -j 6 --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 3: Run the iOS compile check**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`; no archive or upload occurs.

- [ ] **Step 4: Audit scope and worktree state**

```bash
git diff 7c7b4f3b...HEAD -- src/replay/ReplayFileActionService.cpp
git diff --check
git status --short
```

Expected: no slot-relocation diff, no whitespace errors, and only intended
commits on the feature branch.

- [ ] **Step 5: Push the branch**

```bash
git push origin feature/file-based-replays
```

Do not reply to or resolve review threads.
