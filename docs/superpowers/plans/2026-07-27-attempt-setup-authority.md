# Attempt Setup Authority Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Capture replay-affecting setup once, bind BRD setup to compact result provenance through one resolver, and preserve that resolved setup across every judged/retry/ghost/export branch.

**Architecture:** Keep BRD playback and result/IR provenance as separate durable projections. Add per-stage DP provenance plus a pure setup authority that performs all Aso comparison, stock enrichment, legacy ownership, and starting-gauge reconciliation; make judged playback carry one complete `ChartPlaybackSetup` instead of a lossy list of copied fields.

**Tech Stack:** C++20, nlohmann JSON, SQLite repositories, Beatoraja BRD codec, CMake/CTest, Xcode iOS build script.

## Global Constraints

- Stock Beatoraja JSON, keyinput events, gzip/Base64URL encoding, filenames, and existing golden fixtures remain wire-compatible.
- Raw input transitions, touch samples, lane-cover events, exact gauge snapshots, file paths, checksums, repository identity, and IR state do not enter `ScoreProvenance`.
- New local attempts derive provenance and BRD setup from one captured setup.
- A BRD attached to `Verified` or `Modified` provenance must reconcile before persistence and after load.
- Full Aso fields compare exactly; stock fields are enriched only through the central resolver.
- `LegacyUnverified` and standalone BRDs retain BRD setup authority.
- DP Normal/Flip is a per-stage provenance fact. Provenance schema v1-v4 decodes it as unknown, never as Normal; schema v5 requires it.
- Judged playback, `StartOptions`, scenes, retry, G-Battle, practice, video, and result-image paths are consumer projections, not authorities.
- Apply DP and player lane transforms exactly once to each freshly parsed chart.
- Do not reply to or resolve GitHub review threads.

---

### Task 1: Persist per-stage DP setup without invalidating old provenance

**Files:**

- Create: `src/replay/DoublePlayOption.h`
- Modify: `src/replay/ReplayPlaybackData.h`
- Modify: `src/ScoreProvenance.h`
- Modify: `src/ScoreProvenance.cpp`
- Modify: `src/ResultPersistenceModel.cpp`
- Test: `tests/score_provenance_tests.cpp`
- Test: `tests/score_provenance_db_tests.cpp`
- Test: `tests/ir_submission_snapshot_tests.cpp`

**Interfaces:**

- Produces: standalone `replay::DoublePlayOption` declaration.
- Produces: `std::optional<replay::DoublePlayOption> ScoreStageProvenance::doublePlayOption`.
- Changes: `ScoreProvenance::kSchemaVersion` from `4` to `5`.
- Consumes later: setup authority compares BRD `ChartPlaybackSetup::doublePlayOption` to the matching stage fact.

- [ ] **Step 1: Write schema, merge, fingerprint, and IR RED tests**

Add tests that construct otherwise identical chart and course results whose
only difference is stage DP Normal versus Flip. Assert:

```cpp
normalStage.doublePlayOption = replay::DoublePlayOption::Normal;
flipStage.doublePlayOption = replay::DoublePlayOption::Flip;
expect(serializeScoreProvenance(normal) != serializeScoreProvenance(flip));
expect(result_persistence::resultFingerprint(normalResult) !=
       result_persistence::resultFingerprint(flipResult));
expect(ir::captureIrSubmissionSnapshot(normalResult, error)->fingerprint !=
       ir::captureIrSubmissionSnapshot(flipResult, error)->fingerprint);
```

Round-trip both values. Decode a canonical schema-v4 fixture and assert its
stage DP is `nullopt` and its schema version remains 4. Remove the DP field from
a schema-v5 document and assert decoding fails. Merge two course stages with
different DP values and assert each stage retains its own value.

Persist/load a schema-v4 result with its pre-v5 fingerprint and assert it still
loads, while a new schema-v5 result with a changed DP value and unchanged
fingerprint is invalid.

- [ ] **Step 2: Run focused tests and verify RED**

```sh
cmake --build cmake-build-debug --target score_provenance_tests score_provenance_db_tests ir_submission_snapshot_tests -j 6
ctest --test-dir cmake-build-debug -R '^(score_provenance_tests|score_provenance_db_tests|ir_submission_snapshot_tests)$' --output-on-failure
```

