# PR 99 Review Follow-ups Implementation Plan

> **Status:** Complete. All valid PR 99 threads were fixed, verified, and resolved.

**Goal:** Address every currently valid unresolved PR 99 review thread without changing already-correct compatibility behavior.

**Architecture:** Keep each repair at the authority boundary identified by the review: scanning persists chart metadata, practice owns prepared-chart state, presentation has distinct clocks and resource I/O, replay export snapshots the same state as live replay, and IR account lookup cannot block profile activation. Resolve only threads whose described behavior is present in the current branch; threads already fixed are resolved without an unrelated code change.

**Tech Stack:** C++23, SQLite migrations, chart/archive scanner, gameplay presentation, replay exporter, existing CTest targets and iOS build-only script.

**Spec:** PR 99 unresolved review threads and the current `feature/skin-compat` branch.

## Global Constraints

- Preserve the pinned Beatoraja-compatible skin-property behavior; do not create substitute values or skin-only validation.
- Keep changes grouped into independently testable commits and leave `vcpkg_installed/` untracked.
- Write each regression test before its corresponding production change and observe the targeted failure first.
- Resolve only PR threads verified as correct in the current code.

---

### Task 1: Chart metadata and durable-score correctness

**Files:** `src/ChartLibraryScanner.cpp`, `src/ResultPersistenceModel.cpp`, `src/ScoreProvenance.*`, `src/repositories/ScoreRepositorySchema.cpp`, and the matching scanner/provenance database tests.

- [x] Add failing coverage for deferred ordinary-chart document metadata, archive-folder document metadata, and a pre-v6 result fingerprint.
- [x] Add failing coverage for replaying the duration migration after chart metadata is rebuilt.
- [x] Capture ordinary flags by value, derive archive-folder `.txt` state, update known-chart document flags, retain the original fingerprint schema during deserialization, and defer completion of an unavailable duration backfill.
- [x] Run the focused scanner/provenance/database tests and commit the metadata/durable-data repair.

### Task 2: Practice attempt correctness

**Files:** `src/practice/PracticeConfiguration.cpp`, `src/scene/play/GamePlayScene.*`, relevant practice/session tests.

- [x] Add failing coverage for short-chart START TIME, DP landmine flipping, and built-in presentation practice start.
- [x] Ensure practice modifiers operate on a pristine retry chart or are not applied twice for Same Pattern.
- [x] Run focused practice/session tests and commit the practice repair.

### Task 3: Presentation clocks and image resources

**Files:** `src/scene/play/GamePlayScene.*`, `src/scene/play/PlayfieldChartVisualModel.cpp`, `src/view/ImageView.*`, `src/view/ImageFileDecoder.cpp`, associated tests.

- [x] Add failing coverage for archive/SAF-aware stage-image availability, non-ASCII WebP fallback paths, and the bounded song-information representation.
- [x] Keep the note-display offset out of global skin/event clocks and persist auto-adjusted note timing at a bounded attempt lifecycle point.
- [x] Run focused image/playfield tests and commit the presentation repair.

### Task 4: Replay-export authority parity

**Files:** `src/ReplayVideoExporter.cpp`, replay-export/playfield tests.

- [x] Verify the single-chart and course exporter authority gaps against live replay initialization.
- [x] Populate the same snapshot authority used by live replay in both single-chart and course export paths while preserving the course no-speed rule.
- [x] Run focused replay-export tests and commit the export repair.

### Task 5: Non-blocking IR account refresh and thread resolution

**Files:** `src/context.h`, IR tests, this plan.

- [x] Add failing coverage that profile activation schedules rather than synchronously performs authenticated account lookup.
- [x] Publish the first successful account name asynchronously with a stale-refresh guard and preserve the source empty branch.
- [x] Run focused IR tests plus build verification, commit the repair, resolve every verified thread (including already-fixed threads), push the branch, and mark this plan complete.

## Review findings already present in the branch

- `PRRT_kwDOLMjYBc6ae647`: `readChartMetaRecord` already starts the wrapper fields at `kChartMetaColumnCount - 1`.
- `PRRT_kwDOLMjYBc6ae65R`: the practice controller already initializes and displays `OPTION-1P` from `SkinMenuInputs::random1P`.