Expected: the new member/schema assertions fail to compile or DP-only values
serialize and fingerprint identically.

- [ ] **Step 3: Extract the neutral DP enum**

Move the existing enum, unchanged, into `src/replay/DoublePlayOption.h`:

```cpp
#pragma once
#include <cstdint>

namespace replay {
enum class DoublePlayOption : std::uint8_t { Normal = 0, Flip = 1 };
}
```

Include this small header from both `ReplayPlaybackData.h` and
`ScoreProvenance.h`; do not include raw replay event types from provenance.

- [ ] **Step 4: Implement provenance schema v5**

Add `doublePlayOptionName`/`doublePlayOptionFromName` helpers using canonical
strings `"normal"` and `"flip"`. Write the field in each stage for schema 5.
For schema 5, require a valid field. For schema 1-4, leave the optional empty.
Preserve the decoded source schema in `ScoreProvenance::schemaVersion` instead
of replacing it with `kSchemaVersion`; newly constructed provenance still
defaults to 5.

Set the stage value from a new
`ScoreProvenanceBuildInput::doublePlayOption`, and make both gameplay capture
overloads populate it. `mergeCourseProvenance` appends stages without
collapsing their DP values.

In `appendProvenance`, append the optional DP enumeration only when
`provenance.schemaVersion >= 5`. This preserves the canonical byte stream for
old schema-v4 fingerprints and binds DP for new results. Reject a schema-v5
provenance object whose stage has no value before persistence.

- [ ] **Step 5: Run GREEN and mutation checks**

Run the Step 2 commands. Temporarily omit DP from `appendProvenance`, confirm
the DP-only fingerprint tests fail, restore it, and rerun. Run
`git diff --check`.

- [ ] **Step 6: Commit the provenance capability**

```sh
git add src/replay/DoublePlayOption.h src/replay/ReplayPlaybackData.h src/ScoreProvenance.h src/ScoreProvenance.cpp src/ResultPersistenceModel.cpp tests/score_provenance_tests.cpp tests/score_provenance_db_tests.cpp tests/ir_submission_snapshot_tests.cpp
git commit -m "feat: bind double-play setup in provenance"
```

---

### Task 2: Centralize result-to-BRD setup reconciliation

**Files:**

- Create: `src/replay/ReplaySetupAuthority.h`
- Create: `src/replay/ReplaySetupAuthority.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/replay_setup_authority_tests.cpp`
- Modify: `tests/result_persistence_coordinator_v11_tests.cpp`
- Modify: `tests/replay_repository_v11_tests.cpp`

**Interfaces:**

- Produces:

```cpp
namespace replay::setup_authority {
enum class Source { CapturedAttempt, AsoExtension, Stock };
enum class Status { Resolved, Conflict, Invalid };

struct Outcome {
  Status status = Status::Invalid;
  std::optional<ChartPlaybackSetup> setup;
  std::string field;
  std::string diagnostic;
  [[nodiscard]] bool resolved() const noexcept;
};

[[nodiscard]] Outcome resolveForResult(
    ChartPlaybackSetup setup,
    const result_persistence::ChartScoreWrite &score,
    int expectedKeyMode,
    Source source,
    bool carriedStartingGaugeAllowed);
}
```

- Consumes: per-stage optional DP from Task 1.
- Replaces: private `bindReplaySetupToProvenance` and its gauge helpers in `ReplayRepositoryResultRecords.cpp`.

- [ ] **Step 1: Write the pure resolver RED matrix**

Port every field covered by the current private binder into a table-driven
authority test. Include chart MD5/SHA-256, key mode, LN mode, initial gauge,
gauge profile/shift/lower bound, P1/P2 option and seeds, DP, BMS RANDOM
values/seed/PRNG, assist, ruleset/revision, playback rate/mode, candidate,
judge scale, club mode, and deterministic starting gauge.

For each field, mutate only that BRD value and assert Aso returns `Conflict`
with the field name. Add these source cases:

- stock source enriches fields absent from stock while preserving stock-owned
  values and the application LN mode;
- `LegacyUnverified` returns the BRD unchanged;
- schema-v4 DP `nullopt` does not compare DP;
- schema-v5 Normal/Flip mismatch conflicts;
- carried course gauge is allowed only when requested.

- [ ] **Step 2: Add creation-time persistence RED tests**

In coordinator tests, make a valid chart attempt and valid course attempt,
mutate only BRD DP, then call save. Assert `SaveState::InvalidAttempt`, no
reservation, no file, no compact result, and no IR draft. Repeat with one
non-DP setup field to prove creation and load use the same complete contract.

In repository tests, mutate an encoded Aso BRD's coherent stock+extension DP
while leaving its compact result unchanged; update the file hash/size reference
so byte integrity passes. Assert `IntegrityConflict`, proving semantic binding
rather than checksum alone detects substitution.

- [ ] **Step 3: Run resolver/coordinator/repository tests and verify RED**

```sh
cmake --build cmake-build-debug --target replay_setup_authority_tests result_persistence_coordinator_tests replay_repository_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_setup_authority_tests|result_persistence_coordinator_tests|replay_repository_tests)$' --output-on-failure
```

Expected: the authority target is absent and both DP mismatch paths are
accepted.

- [ ] **Step 4: Move reconciliation into the pure authority**

Move `deterministicStartingGaugeState`, `equivalentStartingGaugeState`, and the
entire binder into `ReplaySetupAuthority.cpp`. Expand it to validate identity,
key mode, and LN mode as well as existing fields. Normalize play/assist options
exactly as the old binder does. Compare DP only when the matching provenance
stage has a value. Preserve the existing Aso, stock-enrichment,
`LegacyUnverified`, and carried-gauge semantics byte-for-byte.

Return an owned resolved setup; never partially mutate the caller's setup on
failure. Map `ReplayStageDecodeSource::AsoExtension/Stock` to the new source at
the repository boundary.

- [ ] **Step 5: Use the same resolver before save and after load**

In `validateCompletedAttempt` and every course-stage loop, call
`resolveForResult(..., Source::CapturedAttempt, ...)` before encoding or
creating a reservation. In repository chart/course load, replace the private
binder and separate identity/LN checks with `resolveForResult`; move the
resolved value back into decoded playback only after success. Filename/stem
validation remains a repository concern after semantic resolution.

- [ ] **Step 6: Run GREEN and commit**

Run the Step 3 commands, the existing replay file-store tests, and
`git diff --check`. Commit:

```sh
git add src/replay/ReplaySetupAuthority.h src/replay/ReplaySetupAuthority.cpp src/CMakeLists.txt CMakeLists.txt src/repositories/ReplayRepositoryResultRecords.cpp src/ResultPersistenceCoordinator.cpp tests/replay_setup_authority_tests.cpp tests/result_persistence_coordinator_v11_tests.cpp tests/replay_repository_v11_tests.cpp
git commit -m "refactor: centralize replay setup authority"
```

---

### Task 3: Capture once and make judged playback retain complete setup

**Files:**

- Modify: `src/analysis/JudgedPlaybackData.h`
- Modify: `src/analysis/JudgedPlaybackContext.h`
- Modify: `src/replay/LegacyReplayPlaybackAdapter.cpp`
- Modify: `src/PlayOptionUtils.h`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/replay_playback_adapter_tests.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`

**Interfaces:**

- Produces: `JudgedPlaybackData::setup` as the future single semantic setup member; Task 6 removes temporary compatibility fields after all consumers move.
- Produces: `capturePlaybackSetupAtPlayStart(const StartOptions &, const bms_parser::ChartMeta &, const gameplay::GameplayRulesetPolicy &) -> replay::ChartPlaybackSetup`.
- Changes: `captureScoreProvenanceAtPlayStart` consumes the captured setup for all overlapping fields.
- Consumes: Task 1 provenance DP and Task 2 setup contract.

- [ ] **Step 1: Write capture and adapter invariant RED tests**

Build a nondefault DP setup containing P1/P2 random options, BMS RANDOM branch,
assist, nondefault gauge shift/profile, playback, ruleset, candidate, starting
gauge, judge scale, and club mode. Assert both legacy and materialized adapters
retain one setup equal to the raw setup.

Build a play-start fixture and assert the captured setup and generated
provenance reconcile successfully through Task 2 without field patching. Make
DP Flip and assert the raw recorded setup, judged setup, and provenance stage
all report Flip.

Build retry data from schema-v5 provenance without a BRD and assert its judged
setup restores Flip. Build from schema-v4 provenance and assert the provenance
stage remains unknown while runtime uses the documented Normal fallback.

- [ ] **Step 2: Run focused tests and verify RED**

```sh
cmake --build cmake-build-debug --target replay_playback_adapter_tests gameplay_playback_startup_tests result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_playback_adapter_tests|gameplay_playback_startup_tests|result_recall_builder_tests)$' --output-on-failure
```

Expected: judged setup does not exist and DP is lost by adapters/provenance
retry.

- [ ] **Step 3: Replace judged setup duplicates with one setup value**

Add `replay::ChartPlaybackSetup setup;` to `JudgedPlaybackData`. Keep the
duplicate RANDOM, P1/P2 option, assist, and gauge fields temporarily so this
task remains buildable while Tasks 4 and 5 move consumers. Populate those
compatibility fields only through one private `copyCompatibilitySetupFields`
helper, never with another call-site field list. Task 6 removes the helper and
fields. Keep `chartMeta`, judgement/result facts, and touch/lane-cover
presentation data. Keep `PlaybackAnalysisContext` only for proof that cannot
be derived from `ChartPlaybackSetup` (the full ruleset descriptor, optional
explicit starting percentage, and policy/window evidence); it must not repeat
semantic setup.

Update `makeAdapterBase` to assign:

```cpp
adapted.setup = playback.setup;
```

then call the temporary compatibility helper once. Update the focused judged
helpers exercised by this task to read `replay.setup`; the remaining scene and
export consumers move in Tasks 4 and 5.

- [ ] **Step 4: Capture setup once in gameplay**

Add `capturedAttemptSetup` beside `attemptProvenance` in `GamePlayScene`.
Populate it once in each constructor after the gameplay policy is built.
Generate provenance from the captured setup plus proof-only facts such as
judge windows, input devices, auto/practice, and eligibility policy.

At `beginReplayRecording`, initialize all three projections by assignment:

```cpp
analyticsReplay.setup = capturedAttemptSetup;
recordedReplay.setup = capturedAttemptSetup;
recordedPlaybackReplay.setup = capturedAttemptSetup;
```

Then add only runtime/raw facts: exact starting gauge state, effective initial
lane cover, lane-cover enabled state, and reserved event capacity. Do not copy
semantic fields from `StartOptions` a second time. Until Task 6 removes the
compatibility fields, call `copyCompatibilitySetupFields` after each judged
setup assignment.

- [ ] **Step 5: Update provenance-only retry projection**

In `retrySourceFromProvenance`, populate `result.setup` from the matching stage
and top-level provenance in one block. Set DP from the stage when known; use
Normal only as the explicit runtime fallback for schema-v1-v4 unknown. Keep the
optional provenance fact empty so saved data never claims Normal was known.

- [ ] **Step 6: Run GREEN and commit**

Run the Step 2 commands plus `score_provenance_tests` and `git diff --check`.
Commit:

```sh
git add src/analysis/JudgedPlaybackData.h src/analysis/JudgedPlaybackContext.h src/replay/LegacyReplayPlaybackAdapter.cpp src/PlayOptionUtils.h src/scene/play/GamePlayStartOptions.h src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp tests/replay_playback_adapter_tests.cpp tests/gameplay_playback_startup_tests.cpp tests/result_recall_builder_tests.cpp
git commit -m "refactor: retain complete judged replay setup"
```

---

### Task 4: Route retry, G-Battle, course, ghost, and practice through retained setup

**Files:**

- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ChartViewerScene.h`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/practice/PracticeLaunchRequest.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/practice_launch_tests.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`

**Interfaces:**

- Consumes: `JudgedPlaybackData::setup` from Task 3.
- Produces: one `applyJudgedPlaybackSetupToStartOptions(StartOptions &, const JudgedPlaybackData &)` adapter used by retry/course/G-Battle launch paths.
- Produces: `ViewerReplayPlayOptions` and `viewerReplayPlayOptions(const JudgedPlaybackData &)` in `ChartViewerScene.h`, containing DP plus P1/P2 options and seeds.

- [ ] **Step 1: Write branch-level RED tests**

Add behavior tests for:

- saved-result retry with the BRD deleted restores schema-v5 Flip provenance;
- G-Battle retry records and retries Flip using judged setup alone;
- a course judged stage copies its own DP option into stage `StartOptions`;
- practice launch copies retained Flip;
- Chart Viewer ghost installation sets viewer DP to retained Flip before
  applying P1/P2 options.

For Chart Viewer, add a pure
`viewerReplayPlayOptions(const JudgedPlaybackData&) -> ViewerReplayPlayOptions`
helper returning DP plus P1/P2 options and seeds, and test it; the scene must
call it rather than resetting DP to Normal.

- [ ] **Step 2: Run focused tests and verify RED**

```sh
cmake --build cmake-build-debug --target gameplay_playback_startup_tests practice_launch_tests result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^(gameplay_playback_startup_tests|practice_launch_tests|result_recall_builder_tests)$' --output-on-failure
```

Expected: at least provenance-only retry, judged course/G-Battle, and viewer
setup assertions report Normal.

- [ ] **Step 3: Replace branch-specific field mapping**

Make `applyJudgedPlaybackSetupToStartOptions` copy gauge, P1/P2, DP, LN mode,
assist, playback, candidate, club, judge scale, starting gauge, and ruleset from
the retained setup. Purpose-specific callers may override gauge/pacemaker for
G-Battle or practice after this adapter, but they may not reconstruct chart
branch or topology fields.

Replace `resultRetryDoublePlayOption`, the raw-plus-judged G-Battle helper, and
course stage field copies with this adapter. Saved-result retry uses raw setup
when present and provenance-derived judged setup otherwise; both feed the same
adapter.

- [ ] **Step 4: Preserve DP in Chart Viewer and practice**

In `applyGhostReplayData`, set `viewerDoublePlayOption` from
`replayData.setup.doublePlayOption`, then set P1/P2 options from that same
setup. Remove the unconditional Normal assignment. On failure restore all
previous viewer setup fields atomically. Practice launch continues using the
viewer state, which now represents the installed ghost's complete setup.

- [ ] **Step 5: Run GREEN and commit**

Run the Step 2 commands, build `main`, run `git diff --check`, and commit:

```sh
git add src/scene/play/GamePlayStartOptions.h src/scene/play/GamePlayScene.cpp src/scene/ResultScene.cpp src/scene/MainMenuScene.cpp src/scene/ChartViewerScene.h src/scene/ChartViewerScene.cpp src/practice/PracticeLaunchRequest.cpp tests/gameplay_playback_startup_tests.cpp tests/practice_launch_tests.cpp tests/result_recall_builder_tests.cpp
git commit -m "fix: preserve resolved setup across replay launches"
```

---

### Task 5: Make every judged reparse apply the retained DP setup once

**Files:**

- Modify: `src/PlayOptionUtils.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `tests/replay_playback_adapter_tests.cpp`

**Interfaces:**

- Consumes: `JudgedPlaybackData::setup` and `prepareReplayChart(const JudgedPlaybackData&)` from Task 3.
- Guarantee: every fresh judged reparse applies BMS RANDOM, DP, P1/P2 options, and LN mode from the same setup exactly once.

- [ ] **Step 1: Write a DP reparse RED test**

Create a small 10-key or 14-key DP chart fixture with distinguishable P1/P2
lane contents. Build judged playback whose retained setup is Flip. Call
`prepareReplayChart(path, judged, cancelled)` and assert lane ownership is
swapped once. Add P1/P2 options and assert their resulting lane order is
applied after Flip, not before and not twice.

Call `prepareReplayChart(path, judged, cancelled)` through the same function
used by course video and result-image stage loops and assert the prepared
identity/order. Keep the existing single-chart video behavior unchanged.

- [ ] **Step 2: Run the adapter test and verify RED**

```sh
cmake --build cmake-build-debug --target replay_playback_adapter_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_playback_adapter_tests$' --output-on-failure
```

Expected: judged preparation applies only P1/P2 options and leaves the chart
unflipped.

- [ ] **Step 3: Route exporters through the common judged preparation**

Ensure `prepareReplayChart(const JudgedPlaybackData&)` parses from
`judged.setup.random*`, validates stored chart identity, applies DP, then P1/P2,
then effective LN mode. Remove any export-local reconstruction or Normal
fallback. Course video and result-image exporters must call this helper for
each fresh stage parse; they must not reapply transforms to a chart already
prepared from that same parse.

- [ ] **Step 4: Run GREEN and commit**

Run the Step 2 commands, build `main`, run `git diff --check`, and commit:

```sh
git add src/PlayOptionUtils.h src/ReplayVideoExporter.cpp src/ResultImageExporter.cpp tests/replay_playback_adapter_tests.cpp
git commit -m "fix: preserve replay setup in export reparses"
```

---

### Task 6: Audit all setup bridges and verify the branch

**Files:**

- Inspect: every file returned by `rg -l 'ChartPlaybackSetup|JudgedPlaybackData|StartOptions|ScoreProvenance' src tests`
- Modify: only files with a confirmed remaining field-by-field setup bridge or regression.
- Test: all named replay/provenance/persistence targets in Task 6 Step 4.

**Interfaces:**

- Consumes: all prior tasks.
- Produces: no unresolved setup conversion outside capture, provenance serialization, codec transport, central authority, and the shared judged/launch adapters.

- [ ] **Step 1: Run a source-boundary audit**

Use `rg` to enumerate every conversion among the four setup-bearing models.
Classify each as capture, durable serialization, codec transport, authority
resolution, or consumer projection. Replace any remaining consumer-side list
of three or more overlapping setup assignments with the shared adapter. Pay
special attention to course stage transitions, retry-new-pattern, G-Battle,
practice ghosts, result recall, video, image export, and replay summaries.

- [ ] **Step 2: Remove the temporary judged compatibility projection**

After `rg` confirms no production consumer reads the duplicate judged RANDOM,
P1/P2 option, assist, or gauge members, delete those members and
`copyCompatibilitySetupFields`. Build every focused target listed in Step 4;
any compiler error identifies a remaining consumer that must read
`JudgedPlaybackData::setup` through the shared adapter.

- [ ] **Step 3: Add RED tests for each confirmed gap**

For every concrete gap, add a focused test that changes only DP or one other
setup field and demonstrates the loss/conflict. Run that test to record RED,
then make the smallest adapter-based correction and rerun GREEN. Do not make
speculative changes without a reproducing test.

- [ ] **Step 4: Run focused replay/provenance/persistence suites**

```sh
ctest --test-dir cmake-build-debug --output-on-failure -R '^(score_provenance_tests|score_provenance_db_tests|ir_submission_snapshot_tests|replay_setup_authority_tests|beatoraja_replay_codec_tests|replay_playback_adapter_tests|gameplay_playback_startup_tests|practice_launch_tests|result_recall_builder_tests|result_persistence_coordinator_tests|result_persistence_integration_tests|replay_repository_tests|replay_file_migration_tests)$'
```

- [ ] **Step 5: Run full desktop verification**

```sh
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short --branch
```

Expected: build success, all tests pass, clean whitespace, and only intended
tracked changes remain.

- [ ] **Step 6: Run local iOS compile verification**

```sh
scripts/ios_firebase_deploy.sh --build-only
```

Do not run the upload path. Confirm the synchronized Xcode source group picks
up `ReplaySetupAuthority.cpp` and the new neutral header without membership
exceptions.

- [ ] **Step 7: Commit final audited corrections**

If Task 6 produced changes, commit only those tested corrections as:

```sh
git add -u
git commit -m "fix: close remaining replay setup gaps"
```

If Task 6 produced no changes, do not create an empty commit.
